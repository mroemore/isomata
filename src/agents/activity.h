#ifndef ISOMATA_AGENTS_ACTIVITY_H
#define ISOMATA_AGENTS_ACTIVITY_H

/*
 * The activity catalogue: what an agent CAN do, and how urgent a job is
 * because of the needs it serves. See agents/need.h for the want ladder this
 * reads, agents/job.h for the record a row is instantiated into, and
 * agents/agent.h for the agent that owns a need vector and a board.
 *
 * AN ACTIVITY IS NEVER "PRIMARY" OR "SECONDARY". This header is the whole
 * reason that sentence is true in code rather than in prose. An activity is
 * a DECLARATION — a row saying which needs it serves, how well, how long it
 * takes, where it happens. It says nothing about who wanted it. The arcade
 * game is one row; it is reached by an errand that enqueues a job for it and
 * equally by a blocked agent picking it for itself off an empty board. Only
 * the PRODUCER differs, and the producer is never recorded here — which is
 * why jobUrgency cannot possibly order by it. A job's urgency is computed
 * from the needs it serves, so "primary" and "secondary" are emergent
 * outcomes of an agent's state, never a property of the activity.
 *
 * URGENCY IS THE MAXIMUM, NEVER THE SUM. jobUrgency folds every need a
 * activity serves and keeps the largest wantUrgency * serves product. A job
 * is as urgent as its most pressing served need, and nothing more: summing
 * would let one activity that touches four needs out-rank a survival job
 * just by being broad, which is precisely the "an activity is primary"
 * property the sum would smuggle back in through the back door. The test
 * suite pins this against a two-need row, because with only single-need rows
 * max and sum agree and the assertion would pass either way.
 *
 * THE ORDER IS EMERGENT BECAUSE THE WEIGHTS ARE NOT UNIFORM. wantUrgency is
 * banded 0..NEED_BANDS-1 and already folds in the Maslov-style tier
 * (agents/need.h), so an activity that serves a small slice of an emptied
 * tier-1 need outranks one that serves a large slice of an emptied tier-4
 * need. That inversion — ACT_NAP at FULFILMENT's expense — is the single
 * most important assertion in this module, and it is why serves[] is a
 * STRENGTH (0..1) rather than a flag.
 *
 * UNKNOWN ID IS AN ERROR, NOT A ZERO. activitiesGet returns NULL and
 * jobUrgency returns -1 for an id no row claims. Nothing in this module
 * guesses a default row, invents a zero urgency, or skips a job silently:
 * the caller logs the diagnostic and fails the job. A silently-zero urgency
 * is the worst possible failure mode here, because a real row with band 0 is
 * indistinguishable from it and the job would sit on a board forever looking
 * merely boring instead of broken. -1 is outside the 0..NEED_BANDS-1 band
 * range precisely so the two can never collide.
 *
 * PLACES ARE A PURE PREDICATE OVER A CACHED VIEW. An activity that needs a
 * place — somebody to talk to, an arcade cabinet, a machine — cannot decide
 * that for itself: the world knows where those things are and this module is
 * SDL-free, allocation-free and knows no registry. ActivityPlaces is the
 * caller's cached answer, refreshed by the world, and
 * activityPlaceSatisfied is the whole decision as a pure function of (row,
 * cached places). It is deliberately separate from the urgency arithmetic so
 * it can be tested with no world at all, and so an unavailable activity is a
 * filtered CANDIDATE rather than a failed job.
 *
 * Cooldowns are PER AGENT. ActivityCooldowns lives in the Agent record
 * (agents/agent.h), because "I just did that" is a fact about one mind and
 * sharing it would make one agent's nap suppress every other agent's. The
 * clock is the CALLER's, exactly as for a board (agents/board.h): activity.c
 * has no time source, so the same (row, places, clock) sequence always
 * produces the same choice.
 *
 * NOT RESPONSIBLE FOR: what a job IS (agents/job.h), where jobs wait or who
 * holds them (agents/board.h), deciding when an agent changes its mind
 * (agents/agent.h), or resolving a place to a world object — that is the
 * caller's, against the registry it owns.
 *
 * Pure: no SDL, no allocation, no time source, no libm. The table is
 * `static const` storage, so activitiesGet returns the SAME pointer for the
 * same id on every call and for the process lifetime.
 */

#include "agents/board.h"
#include "agents/need.h"

/* Forward declaration only: the agent record (agents/agent.h) owns a
 * cooldown store and a cached place view, so agent.h includes THIS header —
 * which is why this one must never include agent.h back. The functions below
 * that take an agent say `const struct Agent *`, which is the very same type
 * as `const Agent *`: a caller passing an Agent * compiles and links against
 * them unchanged. activity.c includes agent.h and dereferences the record. */
struct Agent;

/* The catalogue is bounded, fixed and static: ACTIVITY_MAX rows, no
 * allocation, and a cooldown slot per row so the store cannot overflow. */
