#ifndef ISOMATA_AGENTS_AGENT_H
#define ISOMATA_AGENTS_AGENT_H

/*
 * The agent record and its registry: the thing that OWNS a need vector and
 * answers "which role is this, and what are its per-role curves". See
 * agents/need.h for the want ladder's arithmetic, which this module only
 * calls.
 *
 * MODEL. A registry is AGENT_MAX fixed slots plus an `alive[]` gate and a
 * live count — the same shape as EntityRegistry (see entities/entities.h),
 * because the same properties matter: no allocation, a handle is a slot
 * index, a freed slot can be reused, and two live agents never alias. The
 * registry is caller-owned; there is no global state and no constructor.
 *
 * The slot is the HANDLE; the `id` is the IDENTITY. `id` is stamped from a
 * monotonic counter and never reissued, so a reused slot gets a NEW id and a
 * stale id can never be mistaken for a live agent. Callers that need a
 * stable, deterministic tie-break key (job ordering, selection ordering)
 * compare `id`, never the handle — the handle is reused, the id is not.
 *
 * ROLES ARE DATA. RoleProfile is a table row: a per-need decay multiplier
 * and a per-need panic threshold. Adding a role is a row plus an enum value,
 * never a new code path — that is the whole point of the per-role curves.
 * agentRoleProfile returns NULL for an out-of-range role, so callers must
 * check; agentsDecay does, and SKIPS an agent whose role has no row rather
 * than dereferencing NULL. Roles arrive from data (a saved game, a spawn
 * table), so one bad row must cost that agent its decay for a tick, never
 * take the whole loop — and every other agent — down.
 *
 * THE TICK LIVES HERE, NOT IN THE BOARD. agentsUpdate walks the registry,
 * asks the one question the framework allows an agent to ask each tick
 * (agentShouldSwitch — idle, or a challenger that clears the band margin
 * once the dwell has expired) and then executes what it decided: claim, run
 * the lifecycle, route the body. board.c knows nothing about agents;
 * agentShouldSwitch takes two integers and a dwell for exactly that reason.
 *
 * NOT RESPONSIBLE FOR: what a job IS (agents/job.h), where jobs wait and who
 * holds them (agents/board.h — a claim is written there and nowhere else),
 * spawning/despawning, errands (task 5), or persistence.
 *
 * Pure: no SDL, no allocation, no time source. agentsUpdate clamps dt to
 * AGENT_MAX_DT like entitiesUpdate clamps to ENTITY_MAX_DT, so a long stall
 * advances a bounded amount and a given (create sequence, dt sequence) always
 * produces a byte-identical registry. The only process-wide state is the
 * AgentStats accumulator, and no decision reads it.
 */

#include "agents/board.h"
#include "agents/need.h"
#include "entities/entities.h"

#include <stdbool.h>
#include <stdint.h>

/* Registry capacity (slots). Fixed, no allocation. */
#define AGENT_MAX 256
/* agentsDecay clamps a frame's dt to at most this many seconds, so a long
 * stall advances a bounded amount. */
#define AGENT_MAX_DT 0.1f
/* An invalid / dead handle. */
#define AGENT_INVALID (-1)
/* PREEMPTION MARGIN (bands). A challenger must be this many bands MORE
 * urgent than the job an agent already holds before it is worth moving —
 * the spec's "an agent cannot be short-circuited by two near-equal
 * activities". Bands are integers, so two nearly equal jobs land in one band
 * and a one-band gap is not a reason to move at all. */
#define AGENT_PREEMPT_MARGIN 2
/* STAYING POWER (seconds). The dwell an agent gets on a job it has just
 * taken: nothing preempts it until the dwell runs out, however urgent the
 * challenger is. The spec's dwell is "MIN_DWELL or its first real yield,
 * whichever is longer"; the yield half belongs to the activity layer. */
#define AGENT_MIN_DWELL 1.5f

typedef int AgentHandle;	/* 0..AGENT_MAX-1, or AGENT_INVALID */

/* Roles are data rows, not behaviour switches (see the header note). */
typedef enum AgentRole {
	ROLE_CUSTOMER,
	ROLE_STAFF,
	ROLE_CRITTER,
	ROLE_COUNT	/* table width; agentRoleProfile(ROLE_COUNT) is NULL */
} AgentRole;

