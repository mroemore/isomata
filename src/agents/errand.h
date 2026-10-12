#ifndef ISOMATA_AGENTS_ERRAND_H
#define ISOMATA_AGENTS_ERRAND_H

/*
 * Errands: what an agent is TRYING TO DO. See agents/agent.h for the agent
 * that holds one, agents/job.h for the work an errand's steps enqueue, and
 * sim/brain.h for the customer driver that is the first errand-driven mind in
 * the tree.
 *
 * AN ERRAND IS DATA. An ErrandTemplate is a static table row: a role it is
 * registered for, the needs it serves, an ordered list of steps and a
 * priority floor. There is no errand code per goal — "the customer's
 * hardcoded flow" is a row, and adding a second flow (a staffer's fault round,
 * a critter's nightly wander) is another row, not another switch. That is the
 * whole point of the type: the T23 BrainPhase switch was code where this is a
 * row.
 *
 * AN ERRAND NEVER COMPETES FOR ATTENTION. It only enqueues jobs onto the
 * agent's own board (agents/board.h), and those jobs are arbitrated by the
 * want ladder exactly like everything else — which is why a world-seeded
 * errand is adopted AT ITS FLOOR and never forced: the floor is recorded in
 * the errand and handed to boardEnqueue as a PROVENANCE class, which board.h
 * documents may decide who may claim a job and may never decide its order.
 * The floor is not a priority the errand can push above the ladder with.
 *
 * THE TWO PRODUCERS LAND THE SAME WAY. errandAdoptByNeed scans the templates
 * registered for the agent's role and takes the one whose most pressing
 * served need is banded highest (agents/need.h), refusing one that is already
 * active. errandOfferToAgent is the world-seeded producer: the world hands an
 * agent a template id and it is adopted on the same terms, at the template's
 * declared priorityFloor. Neither producer can put an errand in front of a
 * more urgent want; they only decide WHETHER a new row appears in the agent's
 * errand list.
 *
 * A STEP IS A JOB OR A GATE. STEP_JOB enqueues one job of the step's jobKind
 * and waits for it to end: DONE advances the errand, FAILED re-selects (the
 * failed target is blacklisted for THIS errand instance only — a fresh
 * adoption starts with a clean slate). STEP_GATE waits for a named wake and
 * parks a stand-by job so the wait is bounded: the gate's deadline is the
 * caller's (the world knows how long a machine runs), it is carried into the
 * stand-by job's WAIT, and when it expires the job FAILS. That is the
 * producer for the JOB_TIMEOUT cell agents/job.h documents and the framework
 * had no producer for: "the WAITING cell is what stops a machine that never
 * delivers its phase wake from hanging the job — and the agent holding it —
 * forever." A gate is therefore never an unbounded wait.
 *
 * A TEMPLATE'S ROLE IS A GATE, NOT A DEFAULT. errandAdopt refuses when
 * a->role != t->role: a customer's laundry round is not something a critter
 * may adopt, and a data row that named the wrong role would be a silent
 * mis-cast rather than a refused one.
 *
 * WHAT THIS MODULE DOES NOT KNOW. It never touches an Entity, a Machine, an
 * Voxmap or a fee: picking which machine is free is the WORLD's business, and
 * so is what a machine costs. The caller owns that registry and answers two
 * questions this module asks by returning "not yet":
 *
 *   - errandBindTarget — "the step I am on needs a thing; here it is, and
 *     here is the tile its job walks to." Until a STEP_JOB step is bound,
 *     errandAdvance enqueues NOTHING rather than walking to the origin, which
 *     is the same "an unbound target is a hard failure" rule
 *     activityBindJob states for the activity layer.
 *   - the gate's deadline — errandGateSeconds.
 *
 * Not responsible for: choosing a job (agents/agent.h), scoring one
 * (agents/activity.h), executing one (agents/job.h), the world's effects
 * (payment, machine start, garment advance — sim/brain.h), or DECIDING to walk
 * out: this module says whether the errand is blocked and has been for how
 * long, and the driver's two-line gate (blocked past the window AND fed up)
 * turns that into an abandon. See ERRAND_LEAVE_THRESHOLD_WINDOW for the whole
 * mechanism, including which half of it actually decides.
 *
 * Pure: no SDL, no allocation, no time source, no libm. Templates are
 * `static const`, so errandsTemplate returns the SAME pointer for the same id
 * on every call and for the process lifetime. An unknown id is reported as a
 * NULL plus a stderr diagnostic — the Review Focus #5 contract: a silent
 * default row is indistinguishable from a real one.
 */

