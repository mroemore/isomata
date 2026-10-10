/*
 * Executor FSM (see ai/fsm.h). One static transition table; fsmStep reads
 * exactly one cell and copies the caller-set payload into its result.
 */

#include "ai/fsm.h"

#include <stddef.h>

/* (state, event) -> {next state, action}. Rows: IDLE, MOVING_TO, ACTING,
 * WAITING, DONE. Columns: START, ARRIVED, DENIED, PHASE_DONE, INTERRUPTED,
 * TIMEOUT. A cell whose next state equals its own state with action NONE is a
 * no-op (illegal pair or a handled-but-ignored event); both are the same
 * observable result, by design. */
static const int FSM_TABLE[FSM_STATE_COUNT][FSM_EVENT_COUNT][2] = {
	/* IDLE */
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
	},
	/* MOVING_TO: ARRIVED binds the activity; DENIED/INTERRUPTED abort the
	 * task; PHASE_DONE/TIMEOUT cannot arrive in flight and are ignored. */
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_ACTING, FSM_ACTION_BEGIN_ACTIVITY },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_MOVING_TO, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_MOVING_TO, FSM_ACTION_NONE },
	},
	/* ACTING: PHASE_DONE hands off to the timed phase (WAIT); any failure
	 * aborts. */
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_ACTING, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_WAITING, FSM_ACTION_WAIT },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_ACTING, FSM_ACTION_NONE },
	},
	/* WAITING: PHASE_DONE completes the goal; TIMEOUT is a bounded-wait
	 * failure; DENIED/INTERRUPTED abort. */
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_WAITING, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
	},
	/* DONE: terminal until a new START. */
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
	},
};

static FsmResult noop(int state)
{
	FsmResult r;

	r.state = state;
	r.action = FSM_ACTION_NONE;
	r.targetX = 0;
	r.targetZ = 0;
	r.step = 0;
	r.waitReason = 0;
	r.waitSeconds = 0.0f;
	return r;
}

void fsmInit(Fsm *fsm)
{
	if (fsm == NULL)
		return;
	fsm->state = FSM_STATE_IDLE;
	fsm->goalX = 0;
	fsm->goalZ = 0;
	fsm->step = 0;
	fsm->waitReason = 0;
	fsm->waitSeconds = 0.0f;
}

void fsmSetGoal(Fsm *fsm, int targetX, int targetZ)
{
	if (fsm == NULL)
		return;
	fsm->goalX = targetX;
	fsm->goalZ = targetZ;
}

FsmResult fsmStart(Fsm *fsm, int targetX, int targetZ)
{
	if (fsm == NULL)
		return noop(FSM_STATE_IDLE);
	fsmSetGoal(fsm, targetX, targetZ);
	return fsmStep(fsm, FSM_EVENT_START);
}

FsmResult fsmStep(Fsm *fsm, int event)
{
	FsmResult r;
	int state;
	int action;

	if (fsm == NULL)
		return noop(FSM_STATE_IDLE);
	state = fsm->state;
	/* Out-of-range state or event: a NONE no-op, the state untouched. */
	if (!fsmStateValid(state))
		return noop(state);
	if (!fsmEventValid(event))
		return noop(state);

	r.state = FSM_TABLE[state][event][0];
	action = FSM_TABLE[state][event][1];
	r.action = action;
	r.targetX = 0;
	r.targetZ = 0;
	r.step = 0;
	r.waitReason = 0;
	r.waitSeconds = 0.0f;
	switch (action) {
	case FSM_ACTION_MOVE_TO:
		r.targetX = fsm->goalX;
		r.targetZ = fsm->goalZ;
		break;
	case FSM_ACTION_BEGIN_ACTIVITY:
		r.step = fsm->step;
		break;
	case FSM_ACTION_WAIT:
		r.waitReason = fsm->waitReason;
		r.waitSeconds = fsm->waitSeconds;
		break;
	default:
		break;
	}
	fsm->state = r.state;
	return r;
}

const char *fsmStateName(int state)
{
	switch (state) {
	case FSM_STATE_IDLE:
		return "IDLE";
	case FSM_STATE_MOVING_TO:
		return "MOVING_TO";
	case FSM_STATE_ACTING:
		return "ACTING";
	case FSM_STATE_WAITING:
		return "WAITING";
	case FSM_STATE_DONE:
		return "DONE";
	default:
		return "?";
	}
}

const char *fsmEventName(int event)
{
	switch (event) {
	case FSM_EVENT_START:
		return "START";
	case FSM_EVENT_ARRIVED:
		return "ARRIVED";
	case FSM_EVENT_DENIED:
		return "DENIED";
	case FSM_EVENT_PHASE_DONE:
		return "PHASE_DONE";
	case FSM_EVENT_INTERRUPTED:
		return "INTERRUPTED";
	case FSM_EVENT_TIMEOUT:
		return "TIMEOUT";
	default:
		return "?";
	}
}

const char *fsmActionName(int action)
{
	switch (action) {
	case FSM_ACTION_NONE:
		return "NONE";
	case FSM_ACTION_MOVE_TO:
		return "MOVE_TO";
	case FSM_ACTION_BEGIN_ACTIVITY:
		return "BEGIN_ACTIVITY";
	case FSM_ACTION_WAIT:
		return "WAIT";
	case FSM_ACTION_FINISH_GOAL:
		return "FINISH_GOAL";
	default:
		return "NONE";
	}
}

bool fsmStateValid(int state)
{
	return state >= 0 && state < FSM_STATE_COUNT;
}

bool fsmEventValid(int event)
{
	return event >= 0 && event < FSM_EVENT_COUNT;
}