#define ACTIVITY_MAX 32

/* How close another agent must be for ACT_TALK to be available (tiles). */
#define TALK_RADIUS 3

/* The served-strength scale Job.served[] is stamped in: an activity's
 * `serves[n]` of 0.6 becomes 60, and 0 means "this activity does not serve
 * slot n at all" — the same meaning jobInit gives a fresh record. */
#define ACTIVITY_SERVE_SCALE 100

/* Where an activity happens. The value is a bit set, so a row may declare
 * both; OR them. */
typedef enum ActivityFlag {
	ACT_IN_PLACE = 0,	/* no conflict, no target to walk to */
	ACT_NEEDS_PLACE = 1,	/* something in the world must be found first */
	/* NOT A FILLER. This activity is reached only by an errand that
	 * enqueues it (agents/errand.h), and agentBestFill skips it.
	 *
	 * WHY IT IS A FLAG AND NOT A COOLDOWN: "wash the laundry", "collect
	 * the wash" and "leave" are things an agent does because it set out
	 * to do them, never things an agent decides to do on a whim while
	 * standing idle — a cooldown would still let every agent eventually
	 * pick them, and a skill gate would be a lie (skills modify, they do
	 * not disqualify). Declaring it in the row is what keeps the header's
	 * promise honest: an activity is never "primary" or "secondary",
	 * because whether an agent reaches one is a property of what the
	 * agent was doing, never of the activity.
	 *
	 * It does not make the row unscored: jobUrgency still resolves it,
	 * which is what an errand's enqueued job needs, and NEEDS_PLACE still
	 * says what the row needs before it can run. */
	ACT_ERRAND_ONLY = 2
} ActivityFlag;

/* One row: a declaration of something an agent can do. Uniform shape, so a
 * new activity is copy-and-edit and the compiler catches a mistake. */
typedef struct Activity {
	int id;		/* a JobKind: one declaration, every producer */
	const char *name;	/* "PLAY_ARCADE" — intent-revealing */
	int flags;	/* ActivityFlag bits */
	float duration;	/* seconds the activity itself runs */
	float cooldown;	/* seconds before this agent may pick it again */
	float serves[NEED_COUNT];	/* -1 = not served, else 0..1 strength */
	int skillGate;	/* AgentSkill required, or -1 for no gate */
	float skillMin;	/* the level that opens the gate */
} Activity;

/* The ids this module declares rows for. Every one is a JobKind, because a
 * job's `kind` is how a row is resolved back from a live job — the two must
 * not drift apart, and the suite pins every id against JobKind. */
typedef enum ActivityId {
	ACT_WANDER = JOB_WANDER,
	ACT_TALK = JOB_TALK,
	ACT_PLAY_ARCADE = JOB_PLAY_ARCADE,
	ACT_NAP = JOB_NAP,
	ACT_TIDY = JOB_REPAIR,
	ACT_COFFEE = JOB_PAY,	/* the two-need row: see ACTIVITIES in activity.c */
	/* THE ERRAND'S OWN ROWS. A laundromat errand enqueues a job per step
	 * (agents/errand.h), and a job is only ever comparable by a band, so
	 * every kind an errand step names has to RESOLVE to a row — an
	 * unresolved id scores -1 and the tick fails the job. They are
	 * ACT_ERRAND_ONLY because an agent never picks any of them for
	 * itself. */
	ACT_USE_MACHINE = JOB_USE_MACHINE,
	ACT_COLLECT = JOB_COLLECT,
	ACT_LEAVE = JOB_LEAVE
} ActivityId;

/* WHAT THE WORLD OFFERS, AS THE CALLER SEES IT. A cached view refreshed by
 * the world, not a registry handle: this module must stay testable with no
 * world at all, so it reads three booleans-worth of answers rather than
 * resolving anything itself. JOB_INVALID means "there is none". */
typedef struct ActivityPlaces {
	int othersInRange;	/* other agents within TALK_RADIUS tiles */
	int arcadeTile;		/* a packed tile of an arcade fixture */
	int machineId;		/* a machine handle an activity could work on */
} ActivityPlaces;

/* When an agent may next pick `id`. Index is the activity id, so the store
 * is a plain fixed array with no allocation and no lookup. Every slot starts
 * at 0.0f — "available since the beginning of time", which is the honest
 * reading for a mind that has done nothing yet. */
typedef struct ActivityCooldowns {
	float readyAt[ACTIVITY_MAX];
} ActivityCooldowns;

/* Reset every cooldown so every activity is pickable. NULL is a no-op. */
void activityCooldownsInit(ActivityCooldowns *cd);

/* The row for `id`, or NULL when no row claims it. The NULL is the
 * diagnostic path the whole framework depends on (see the header note): a
 * caller that finds one must report it and fail the job it was resolving,
 * never treat the activity as absent-but-harmless. The pointer is to static
 * storage — the same pointer on every call, never freed. */