#include "agents/board.h"
#include "agents/need.h"

#include <stdbool.h>

/* Errands one agent may hold at once: a small, fixed, visible list, so the
 * queue of goals is bounded without allocation and the errand an agent is on
 * is the FIRST live entry (errandActive). */
#define ERRAND_MAX 4
/* Steps a template may declare. The shipped laundry round is seven (claim,
 * load, gate, collect — twice — then leave); sixteen leaves room for a
 * template with a gate between every pair of jobs without a second constant. */
#define ERRAND_MAX_STEPS 16
/* Targets one errand instance may blacklist after failed jobs. Per-instance
 * and deliberately small: the point is "do not pick that one again", not
 * "remember every machine I have ever failed on". */
#define ERRAND_MAX_FAILED 8
/* Failed steps on ONE step before the errand gives up as ERRAND_FAILED. The
 * blacklist is the real bound (a registry cannot exceed MACHINE_MAX), so this
 * is the bound on a failure that is NOT a machine — a route that will not
 * compute, say — which would otherwise retry forever. */
#define ERRAND_MAX_STEP_FAILURES 8
/* A sentinel for "no such thing": a step with no target, a gate with no
 * seconds, an adoption that did not happen. Never a valid index. */
#define ERRAND_INVALID (-1)
/* THE WALK OUT, STATED PLAINLY, because the honest version is less flattering
 * than the spec's prose and a reader deserves the real mechanism.
 *
 *   WHAT DECIDES: SUSTAINED BLOCKED TIME. `Errand.blockedFor` counts the
 *   seconds this errand has spent WAITING on the world — a gate whose machine
 *   has not finished, or a step with nothing bound to it because no machine of
 *   the kind it needs is free. It advances only while errandBlocked() is true
 *   and is RESET TO ZERO the moment the errand does anything at all (walking,
 *   paying, loading, collecting, walking out of the building). The patience
 *   budget is therefore spent on waiting and never on living: a customer whose
 *   errand is progressing is not impatient however long the round takes.
 *
 *   WHAT THE THRESHOLD IS: A GUARD RAIL, NOT A DISCRIMINATOR.
 *   ERRAND_LEAVE_THRESHOLD is checked as a necessary condition for walking
 *   out — but under the framework's CURRENT need arithmetic it never
 *   discriminates, and pretending otherwise would be the lie this comment
 *   exists to prevent. needDecay removes decayMul[n] per second at content and
 *   NOTHING in the tree restores a need, so NEED_SATISFACTION is at 0 within a
 *   second of any agent's birth and sits there; the condition
 *   "satisfaction < 0.25" is therefore true for essentially every blocked
 *   errand, which makes the threshold a floor the value sits under rather than
 *   a comparison that decides anything. Read it as "may only walk out a
 *   customer who is fed up", and expect it to start discriminating the day a
 *   later phase restores needs (which would put a progressing customer's
 *   satisfaction back up and make the pair "blocked AND fed up" mean
 *   something the blocked half does not already mean). Do NOT invent
 *   restoration to make this constant look busier: that is a different task.
 *
 *   The window is DELIBERATELY SHORTER THAN A GATE'S BOUND (brain.h's slack
 *   plus the run length), and that ordering is load-bearing: when a machine is
 *   never going to finish, the wait must END by failing its job — the
 *   framework's one producer for JOB_TIMEOUT — before the customer gives the
 *   errand up, so the re-plan is a re-plan and not a walk out.
 *
 * Both halves are pinned by tests that discriminate between them:
 * test_a_customer_who_is_progressing_never_walks_out runs a round LONGER
 * than the window and requires no abandon, and
 * test_the_patience_budget_is_spent_only_on_blocked_time blocks one customer
 * past it and requires the abandon. Dropping the window fails both; charging
 * the budget for progress fails the first alone. */
#define ERRAND_LEAVE_THRESHOLD 0.25f
/* How many BLOCKED seconds a fed-up customer tolerates before giving the
 * errand up and walking out. See the note above: this is the half of the gate
 * that decides. */
