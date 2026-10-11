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
 * NOT RESPONSIBLE FOR: deciding anything (no activities, jobs, errands or
 * boards), moving the body (the EntityHandle is a link to entities.c, and
 * this module never touches it), spawning/despawning, or persistence. It
 * stores the links the later phases will fill in — personalBoard, engagedJob,
 * dwellLeft, panicMask — and nothing else.
 *
 * Pure: no SDL, no allocation, no time source. agentsDecay clamps dt to
 * AGENT_MAX_DT like entitiesUpdate clamps to ENTITY_MAX_DT, so a long stall
 * advances a bounded amount and a given (create sequence, dt sequence) always
 * produces a byte-identical registry.
 */

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
 * role, and how low it must fall before that need counts as panicking. */
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
	/* Agency links the job/errand layers fill in later; -1 = none. */
	int personalBoard;
	int engagedJob;
	float dwellLeft;
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
 * a phase reads it at its start and again at its end. */
typedef struct AgentStats {
	uint32_t agentDecisions;
	uint32_t pathfindCalls;
} AgentStats;

/* Reset a registry to empty. Also restarts the id stamp, which is what makes
 * a fresh run byte-identical. NULL is a no-op. */
void agentsInit(AgentRegistry *r);

/* Claim the lowest free slot and initialise it: content needs, zeroed
 * skills and flaws, no body, no board, no job, zero dwell, and panicMask
 * taken from the freshly initialised needs. Returns AGENT_INVALID when the
 * registry is full or NULL — the registry is then unchanged.
 *
 * WARNING — needSetInit starts every panic channel at 1.0, so a new agent
 * is born panicking on all six needs (see the needSetInit note in
 * agents/need.h): wantUrgency is NEED_BANDS - 1 for every want and never
 * recovers, because the decaying need crosses its panic threshold before the
 * panic level bleeds to zero. This call follows the spec as written; a spawn
 * path is responsible for clearing the panic channel at birth. */
AgentHandle agentCreate(AgentRegistry *r, int role);

/* Borrow a live agent, or NULL for a NULL registry, an out-of-range handle
 * or a dead slot. The pointer is invalidated by agentDestroy on that slot. */
Agent *agentGet(AgentRegistry *r, AgentHandle h);

/* Release a slot: the `alive` flag clears, the slot is scrubbed to zero so a
 * stale handle can never read a live agent, and liveCount drops. Destroying
 * a dead or out-of-range handle is a no-op. */
void agentDestroy(AgentRegistry *r, AgentHandle h);

/* Advance every LIVE agent one tick: needDecay with the role's multipliers,
 * then needPanicDecayRole with the role's thresholds, then refresh
 * panicMask. dt is clamped to AGENT_MAX_DT and a non-positive or NaN dt is a
 * no-op. An agent whose role has no profile row is skipped (see the header
 * note on why). NULL is a no-op. */
void agentsDecay(AgentRegistry *r, float dt);

/* The role's curve row, or NULL when `role` is out of range. The returned
 * pointer is to static storage — valid for the process lifetime, never
 * freed, and the same pointer on every call. */
const RoleProfile *agentRoleProfile(int role);

/* Zero the counters on read, so a phase reports the delta across its own
 * span. Phase 1 decides nothing and paths nothing, so both read 0 until the
 * activity layer lands. NULL is a no-op. */
void agentsStats(AgentStats *out);

#endif