const Activity *activitiesGet(int id);

/* How many rows the catalogue holds: 0..ACTIVITY_MAX, never more. */
int activitiesCount(void);

/* Row `index` in catalogue order, or NULL outside 0..activitiesCount()-1.
 * Iteration order is table order and is stable; nothing may depend on an id
 * being at a particular index (use activitiesGet for lookups). */
const Activity *activitiesAt(int index);

/* THE URGENCY ALGORITHM. Resolves `j`'s activity by `j->kind`, walks the
 * row's serves[] and returns the LARGEST round(wantUrgency(needs, n) *
 * serves[n]) over the needs it serves — the max, never the sum (see the
 * header note). The result is a band in 0..NEED_BANDS-1.
 *
 * -1 means THE JOB COULD NOT BE SCORED: an unknown activity id, a NULL job
 * or a NULL agent. It is outside the band range on purpose, so "broken" is
 * never readable as "boring". The caller logs and fails the job. An activity
 * that genuinely serves nothing scores 0, which is a different answer from
 * -1 and means the same thing it means everywhere else in the framework:
 * nothing wants this. */
int jobUrgency(const struct Agent *a, const Job *j);

/* Instantiate `j` from the row for `actId`: kind, duration, served[] and the
 * target reset. The band's own reset to -1 (dirty) is left to the caller
 * that has an agent to compute it with, so binding and scoring stay
 * separable.
 *
 * An IN-PLACE row binds JOB_TARGET_NONE explicitly — "deliberately happens
 * where the agent stands", which the tick knows to start without a route.
 * A NEEDS_PLACE row binds no target at all (JOB_INVALID): the caller owns
 * the world and MUST bind one before the job can run, and an unbound target
 * is a hard failure rather than a silent walk to the origin. An unknown
 * `actId` is refused (false) and leaves the job untouched. */
bool activityBindJob(Job *j, int actId);

/* True when the row can happen where this agent is standing, given what the
 * world currently offers. Pure: (row, cached places) in, one boolean out,
 * with no world and no clock. A NULL row or NULL places satisfy nothing —
 * an in-place activity needs neither and reads true, because "in place" is
 * answered by the row's own flags and never by the world. */
bool activityPlaceSatisfied(const Activity *act, const ActivityPlaces *p);

/* True when the row's competency gate is open for this agent. Skills
 * MODIFY rather than disqualify (agents/agent.h), so this only ever closes
 * a gate whose `skillMin` the agent is actually below; with the shipped
 * `skillMin` of 0 every agent passes, which is deliberate — a bad hire must
 * not be able to deadlock the floor. A row with no gate, a NULL row or a
 * NULL agent is open. */
bool activitySkillOpen(const struct Agent *a, const Activity *act);

/* Stamp the cooldown: this agent has just done `id`, so it is not available
 * again until now + the row's cooldown.
 *
 * The cooldown store is the CALLER's (an ActivityCooldowns embedded in the
 * Agent), which is why it is a parameter here rather than module state: a
 * process-wide table would make one agent's nap suppress everybody's, which
 * is the opposite of per-agent bookkeeping. An unknown id is refused with a
 * stderr diagnostic and changes nothing — a cooldown for an activity that
 * does not exist is a broken catalogue row, not a slot to silently fill.
 * Returns false for that refusal, true otherwise. NULL is a no-op. */
bool activityMarkDone(ActivityCooldowns *cd, int id, float now);

/* True when `id` is off cooldown at `now`. The boundary is INCLUSIVE: at
 * exactly now + cooldown the activity is available again, so a cooldown of
 * zero never suppresses anything. An unknown id is never available. */
bool activityReady(const ActivityCooldowns *cd, int id, float now);

/* THE FILL PICK. Score every row that is off cooldown and whose place
 * requirement the cached places satisfy, and return the id of the most
 * urgent one to enqueue — JOB_INVALID when there is nothing to do.
 *
 * "Urgency" here is the band's honest reading: a fill has no agent-specific
 * pressure beyond the needs the activity serves, so the score is the
 * activity's STRONGEST served need by wantUrgency, the same fold jobUrgency
 * performs. An in-place row is available by construction, which is why the
 * demo's critters always have something to do.
 *
 * `b` is the board the fill will be enqueued on, consulted so the pick is
 * not a duplicate of work already queued there (an activity whose kind is
 * already live on that board is skipped). That is what stops an idle agent
 * from filling its board with thirty identical naps.
 *
 * A tie goes to the LOWEST id, and the scan is a strict `>` over a static
 * table in id order, so the same (agent, board, clock) always picks the same
 * activity, run to run and row for row. NULL agent or NULL board is
 * JOB_INVALID. */
int agentBestFill(const struct Agent *a, const JobBoard *b, float now);

#endif /* ISOMATA_AGENTS_ACTIVITY_H */