#define ERRAND_LEAVE_THRESHOLD_WINDOW 45.0f
/* Extra decay the WORLD applies to NEED_SATISFACTION for every second an
 * errand is blocked, on top of the role's own decay in agentsDecay:
 * "being blocked is an inconvenience" is the spec's tier-1 reading, and the
 * base decay alone cannot know what the agent was doing. It is a bias on the
 * want, not the decision — the want decides nothing here (see above). */
#define ERRAND_BLOCKED_DECAY 0.25f
/* The job a gate parks while it waits. WANDER is the catalogue's one
 * in-place activity, and a gate binds the wait tile as its target, so the body
 * actually goes and stands by (the demo's "owner away") instead of standing
 * on the machine it is waiting for. */
#define ERRAND_GATE_JOB JOB_WANDER

/* Forward declaration only: the agent record owns the errand list, so
 * agent.h includes THIS header. The functions below that take an agent say
 * `struct Agent *`, which is the very same type as `Agent *` (agents/
 * activity.h makes the same argument for the same reason). */
struct Agent;

/* What one step IS. STEP_JOB enqueues work; STEP_GATE waits. The two gate
 * values are the WAKE a gate resumes on, and they are a separate range from
 * the step kinds above them because a gate's `kind` is always STEP_GATE —
 * which value is what decides when the step ends. */
typedef enum ErrandStepKind {
	STEP_JOB = 0,		/* enqueue one job, then wait for it to end */
	STEP_GATE,		/* wait: `gate` names the wake that resumes it */
	STEP_GATE_WAIT_MACHINE,	/* the target machine's PHASE_DONE wake */
	STEP_GATE_WALK_TO,	/* the body's arrival at the step's tile */
	STEP_GATE_KIND_COUNT	/* table width; not a value itself */
} ErrandStepKind;

/* An errand's own lifecycle. Only ERRAND_ACTIVE advances; the three terminal
 * states are what the caller reads to decide what the agent does next (the
 * customer driver walks out on ABANDONED and re-plans on FAILED). */
typedef enum ErrandState {
	ERRAND_ACTIVE = 0,
	ERRAND_DONE,
	ERRAND_FAILED,
	ERRAND_ABANDONED,
	ERRAND_STATE_COUNT
} ErrandState;

/* The template ids this module declares rows for. */
typedef enum ErrandId {
	ERR_LAUNDRY_CLEAN = 0,
	ERR_FIX_MACHINE,
	ERR_COUNT	/* table width; not an id itself */
} ErrandId;

/* The thing the current step is about. This is NOT Job.targetKind: a job
 * names the TILE it walks to (agents/job.h), while an errand names the THING
 * the tile belongs to, which is how the world knows which machine to claim
 * and which garment state the step applies to. */
typedef enum ErrandTarget {
	ERRAND_TARGET_NONE = 0,	/* the step needs nothing bound */
	ERRAND_TARGET_MACHINE,	/* targetId is a MachineHandle */
	ERRAND_TARGET_PLACE	/* targetId is a packed tile (exit, wait spot) */
} ErrandTarget;

/* One declared step. `machineKind` is the payload a STEP_JOB step that works a
 * machine needs (MACHINE_KIND_WASHER / _DRYER): it is what the template says
 * "wash, THEN dry", so the order is data rather than the caller inferring it
 * from whatever load the agent happens to be carrying. */
typedef struct ErrandStep {
	int kind;		/* ErrandStepKind: STEP_JOB or STEP_GATE */
	int jobKind;		/* STEP_JOB: the JobKind to enqueue */
	int gate;		/* STEP_GATE: the wake that resumes this step */
	int machineKind;	/* STEP_JOB on a machine: MACHINE_KIND_* */
} ErrandStep;