/* Fixed competency slots. Skills shorten durations and raise quality; flaws
 * are deficiencies held independently of skill, and can fail a job outright.
 * Both are plain levels today and interpreted by the activity layer later. */
typedef enum AgentSkill {
	SKILL_REPAIR,
	SKILL_SPEED,
	SKILL_SERVICE,
	SKILL_COUNT	/* array width for skills[] / flaws[] */
} AgentSkill;

/* One row of the per-role curve table: how fast each need decays for this
 * role, and how low it must fall before that need counts as panicking.
 *
 * A NEGATIVE panicThreshold reserves that need: it never panics. Panic is the
 * tier-0 survival alarm (the spec's "a dryer is on fire" reprioritises
 * everyone through survival), so every shipped row carries a real threshold
 * on NEED_SURVIVAL ONLY and -1.0f elsewhere. A role therefore tunes WHERE an
 * emptied survival need trips, not which needs are allowed to set off an
 * alarm. */
typedef struct RoleProfile {
	float decayMul[NEED_COUNT];
	float panicThreshold[NEED_COUNT];
} RoleProfile;

typedef struct Agent {
	/* Monotonic identity, never reissued (the stable tie-break key). */
	int id;
	/* AgentRole as a plain int: a role is data, so a bad row is handled
	 * by the NULL check in agentRoleProfile rather than a crash. */
	int role;
	bool alive;
	/* The want ladder's state. Initialised to content by agentCreate. */
	NeedSet needs;
	/* Competencies / deficiencies, both plain levels. */
	uint16_t skills[SKILL_COUNT];
	uint16_t flaws[SKILL_COUNT];
	/* The physical body (borrowed; this module never dereferences it). */
	EntityHandle entity;
	/* THIS AGENT'S BOARD, embedded by value: "personal" is literal, not
	 * an index into a pool of boards. AGENT_MAX boards of BOARD_MAX
	 * jobs is static memory and no allocation, which is the same trade
	 * the entity and machine registries make. boardInit gives a born
	 * agent an empty, unshared board. */
	JobBoard personal;
	/* The job this agent is running and the board it lives on:
	 * engagedJob is a slot index into `personal`, or into
	 * `boards->shared` when engagedShared is set (a handle alone cannot
	 * say which of two boards it names). JOB_INVALID when idle. */
	int engagedJob;
	bool engagedShared;
	/* Staying power: seconds left before a challenger may preempt
	 * engagedJob. Set to AGENT_MIN_DWELL when a job is taken, counted
	 * down each tick, floored at 0. */
	float dwellLeft;
	/* Tiles queued by the last route this agent's body was given, or -1
	 * when the last attempt was refused (or none has been made). Pure
	 * observability — no decision reads it. */
	int pathLen;
	/* Cached needPanicMask(&needs), refreshed by every agentsDecay. */
	int panicMask;
} Agent;

typedef struct AgentRegistry {
	Agent slots[AGENT_MAX];
	bool alive[AGENT_MAX];
	int liveCount;
	/* Next id to stamp; monotonic, never reissued, reset only by
	 * agentsInit (so a fresh run is byte-identical). */
	int nextId;
} AgentRegistry;

/* The perf counter phases report deltas from. agentsStats ZEROES on read, so
 * a phase reads it at its start and again at its end. Both counters are fed
 * by agentsUpdate (decisions taken, routes computed) and by nothing else;
 * they are diagnostics, never inputs to a decision. */
typedef struct AgentStats {
	uint32_t agentDecisions;
	uint32_t pathfindCalls;
} AgentStats;

/* Reset a registry to empty. Also restarts the id stamp, which is what makes
 * a fresh run byte-identical. NULL is a no-op. */
void agentsInit(AgentRegistry *r);

/* Claim the lowest free slot and initialise it: content needs with every
 * panic channel clear, zeroed skills and flaws, no body, no board, no job,
 * zero dwell, and panicMask taken from the freshly initialised needs. Returns
 * AGENT_INVALID when the registry is full or NULL — the registry is then
 * unchanged. A new agent is content and calm, so its wants band and order
 * normally from its first tick; see needSetInit in agents/need.h. */
