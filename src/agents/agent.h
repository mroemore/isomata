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
 * ONE REGISTRY, AND IT IS THE WORLD'S (settled here; task 5 recorded the
 * debt this resolves). There used to be two: customers in SimWorld.agents,
 * the demo critters in LevelState.agents. Two registries meant two decay
 * passes per frame, two AgentBoards, two clocks, and — the part that
 * mattered — a "shared" board reachable only from whichever registry happened
 * to hold the staff, so exclusivity between two workers was not something
 * one pass could even express. It is settled for SimWorld.agents, and the
 * order of the reasons is the order they decided it:
 *
 *   1. THE SHARED BOARD HAS TO BE SHARED. A claim is a promise made between
 *      two agents, so the agents that contend for one job must live in one
 *      registry that hands them one board (agentsClaimFromShared). There is
 *      no arrangement in which two registries share a board honestly: one of
 *      them cannot see it, and "cannot see it" is exactly the property that
 *      makes the board private again.
 *   2. THE WORLD OWNS THE AGENTS. SimWorld already owns the entity registry
 *      their bodies live in and the machine registry their errands name
 *      (sim/brain.h), and the level holds the world by value — so one
 *      registry means one place an agent is created and one place an agent
 *      is looked up, which is what made the split expensive in the first
 *      place. The critters moved to the world registry; the staff did not
 *      move to a demo one, because a demo is not a home for a workforce.
 *   3. A HALF-MERGE IS WORSE THAN TWO HONEST REGISTRIES. So LevelState.agents
 *      and LevelState.boards are GONE: there is exactly one AgentRegistry and
 *      exactly one AgentBoards in the tree, and both live in SimWorld.
 *      level_scene.c reaches every agent through st->sim.agents and every
 *      shared board through st->sim.boards.
 *
 * ONLY STAFF PULL FROM THE SHARED BOARD (task 6; task 3 shipped the scan
 * ungated and named this as the seat). The gate is ROLE_STAFF, applied in
 * agentsUpdate's scan and in agentsClaimFromShared — the two places a shared
 * job can be picked up. A customer's laundry round belongs to its own errand
 * on its own board, and a critter's fill is its own; letting either pull
 * somebody else's work would make the board a free-for-all and would put a
 * customer on a repair, which is precisely what the errand's role gate
 * refuses one step earlier. The gate is on the ROLE, not on the job: who may
 * take work off the shared board is a staffing fact, and which jobs are on it
 * is the world's.
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
 * spawning/despawning, or persistence. Errands ARE carried here (task 5),
 * because "what this agent is trying to do" is a property of the agent; the
 * errand's STEPS are jobs on the personal board, and the world's half of a
 * step (which machine, how much money) belongs to whoever owns the world.
 *
 * Pure: no SDL, no allocation, no time source. agentsUpdate clamps dt to
 * AGENT_MAX_DT like entitiesUpdate clamps to ENTITY_MAX_DT, so a long stall
 * advances a bounded amount and a given (create sequence, dt sequence) always
 * produces a byte-identical registry. The only process-wide state is the
 * AgentStats accumulator, and no decision reads it.
 */

#include "agents/activity.h"
#include "agents/board.h"
#include "agents/errand.h"
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
 * Both are plain levels today and interpreted by the activity layer later.
 *
 * NEITHER EVER DISQUALIFIES. The spec's rule is exact — "low competency
 * never disqualifies an agent, so a bad hire can't deadlock the sim, and
 * staffing is a soft optimisation puzzle" — and it is why the competency
 * question is a FUNCTION (agentCanAttempt) rather than a comparison at every
 * call site: the rule is stated once, and a later game that wants a hard gate
 * changes one function instead of every caller. Task 6 is what writes these
 * arrays for the first time (a staff agent is hired with levels); before it
 * every agent was born at zero and no competency could be observed at all. */
typedef enum AgentSkill {
	SKILL_REPAIR,
	SKILL_SPEED,
	SKILL_SERVICE,
	SKILL_COUNT	/* array width for skills[] / flaws[] */
} AgentSkill;