/* One errand template: a static row. */
typedef struct ErrandTemplate {
	int id;			/* ErrandId */
	const char *name;	/* "LAUNDRY_CLEAN" — intent-revealing */
	int role;		/* AgentRole; an errand is refused cross-role */
	/* 0..1 STRENGTH per need, like Activity.serves (agents/activity.h) and
	 * for the same reason: an errand that relieved a want 0.9 of the way
	 * must be distinguishable from one that relieved it in full, or the
	 * fold in errandAdvance collapses to a flag and the ladder's ordering
	 * between two errands becomes arbitrary. The brief sketched this as an
	 * int array; an int cannot express a strength and silently truncates
	 * 0.9f to 0. */
	float serves[NEED_COUNT];
	ErrandStep steps[ERRAND_MAX_STEPS];
	int stepCount;
	/* THE DECLARED FLOOR, and the whole of the world-seeded contract. It
	 * is a board provenance class (agents/board.h): recorded on the
	 * errand and handed to boardEnqueue, which decides who MAY claim
	 * the job and never how it orders. A world event that offers an
	 * errand therefore lands on the ladder at its floor instead of on
	 * top of it. */
	int priorityFloor;
	bool worldSeeded;	/* offered by the world, not chosen by need */
} ErrandTemplate;

/* One live errand on one agent: a template id, how far through it the agent
 * is, and the bookkeeping a re-plan needs. Plain data, no pointers, no owned
 * memory — the list is embedded in the Agent by value. */
typedef struct Errand {
	int tmplId;
	int step;		/* index into the template's steps */
	int state;		/* ErrandState */
	int targetKind;		/* ErrandTarget */
	int targetId;		/* MachineHandle, or a packed tile */
	int targetTile;		/* JOB_TILE_PACK(x, z), or ERRAND_INVALID */
	int failureCount;	/* failed jobs on the CURRENT step */
	float startedAt;	/* the caller's clock at adoption */
	/* THE GARMENT THIS ERRAND IS DELIVERING (a GarmentState, stored as a
	 * plain int so this header needs no sim/ include). The brief spelled
	 * this field `garbage`; it holds a load state, and a field called
	 * `garbage` in a struct reads as uninitialised, so it is named for
	 * what it is. */
	int garment;
	int jobHandle;		/* the job this step enqueued, or ERRAND_INVALID */
	int retryStep;		/* the STEP_JOB step a failed gate returns to */
	int priorityFloor;	/* the template's floor, recorded at adoption */
	int gateSecs;		/* the caller's deadline for this gate */
	/* THE BAND THIS ERRAND'S LAST JOB WAS STAMPED WITH, so a gate can be
	 * stamped with it too. A gate is a CONTINUATION of the step before it,
	 * not a new activity, and it must never be less urgent than the work it
	 * is waiting for: a stand-by wander bands 0 on its own merits, which
	 * would let a nap (band 6) out-rank the errand's own wait and the
	 * customer would sleep through their own wash. See errandEnqueueJob. */
	int stepBand;
	int blacklist[ERRAND_MAX_FAILED];	/* target ids this instance
						   will not try again */
	int blacklistCount;
	float blockedFor;	/* seconds the errand has spent blocked */
} Errand;

/* One agent's errands, embedded in the Agent by value (agents/agent.h) for
 * the same reason the personal board is: AGENT_MAX x ERRAND_MAX of static
 * memory, and no allocation anywhere on the hot path. */
typedef struct ErrandList {
	Errand items[ERRAND_MAX];
	int count;
} ErrandList;

/* Empty the list: every slot ERRAND_INVALID, count 0. NULL is a no-op. An
 * agent's list starts this way (agentCreate), so a born agent has no goal. */
void errandListInit(ErrandList *l);

/* The template for `id`, or NULL for an unknown id — with a stderr diagnostic,
 * because "no such template" must never read as "a template with no steps".
 * The pointer is static storage: the same pointer on every call, never
 * freed. NULL / negative ids are refused the same way. */
const ErrandTemplate *errandsTemplate(int id);

/* How many templates the table holds: 0..ERRAND_MAX, never more. */
int errandsTemplateCount(void);

/* Row `index` in table order, or NULL outside 0..errandsTemplateCount()-1.
 * Iteration order is table order and is stable. */
const ErrandTemplate *errandsTemplateAt(int index);

/* ADOPT a template into `a`'s errand list. Returns the template id on
 * success, ERRAND_INVALID on any refusal:
 *
 *   - a NULL agent, or a NULL template (a data row that never arrived);
 *   - a->role != t->role — an errand is registered for a role;
 *   - the template is ALREADY ACTIVE on this agent — one goal at a time is
 *     what "adopt" means, and a second instance of the same goal would
 *     enqueue the same jobs twice;
 *   - the list is full (ERRAND_MAX live errands).
 *
 * Adoption compacts first: terminal errands are dropped from the list, so a
 * long-lived agent reuses its slots and the list can never fill with finished
 * business. A world-seeded template is adopted AT ITS DECLARED FLOOR and no
 * higher — the floor is recorded on the errand and nothing else happens
 * here, because the ladder (agents/agent.h) is what arbitrates the errand's
 * jobs against everything else. */
