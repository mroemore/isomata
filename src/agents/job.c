/*
 * The job record and its lifecycle machine (see agents/job.h for the model
 * and the contract). One static transition table; jobStep reads exactly one
 * cell, writes back the next state and returns the action. Nothing else.
 *
 * Pure: no SDL, no allocation, no time source, no libm. The only write is
 * j->state, so a given (record, event) sequence is byte-identical run to run.
 */

#include "agents/job.h"

#include <stddef.h>

/* (state, event) -> {next state, action}. Rows: UNCLAIMED, CLAIMED, WALKING,
 * INTERACTING, WAITING, DONE, FAILED. Columns: START, ARRIVED, DENIED,
 * PHASE_DONE, INTERRUPTED, TIMEOUT.
 *
 * A cell whose next state equals its own state with action NONE is a no-op:
 * an illegal pair, which is a no-op BY DESIGN (same rule, same reasoning as
 * the T21 executor FSM this table grew out of) so a caller can never be
 * dragged into a state it did not ask for. A cell whose next state equals its
 * own state with a REAL action is not a no-op — the state is idempotent but
 * the action is observable, which is what keeps the wildcard failure rows
 * honest. */
static const int JOB_TABLE[JOB_STATE_COUNT][JOB_EVENT_COUNT][2] = {
	/* UNCLAIMED: sitting on a board. START claims it; the two wildcard
	 * failures can land before anyone claims it (the board dropping a
	 * stale row). Nothing else means anything here. */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
	},
	/* CLAIMED: reserved, not yet walking. ARRIVED issues the move (a
	 * claimer already standing on the target completes it on this same
	 * tick, so one place says "go" and one says "I am here"). START is
	 * NOT a re-claim — the job is owned, and a second START on a live job
	 * is exactly what an exclusive claim exists to prevent. */
	{
		{ JOB_CLAIMED, JOB_ACTION_NONE },
		{ JOB_WALKING, JOB_ACTION_MOVE_TO },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_CLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_CLAIMED, JOB_ACTION_NONE },
	},
	/* WALKING: ARRIVED binds the activity. TIMEOUT cannot arrive in flight
	 * (the activity's runtime has not begun) and is ignored; an unbounded
	 * walk is the body's problem, not the lifecycle's. */
	{
		{ JOB_WALKING, JOB_ACTION_NONE },
		{ JOB_INTERACTING, JOB_ACTION_BEGIN_ACTIVITY },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WALKING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WALKING, JOB_ACTION_NONE },
	},
	/* INTERACTING: PHASE_DONE hands off to the timed phase (WAIT);
	 * TIMEOUT is the activity's OWN runtime expiring, so it fails here
	 * and nowhere else. */
	{
		{ JOB_INTERACTING, JOB_ACTION_NONE },
		{ JOB_INTERACTING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WAITING, JOB_ACTION_WAIT },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
	},
	/* WAITING: parked on a machine phase. PHASE_DONE completes the job;
	 * TIMEOUT is the bounded wait running out. The board and the errand own
	 * the deadline (the laundromat's BRAIN_MACHINE_WAIT_SLACK is exactly
	 * this kind of bound) — the job's whole part is to consume the event.
	 * Without this cell a machine that never delivers its phase wake would
	 * hang the job, and its agent, forever. */
	{
		{ JOB_WAITING, JOB_ACTION_NONE },
		{ JOB_WAITING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
	},
	/* DONE: terminal until a new START revives it into CLAIMED. The two
	 * wildcard rows still fail it — a late interrupt on a finished job is
	 * the caller learning its reservation is gone. */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_DONE, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_NONE },
	},
	/* FAILED: also revivable by START. Both wildcard events stay FAILED —
	 * idempotent in state, but FINISH is still emitted so the caller sees
	 * the release (a claim released twice must be released, not silently
	 * swallowed). */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_FAILED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_NONE },
	},
};

/* Build a result. There is no payload to build: JobResult is {state, action}
 * because the record holds no move tile, no activity step and no wait reason
 * (see the header note). One constructor keeps the no-op and the real
 * transition identical in shape, so a caller cannot tell them apart by
 * anything it did not set. */
static JobResult makeResult(int state, int action)
{
	JobResult r;

	r.state = state;
	r.action = action;
	return r;
}

