#ifndef ISOMATA_AGENTS_JOB_H
#define ISOMATA_AGENTS_JOB_H

/*
 * The job record and its lifecycle machine: what an agent DOES, as a live
 * instance of an activity bound to a target. See agents/need.h for the want
 * ladder that decides which job is worth taking, and agents/agent.h for the
 * agent that owns one.
 *
 * WHAT A JOB IS. A Job is data — a kind (JobKind), a bound target
 * (targetKind/targetId, e.g. machine 3), the needs it serves, its cached
 * urgency band, and who has claimed it. It never holds a movement path: the
 * Entity walking the job owns its own path[ENTITY_MAX_PATH][2] (see
 * entities/entities.h, consumed by entityWalkPath), so a job is one small
 * fixed record and a board of 128 of them costs nothing.
 *
 * THE LIFECYCLE IS A TABLE, NEVER A PLANNER. One static 7 x 6 table maps
 * (state, event) -> (next state, action); jobStep applies exactly one cell and
 * copies nothing else out of the record. The machine has no opinion about
 * WHICH job to run, whether the run is worth it, or how to get there — that is
 * the board's and the body's business. The table is the whole contract.
 *
 *   UNCLAIMED -> CLAIMED -> WALKING -> INTERACTING -> WAITING -> DONE
 *        every state --DENIED / INTERRUPTED---------> FAILED
 *        DONE, FAILED --START------------------------> CLAIMED (revive)
 *
 * THE MOVE IS TWO STEPPED. CLAIMED + ARRIVED issues MOVE_TO and enters
 * WALKING; WALKING + ARRIVED begins the activity. A claimed job whose claimer
 * is already standing on the target completes the move on the same tick, so
 * there is exactly one place that says "I am there" and exactly one that says
 * "go there".
 *
 * DENIED AND INTERRUPTED FAIL THE JOB FROM WHENEVER IT IS. A denial (the
 * claim expired, the machine was taken, the reservation lapsed) or a
 * preemption can land at any moment, so BOTH fail the job from EVERY state —
 * including the terminals, where FAILED is idempotent but JOB_ACTION_FINISH is
 * still emitted so the caller always sees the release. There is no state in
 * which a denial is ignored: a refused job is a dead job, wherever it happened
 * to be when it was refused.
 *
 * TIMEOUT IS NOT A WILDCARD — IT IS A DEADLINE. It lands only where a deadline
 * is actually running: INTERACTING (the activity's own runtime expiring) and
 * WAITING (the bounded wait for a machine phase — the laundromat's own
 * BRAIN_MACHINE_WAIT_SLACK is exactly this kind of bound). The board and the
 * errand OWN the deadline; the job's whole part is to consume the event.
 * Everywhere else there is no deadline to expire, so TIMEOUT is a no-op and a
 * stray wake can never kill a job that is waiting on nothing. The WAITING cell
 * is what stops a machine that never delivers its phase wake from hanging the
 * job — and the agent holding it — forever.
 *
 * START CLAIMS, IT DOES NOT RE-CLAIM. START revives DONE and FAILED into
 * CLAIMED, and claims an UNCLAIMED job. On a LIVE job (CLAIMED, WALKING,
 * INTERACTING, WAITING) START is a no-op.
 *
 * Why revival works from DONE and FAILED but not from WALKING: DONE and FAILED
 * are TERMINAL — nobody owns them and nothing is in flight, so a START there is
 * simply the first act of a new claim. A live job is owned by `claimer` and has
 * an activity in flight, so a START there would be a second claim on a job
 * somebody already holds: precisely what an exclusive claim exists to prevent
 * ("no job is double-claimed"). Re-planning is therefore expressed by FAILING
 * a job (DENIED / INTERRUPTED / TIMEOUT) and enqueuing a new one, never by
 * retargeting a live one.
 *
 * ILLEGAL PAIRS ARE NO-OPS. An event the current state does not handle leaves
 * the state alone and reports JOB_ACTION_NONE, so a caller can never be
 * dragged into a state it did not ask for. A no-op is distinguishable from a
 * real transition because every real transition reports a non-NONE action.
 * jobStep never fails and never asserts: an out-of-range state or event, or a
 * NULL job, is likewise a NONE no-op.
 *
 * THERE IS NO ACTION PAYLOAD. JobResult is {state, action} and nothing else,
 * because this module has nothing to put in one. The job's target is already
 * reachable on the record as targetKind/targetId, and the caller resolves that
 * pair to a tile when it acts on JOB_ACTION_MOVE_TO. A wait's duration belongs
 * to the activity and an activity's step id to the activity table — neither is
 * a property of the TRANSITION, so neither is a field here. If a later task
 * needs one, it adds the field when it has a real source for it; a result
 * whose payload can never be set is worse than no payload, because every
 * reader has to rediscover that.
 *
 * The actions are worth reporting all the same: MOVE_TO, BEGIN_ACTIVITY, WAIT,
 * FINISH and CLAIM say which lifecycle phase the job has just entered and what
 * the caller should do about it. That is the whole output of this module.
 *
 * NOT RESPONSIBLE FOR: choosing a job (no wants, no bands, no preemption —
 * that is agents/need.h and the board), moving the body (no path, no tile),
 * the clock (no dt, no timers: duration/claimExpiry are read by their owner),
 * persistence, and the registry (a Job is a plain record the caller owns).
 *
 * Pure: no SDL, no allocation, no time source, no libm. jobStep writes back
 * exactly one field — state — so the same (record, event) sequence always
 * produces the same result, run to run.
 */