int errandAdopt(struct Agent *a, const ErrandTemplate *t, float now);

/* THE NEED-DRIVEN PRODUCER. Scan every template registered for a->role,
 * skip one already active on this agent, and adopt the one whose most
 * pressing served need reads the highest band (the same wantUrgency fold
 * agents/activity.h scores jobs with, so an errand and a fill compete on
 * identical terms). A tie goes to the LOWEST template id, over a scan in id
 * order with a strict `>`, so the same agent always adopts the same row.
 *
 * Returns the template id adopted, or ERRAND_INVALID when there is nothing to
 * adopt (a NULL agent, no template for the role, them all already active, or a
 * full list). Refusing is a normal answer, not an error: an agent whose needs
 * are all content and whose only template is already running must not be given
 * a second copy of it. */
int errandAdoptByNeed(struct Agent *a, float now);

/* THE WORLD-SEEDED PRODUCER. Adopt `templateId` for `a` on exactly the terms
 * errandAdopt uses — same role gate, same already-active refusal, same floor
 * — reached by id rather than by need scan, because the WORLD knows what it
 * is offering. Returns the template id adopted, or ERRAND_INVALID when the id
 * names no template (a stderr diagnostic says so) or the adoption is refused.
 *
 * "Never forced" is the whole contract and it lives in what this function
 * does NOT do: it does not write a need, does not claim a job, and does not
 * touch the ladder. The errand it creates carries t->priorityFloor into every
 * job it enqueues as a provenance class, so it wins or loses on the same band
 * comparison as the agent's own filler — an errand offered at
 * JOB_PRIO_ASSIGNED loses to an urgent wash exactly as a job from the shared
 * board would. */
int errandOfferToAgent(struct Agent *a, int templateId, float now);

/* The live errand on `a` — the first ERRAND_ACTIVE entry — or NULL. This is
 * the one an errand-driven caller steps each tick. */
Errand *errandActive(struct Agent *a);

/* The errand on `a` running `tmplId`, or NULL (including when that errand is
 * terminal: a finished errand is not a live one). */
Errand *errandFind(struct Agent *a, int tmplId);

/* Bind the thing the CURRENT step is about, and the tile its job walks to:
 * errandBindTarget(e, ERRAND_TARGET_MACHINE, h, JOB_TILE_PACK(mx, mz)).
 * Purely a binding — it applies no transition and enqueues nothing; the step
 * still advances on the next errandAdvance. `targetTile` is a packed tile
 * (agents/job.h) or ERRAND_INVALID to leave the step unbound. Binding CLEARS
 * the blacklist: the blacklist bounded the PREVIOUS candidate, and a fresh
 * target is a fresh chance — which is also what makes it impossible for one
 * failure to blacklist every machine a re-select has left to choose from. */
void errandBindTarget(Errand *e, int targetKind, int id, int targetTile);

/* The wake that resumes the CURRENT step: STEP_GATE_WAIT_MACHINE /
 * STEP_GATE_WALK_TO on a gate step, ERRAND_INVALID on a job step (a job
 * ends on its own lifecycle, not on a named wake). */
int errandStepGate(const Errand *e);

/* Bound a gate's deadline: how many seconds the wait job may run before the
 * deadline FAILS the job, which is how a machine that never delivers its
 * phase wake stops hanging the errand. The deadline is the CALLER's — only
 * the world knows how long a machine runs — and errandAdvance carries it into
 * the wait job's duration. ERRAND_INVALID clears it (an unbounded wait; the
 * caller says so explicitly). */
void errandGateSeconds(Errand *e, float secs);

/* Blacklist one target id for THIS errand instance: the next re-select will
 * not choose it. Idempotent and bounded (ERRAND_MAX_FAILED); a NEGATIVE id
 * is ignored, so a caller with nothing bound cannot blacklist slot -1.
 *
 * The caller owns the blacklist's content, not the errand: a gate's binding
 * is the WAIT SPOT (a tile) while the thing that actually went wrong is the
 * machine the errand is waiting on, and only the caller knows which of the
 * two it is talking about. */