/* WHAT ONE DEFICIENCY COSTS IN TIME (a multiplier on the job's own
 * duration). A flaw is a deficiency held independently of skill, and the
 * spec's "a flaw (clumsy) can make a job fail outright" has no quality or
 * failure model to fail through yet — so what a flaw does today is make the
 * work take longer, which is the half that is arithmetic rather than
 * simulation. The failing half lands when something models job QUALITY; until
 * then this constant is the whole of a deficiency's effect and is named as
 * such rather than left to look like a quality weight. */
#define FLAW_PENALTY 2.0f

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
	 * say which of two boards it names). JOB_INVALID when idle.
	 *
	 * IT ASSERTS OWNERSHIP, IT IS NOT A HINT. A handle names a SLOT,
	 * and slots are reused: a job can be retired and refilled between
	 * two ticks, so the job in that slot may no longer be this agent's.
	 * agentsUpdate therefore treats anything the slot holds that this
	 * agent has no claim on as VANISHED — dropping the handle rather
	 * than adopting the new job, which would execute somebody else's
	 * work and then release their claim on it. */
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
	/* WHEN THIS AGENT MAY NEXT DO WHAT. The catalogue's per-agent
	 * bookkeeping (agents/activity.h): "I just napped" is a fact about
	 * ONE mind, and a process-wide cooldown table would let one agent's
	 * rest suppress every other agent's. Seconds, on the agent's own
	 * clock below — this module has no time source and never reads a
	 * wall clock. */
	ActivityCooldowns cooldown;
	/* WHAT THIS AGENT IS TRYING TO DO, as data: a small fixed list of live
	 * errands (agents/errand.h), embedded by value for the same reason the
	 * personal board is — AGENT_MAX x ERRAND_MAX of static memory and no
	 * allocation. An errand never competes for attention itself; it only
	 * enqueues jobs onto the personal board below, which is where its work
	 * is arbitrated against everything else. errandActive names the one an
	 * errand-driven caller steps each tick. */
	ErrandList errands;
	/* WHAT THE WORLD OFFERS THIS AGENT, cached. Refilled by the world
	 * (a later task supplies it from the entity and machine
	 * registries); until then every field reads JOB_INVALID or zero, so
	 * place-requiring activities are simply unavailable and the
	 * in-place ones are all that can be picked. Read-only here: no
	 * decision writes it. */
	ActivityPlaces places;
	/* Seconds of ticked dt this agent has lived — the cooldown clock,
	 * advanced by agentsUpdate and read by activityMarkDone and
	 * agentBestFill. Accumulated dt, never a wall clock, so the same
	 * dt sequence always yields the same clock. */
	float clock;
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
 *      the personal board). THE SHARED BOARD IS STAFF-ONLY: for any other
 *      role the tick scans the personal board alone, so a customer or a
 *      critter cannot be pulled onto somebody else's work (see the role
 *      gate in the header note);
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
 * `ents` is the shared entity registry the bodies live in, and it is the one
 * parameter the tick cannot do without: `Agent.entity` is an EntityHandle, the
 * registry that owns it belongs to the world (the level's SimWorld), and
 * entityPathTo needs the Entity* — so the registry is passed in rather than
 * guessed at. NULL means no body is reachable, and a movement job is then
 * DENIED rather than crashed into. `boards` carries the one board agents share
 * (NULL = personal boards only);
 * `outDecisions`, when given, accumulates the number of switches taken.
 * Returns this tick's switch count as well, so a caller that does not want
 * the running total still gets the delta. `dt` is clamped to AGENT_MAX_DT,
 * NaN-safe, exactly as agentsDecay and entitiesUpdate clamp theirs.
 *
 * The board clock is the CALLER's: sweep with boardExpire(b, now) before
 * calling, or a reservation nobody converted into a running job will never
 * lapse (see agents/board.h — a running job's claim is not swept at all, so
 * this sweep is about reservations only).
 *
 * NULL registry is a no-op returning 0. Everything else is per-agent: one
 * agent's bad row or refused move never takes the loop down. */
int agentsUpdate(AgentRegistry *r, EntityRegistry *ents, AgentBoards *boards,
		 const Voxmap *map, float dt, int *outDecisions);

/* ONE PASS OVER ONE SHARED BOARD — the staffing claim, and the reason a
 * shared board is SHARED rather than merely shared-shaped. Every live agent
 * of ROLE_STAFF that is not already running something takes the best job on
 * `shared` it may attempt, in registry order, and nothing else happens to
 * anybody: no other role is offered a job, no agent is preempted, and no
 * agent ever holds two. Returns how many claims were taken (0 for a NULL
 * registry or board).
 *
 * THE PASS OWNS THE SWEEP. `now` is the only clock a board has (agents/
 * board.h), and a reservation nobody started has to lapse inside the same
 * call that hands the freed work to somebody else — otherwise "the job went
 * back on the board" is a frame late, and an agent that died holding a claim
 * would keep the floor's work hostage until the caller's next tick.
 *
 * THE CLAIM IS A RESERVATION, NOT A START. This function claims (boardClaim,
 * the only writer of Job.claimer) and STARTs the lifecycle, so the job reads
 * JOB_CLAIMED — "reserved, not yet walking", which is the one state
 * boardExpire will free. The MOVE is the tick's: an agent engaged on a
 * reserved job completes the start on its next agentsUpdate (see
 * agentAdvanceJob's JOB_CLAIMED cell). That is why the reservation window
 * exists at all, and it is why this function needs no entity registry and no
 * map: claiming is not routing.
 *
 * ONE JOB PER AGENT IS AN INVARIANT, NOT A HOPE. An agent already engaged —
 * on either board — is skipped, so an idle staffer never stacks a second
 * job beside the errand it is running. The engaged handle is RE-VERIFIED
 * first, by the same ownership rule the tick uses: a slot index can name a job
 * that is no longer this agent's (retired and refilled between passes, or a
 * reservation that lapsed and was taken by somebody else), and acting on it
 * would execute another agent's work. A finished job is released here for the
 * same reason the tick releases it.
 *
 * The pass is the SPEC'S "N staff scan one board in one claim pass, not N
 * boards each": contention is resolved by boardClaim's exclusivity, so two
 * staff can never want the same job and both have it.
 */
int agentsClaimFromShared(AgentRegistry *r, JobBoard *shared, float now);

/* MAY `a` ATTEMPT `j` AT ALL? ALWAYS TRUE in v1, and that is the policy, not
 * an omission.
 *
 * Low competency MODIFIES work (jobDurationFor) and never disqualifies: a
 * clumsy hire takes the repair and takes twice as long, which is the spec's
 * "staffing is a soft optimisation puzzle" and the reason a bad hire cannot
 * deadlock the floor. Making this a function rather than a comparison at the
 * call sites is what makes the rule changeable: a later game that wants a hard
 * gate (a licence, a uniform, a machine only the fitter may touch) implements
 * it HERE and every caller keeps working, which is why the signature already
 * takes both the agent and the job rather than a bare competency level.
 *
 * The function is asked on every claim (agentsClaimFromShared) and the answer
 * is deliberately NOT a branch there — see the note at that call site. */
bool agentCanAttempt(const Agent *a, const Job *j);

/* HOW LONG `j` TAKES `a`: its declared duration, divided by
 * 1 + the competency the ACTIVITY DECLARES (Activity.skillGate — the repair
 * row declares SKILL_REPAIR, a wander row declares none and is unmodified),
 * then multiplied by FLAW_PENALTY when this agent holds a SKILL_SPEED flaw.
 *
 * THE DIVISION CANNOT FAIL, and the arithmetic is spelled so that it cannot:
 * the divisor is 1 + a level read from an unsigned array, so it is >= 1.0f by
 * construction — a skill of 0 divides by exactly 1.0f (the unfocused worker
 * takes the declared time), and even the largest level the record can hold
 * leaves a finite, positive duration. There is no 0 divisor, no infinity and
 * no NaN to guard for, because the formula has no way to produce one.
 *
 * An activity no row claims has no DECLARED competency, so its duration is
 * unmodified — and the framework's one place that treats an unknown activity
 * as an error already handles it: agentsUpdate's band refresh DENIES such a
 * job on sight (agents/activity.h's Review Focus #5 contract). A NULL agent
 * or a NULL job is 0.0f: there is nobody to take the work, so there is no
 * duration to report.
 *
 * This MODIFIES, never GATES: no job is ever refused because of it, which is
 * the same rule agentCanAttempt states. */
float jobDurationFor(const Agent *a, const Job *j);

/* Zero the counters on read, so a phase reports the delta across its own
 * span: agentsUpdate's decisions and route computations accumulate between
 * two reads. NULL is a no-op. */
void agentsStats(AgentStats *out);

#endif