#include "agents/need.h"

#include <stdbool.h>

/* The sentinel for "nothing bound yet": an unclaimed job has no targetKind,
 * no targetId and no claimer. Never a valid index. */
#define JOB_INVALID (-1)

typedef enum JobKind {
	JOB_WANDER,		/* nothing better to do; a filler want */
	JOB_USE_MACHINE,	/* drive a machine through its phases */
	JOB_PAY,		/* settle at the till */
	JOB_LOAD,		/* load a machine */
	JOB_COLLECT,		/* take a finished machine's output */
	JOB_TALK,		/* a conversation with another agent */
	JOB_PLAY_ARCADE,	/* an arcade session */
	JOB_NAP,		/* rest */
	JOB_REPAIR,		/* fix a machine */
	JOB_LEAVE,		/* depart the site */
	JOB_KIND_COUNT		/* table width; a job's kind is a JobKind */
} JobKind;

typedef enum JobState {
	JOB_UNCLAIMED,		/* on a board, nobody owns it */
	JOB_CLAIMED,		/* reserved by `claimer` */
	JOB_WALKING,		/* heading for the target */
	JOB_INTERACTING,	/* running the activity's own phase */
	JOB_WAITING,		/* parked on a timed machine phase */
	JOB_DONE,		/* the activity completed */
	JOB_FAILED,		/* denied, interrupted, or timed out */
	JOB_STATE_COUNT		/* table rows; not a state itself */
} JobState;

typedef enum JobEvent {
	JOB_START,		/* claim (or revive) the job */
	JOB_ARRIVED,		/* the body is at the target */
	JOB_DENIED,		/* a claim or target was refused */
	JOB_PHASE_DONE,		/* an activity step or machine phase ended */
	JOB_INTERRUPTED,	/* external preemption */
	JOB_TIMEOUT,		/* the activity's own runtime expired */
	JOB_EVENT_COUNT		/* table columns; not an event itself */
} JobEvent;

typedef enum JobAction {
	JOB_ACTION_NONE,	/* nothing to do (also: an illegal pair) */
	JOB_ACTION_MOVE_TO,	/* walk to the target */
	JOB_ACTION_BEGIN_ACTIVITY,	/* start the activity at the target */
	JOB_ACTION_WAIT,	/* park for a timed phase */
	JOB_ACTION_FINISH,	/* the job ended: release, succeeded or not */
	JOB_ACTION_CLAIM	/* take ownership of the job */
} JobAction;

/* One job: a live instance of an activity bound to a target, with state,
 * the needs it serves, a cached urgency band and a claim. Plain data, no
 * pointers, no owned memory — a board holds these by value. */
typedef struct Job {
	int kind;		/* JobKind as a plain int: it is data */
	int state;		/* JobState */
	/* The bound thing. A job targets a THING (a machine, a garment, a
	 * place), not a tile: the move tile is resolved by whoever owns the
	 * world, from this pair. JOB_INVALID until jobSetTarget. */
	int targetKind;
	int targetId;
	float timer;		/* phase clock; the owner's to advance */
	float duration;		/* phase length; the owner's to compare */
	int served[NEED_COUNT];	/* the needs this job serves, by slot */
	int band;		/* cached urgency band; -1 = dirty, unrecomputed */
	float claimExpiry;	/* reservation window; the board's to compare */
	int claimer;		/* EntityHandle, or JOB_INVALID when unclaimed */
} Job;

/* One jobStep result: the resulting state and the action to take. There is no
 * payload — see the header note; the caller resolves targetKind/targetId to a
 * tile, and the activity owns its step and wait parameters. */
typedef struct JobResult {
	int state;		/* JobState */
	int action;		/* JobAction */
} JobResult;

/* Initialise a record: kind as given, state JOB_UNCLAIMED, band -1 (dirty, so
 * the board recomputes urgency on its first pass), JOB_INVALID for
 * targetKind/targetId/claimer, every served need 0, and zeroed timer /
 * duration / claimExpiry. A job that serves no need is a broken activity row,
 * not an urgent job, so served[] starts clear rather than "all of them".
 * NULL is a no-op. */
void jobInit(Job *j, int kind);

/* Record the bound target as a (kind, id) pair; JOB_INVALID clears it. Purely
 * a binding operation — it never applies a transition, so claiming a job and
 * naming its target are separate steps. NULL is a no-op. */
void jobSetTarget(Job *j, int kind, int id);

/* Apply one event: look up the cell for (j->state, event), write the next
 * state back to j->state and return {next state, action}.
 *
 * An illegal pair (an event the state does not handle) is a no-op returning
 * the current state with JOB_ACTION_NONE. An out-of-range state is echoed
 * back unchanged and out-of-range events and a NULL job return
 * {JOB_UNCLAIMED, JOB_ACTION_NONE}. Pure apart from writing j->state. */
JobResult jobStep(Job *j, int event);

/* Stable names for logging: "UNCLAIMED"/"CLAIMED"/..., "START"/"ARRIVED"/...,
 * "NONE"/"MOVE_TO"/... An unknown value returns "?" for a state or event and
 * "NONE" for an action (an unknown action is, in effect, no action). */
const char *jobStateName(int state);
const char *jobEventName(int event);
const char *jobActionName(int action);

/* True when `state`/`event` is one of the defined values — the table bounds. */
bool jobStateValid(int state);
bool jobEventValid(int event);

#endif /* ISOMATA_AGENTS_JOB_H */