void jobInit(Job *j, int kind)
{
	int n;

	if (j == NULL)
		return;
	j->kind = kind;
	j->state = JOB_UNCLAIMED;
	j->targetKind = JOB_INVALID;
	j->targetId = JOB_INVALID;
	j->timer = 0.0f;
	j->duration = 0.0f;
	/* band -1 = DIRTY: urgency has never been computed for this job, so
	 * the board recomputes it on its first pass instead of reading a
	 * plausible-looking zero that means "nobody wants this". */
	j->band = -1;
	j->claimExpiry = 0.0f;
	j->claimer = JOB_INVALID;
	/* An activity serves the needs it declares and no others; a row that
	 * serves none is broken, so the default is "serves nothing", never
	 * "serves everything". */
	for (n = 0; n < NEED_COUNT; n++)
		j->served[n] = 0;
}

void jobSetTarget(Job *j, int kind, int id)
{
	if (j == NULL)
		return;
	j->targetKind = kind;
	j->targetId = id;
}

bool jobTargetTile(const Job *j, int dim, int *outX, int *outZ)
{
	int x;
	int z;

	if (j == NULL || outX == NULL || outZ == NULL)
		return false;
	/* Only a place resolves to a tile. An in-place activity has no
	 * target, and an unknown kind is a data error the caller must hear
	 * about — never a walk to tile (0,0). */
	if (j->targetKind != JOB_TARGET_TILE || j->targetId < 0)
		return false;
	x = j->targetId % JOB_TILE_SPAN;
	z = j->targetId / JOB_TILE_SPAN;
	if (dim > 0 && (x >= dim || z >= dim))
		return false;
	*outX = x;
	*outZ = z;
	return true;
}

JobResult jobStep(Job *j, int event)
{
	int state;
	int action;

	/* A NULL job is not a crash and not a transition: it reads as a fresh,
	 * unclaimed record, which is what a caller that has no job yet means. */
	if (j == NULL)
		return makeResult(JOB_UNCLAIMED, JOB_ACTION_NONE);
	state = j->state;
	/* Out-of-range state or event: a NONE no-op, the state echoed back
	 * untouched and the table never indexed. */
	if (!jobStateValid(state))
		return makeResult(state, JOB_ACTION_NONE);
	if (!jobEventValid(event))
		return makeResult(state, JOB_ACTION_NONE);

	action = JOB_TABLE[state][event][1];
	j->state = JOB_TABLE[state][event][0];
	return makeResult(j->state, action);
}

const char *jobKindName(int kind)
{
	switch (kind) {
	case JOB_WANDER:
		return "WANDER";
	case JOB_USE_MACHINE:
		return "USE_MACHINE";
	case JOB_PAY:
		return "PAY";
	case JOB_LOAD:
		return "LOAD";
	case JOB_COLLECT:
		return "COLLECT";
	case JOB_TALK:
		return "TALK";
	case JOB_PLAY_ARCADE:
		return "PLAY_ARCADE";
	case JOB_NAP:
		return "NAP";
	case JOB_REPAIR:
		return "REPAIR";
	case JOB_LEAVE:
		return "LEAVE";
	default:
		return "?";
	}
}

const char *jobStateName(int state)
{
	switch (state) {
	case JOB_UNCLAIMED:
		return "UNCLAIMED";
	case JOB_CLAIMED:
		return "CLAIMED";
	case JOB_WALKING:
		return "WALKING";
	case JOB_INTERACTING:
		return "INTERACTING";
	case JOB_WAITING:
		return "WAITING";
	case JOB_DONE:
		return "DONE";
	case JOB_FAILED:
		return "FAILED";
	default:
		return "?";
	}
}

const char *jobEventName(int event)
{
	switch (event) {
	case JOB_START:
		return "START";
	case JOB_ARRIVED:
		return "ARRIVED";
	case JOB_DENIED:
		return "DENIED";
	case JOB_PHASE_DONE:
		return "PHASE_DONE";
	case JOB_INTERRUPTED:
		return "INTERRUPTED";
	case JOB_TIMEOUT:
		return "TIMEOUT";
	default:
		return "?";
	}
}

const char *jobActionName(int action)
{
	switch (action) {
	case JOB_ACTION_NONE:
		return "NONE";
	case JOB_ACTION_MOVE_TO:
		return "MOVE_TO";
	case JOB_ACTION_BEGIN_ACTIVITY:
		return "BEGIN_ACTIVITY";
	case JOB_ACTION_WAIT:
		return "WAIT";
	case JOB_ACTION_FINISH:
		return "FINISH";
	case JOB_ACTION_CLAIM:
		return "CLAIM";
	default:
		return "NONE";
	}
}

bool jobStateValid(int state)
{
	return state >= 0 && state < JOB_STATE_COUNT;
}

bool jobEventValid(int event)
{
	return event >= 0 && event < JOB_EVENT_COUNT;
}