AgentHandle agentCreate(AgentRegistry *r, int role);

/* Borrow a live agent, or NULL for a NULL registry, an out-of-range handle
 * or a dead slot. The pointer is invalidated by agentDestroy on that slot. */
Agent *agentGet(AgentRegistry *r, AgentHandle h);

/* Release a slot: the `alive` flag clears, the slot is scrubbed to zero so a
 * stale handle can never read a live agent, and liveCount drops. Destroying
 * a dead or out-of-range handle is a no-op. */
void agentDestroy(AgentRegistry *r, AgentHandle h);

/* Advance every LIVE agent one tick: needDecay with the role's multipliers,
 * then needPanicDecayRole with the role's thresholds (so an emptied survival
 * need raises its alarm), then refresh panicMask. dt is clamped to
 * AGENT_MAX_DT and a non-positive or NaN dt is a no-op for the decay itself.
 * An agent whose role has no profile row is skipped (see the header note on
 * why). NULL is a no-op. */
void agentsDecay(AgentRegistry *r, float dt);

/* The role's curve row, or NULL when `role` is out of range. The returned
 * pointer is to static storage — valid for the process lifetime, never
 * freed, and the same pointer on every call. */
const RoleProfile *agentRoleProfile(int role);

/* THE SWITCH DECISION — the one place an agent changes its mind, and the
 * whole anti-oscillation guarantee in four lines:
 *
 *   idle (engagedBand or challengerBand == JOB_INVALID)  -> 1 (take it)
 *   dwellLeft > 0                                         -> 0 (stay)
 *   challengerBand >= engagedBand + AGENT_PREEMPT_MARGIN  -> 1 (preempt)
 *   otherwise                                             -> 0 (stay)
 *
 * A NULL agent has nothing to switch, so it stays (0). Nothing else is
 * consulted: no per-tick re-ranking, no role curve, no need vector — the
 * bands have already folded all of that in (agents/need.h). */
int agentShouldSwitch(const Agent *a, int engagedBand, int challengerBand,
		      float dwellLeft);

/* The tick's decision half plus the execute half, over every LIVE agent:
 *
 *   1. agentsDecay (see above), so the bands below read fresh needs;
 *   2. a finished job (DONE/FAILED) is released and the agent goes idle;
 *   3. the dwell counts down;
 *   4. agentShouldSwitch decides — an idle agent takes the best job on
 *      either board, an engaged one only a challenger that clears the
 *      margin (bands decide, never which board a job sits on; ties go to
 *      the personal board);
 *   5. on a switch: the displaced job is INTERRUPTED (failed) and its claim
 *      released — re-planning is a NEW job, never a retarget of a live one —
 *      and the challenger is claimed and STARTed, which resolves the target
 *      it names and routes the body there with entityPathTo (a target that
 *      does not resolve, a void tile, or a route A* refuses DENIES the job
 *      instead, and the claim is released on the next tick);
 *   6. an engaged job advances: the walk completes on arrival, the
 *      activity step runs, and `duration` is spent as a timed WAIT before
 *      the job completes.
 *
 * `ents` is the shared entity registry the bodies live in (NULL = agents
 * with no reachable body are denied any move, never crashed into); `boards`
 * carries the one board agents share (NULL = personal boards only);
 * `outDecisions`, when given, accumulates the number of switches taken.
 * Returns this tick's switch count as well, so a caller that does not want
 * the running total still gets the delta. `dt` is clamped to AGENT_MAX_DT,
 * NaN-safe, exactly as agentsDecay and entitiesUpdate clamp theirs.
 *
 * The board clock is the CALLER's: sweep claims with boardExpire(b, now)
 * before calling, or reservations will never lapse (see agents/board.h).
 *
 * NULL registry is a no-op returning 0. Everything else is per-agent: one
 * agent's bad row or refused move never takes the loop down. */
int agentsUpdate(AgentRegistry *r, AgentBoards *boards, EntityRegistry *ents,
		 const Voxmap *map, float dt, int *outDecisions);

/* Zero the counters on read, so a phase reports the delta across its own
 * span: agentsUpdate's decisions and route computations accumulate between
 * two reads. NULL is a no-op. */
void agentsStats(AgentStats *out);

#endif