void errandBlacklist(Errand *e, int id);

/* True when the errand is ACTIVE and its current step is waiting on the
 * world rather than making progress: a gate step, or a step with nothing
 * bound to it yet. This is the single question both halves of the walk-out
 * hang on — it is what makes blockedFor advance (the patience budget) and what
 * makes the blocked-time satisfaction decay apply. An agent that is walking,
 * paying or collecting is busy; an agent whose machine has not finished yet is
 * ANNOYED, and annoyance is what this function means. */
bool errandBlocked(const Errand *e);

/* Give the errand up: ERRAND_ABANDONED, step 0, no job handle. The errand
 * enqueues nothing more; the caller decides what the agent does next (the
 * customer driver walks it to the door). Abandoning releases held resources
 * in the CALLER, because the caller is what owns them — the errand layer has
 * no claim on a machine and must not pretend to. NULL is a no-op. */
void errandAbandon(Errand *e);

/* STEP BACK TO THE JOB STEP THAT PRODUCED WHAT WAS LOST — the re-plan a
 * machine that broke between a run finishing and its load being collected
 * needs: the errand returns to the nearest STEP_JOB step at or before the
 * current one that declares `jobKind`, keeping its blacklist (the thing that
 * was lost is still a thing this errand must not pick again).
 *
 * With nothing left to go back to — a LEAVE step, or a template whose earlier
 * steps declare no such job — the errand is ERRAND_FAILED, because "carry on"
 * would be a step this template does not contain. Returns false when that is
 * what happened, so the caller can react rather than assume a rewind. NULL
 * errand is a no-op returning false. */
bool errandRewindToJob(Errand *e, int jobKind);

/* ONE STEP OF THE ERRAND. Advances at most one thing per call, so a caller
 * can interleave the world's effects with the framework's own tick:
 *
 *   STEP_JOB, nothing bound yet  -> return. The caller binds the target it
 *     found in the world; an unbound target is never guessed at.
 *   STEP_JOB, nothing enqueued   -> bind a Job of the step's jobKind from
 *     the catalogue (agents/activity.h), bind the step's tile as its target,
 *     stamp the band COMPUTED from the needs it serves, and enqueue it on `b`
 *     at the errand's recorded priorityFloor.
 *   STEP_JOB, job live           -> return; agentsUpdate is running it.
 *   STEP_JOB, job DONE           -> retire it, step++, clear the handle and
 *     the step's failure count. At the last step the errand is ERRAND_DONE.
 *   STEP_JOB, job FAILED         -> retire it, failureCount++, blacklist the
 *     target for THIS errand, and re-select: the step stays put (a gate step
 *     goes BACK to the job step it was waiting on), so the caller's next
 *     bind picks a different thing. Past ERRAND_MAX_STEP_FAILURES on one step
 *     the errand is ERRAND_FAILED rather than retrying forever.
 *   STEP_GATE, nothing enqueued  -> park the stand-by wait job (see
 *     ERRAND_GATE_JOB) for `gateSecs` seconds on the same board, stamped
 *     with the band the step it follows was stamped with: a gate is a
 *     continuation of that step, not a new activity, and it must not be
 *     less urgent than the work it is waiting for.
 *   STEP_GATE, job FAILED -> re-select, as a job step does.
 *   STEP_GATE, job live          -> return.
 *   STEP_GATE, job DONE          -> retire it, step++ (the wake arrived).
 *   STEP_GATE, job FAILED        -> as above, but the step falls back to
 *     `retryStep` (the job step whose wake it was waiting on).
 *
 * `b` is the agent's OWN board — an errand's jobs are its own committed work
 * and go on its personal board, never on the shared one. `now` is the
 * caller's clock (stamped into startedAt at adoption); the errand has no
 * time source of its own. NULL errand/agent/board is a no-op, and a terminal
 * errand never advances.
 */
void errandAdvance(Errand *e, struct Agent *a, float now, JobBoard *b);

/* Stable names for logging: "ACTIVE"/"DONE"/..., "JOB"/"GATE"/... An unknown
 * state returns "?" and an unknown step kind "JOB". */
const char *errandStateName(int state);
const char *errandStepKindName(int kind);

/* True when `state` is one of the defined ErrandState values. */
bool errandStateValid(int state);

#endif /* ISOMATA_AGENTS_ERRAND_H */