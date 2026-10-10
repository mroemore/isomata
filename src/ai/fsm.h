#ifndef ISOMATA_AI_FSM_H
#define ISOMATA_AI_FSM_H

/*
 * Executor finite state machine (T21 layer 2 of the laundromat sim).
 *
 * Pure, table-driven, branch-bounded: no SDL, no allocation, no time source.
 * The FSM models *decisions* only — it returns an Action describing what the
 * sim glue (T22 interactables / T23 brain) should do; it never moves an entity
 * or touches a machine. One static transition table maps (state, event) ->
 * (next state, action); fsmStep applies exactly one table cell.
 *
 * STATES. IDLE (no task), MOVING_TO (walking to the bound tile),
 * ACTING (running an activity step at the target), WAITING (parked on a timed
 * machine phase / reason), DONE (the task ended — success, denial, interrupt
 * or timeout). DONE is the terminal "goal finished" state; a new START revives
 * it into MOVING_TO, so the brain (T23) sequences tasks
 * useMachine(washer) -> useMachine(dryer) -> leave by issuing one START per
 * task and watching for FINISH_GOAL.
 *
 * EVENTS. START (begin/re-target a task, carrying the move target),
 * ARRIVED (reached the target), DENIED (a claim/target refused), PHASE_DONE (an
 * activity step / machine phase completed), INTERRUPTED (external preemption),
 * TIMEOUT (a wait exceeded its bound).
 *
 * ACTIONS. NONE, MOVE_TO(target), BEGIN_ACTIVITY(step), WAIT(reason/seconds),
 * FINISH_GOAL. The payload is read from the Fsm's caller-set fields (goalX/Z,
 * step, waitReason/waitSeconds), so the table itself is pure state x event.
 *
 * ILLEGAL PAIRS. An event the current state does not handle is a no-op: the
 * state is unchanged and the action is NONE (with a zeroed payload). The table
 * below is the whole contract; fsmStep never fails, never asserts and is safe
 * for any (state, event) — an out-of-range state or event is likewise a NONE
 * no-op. A few cells are "handled but ignored" (e.g. MOVING_TO + PHASE_DONE)
 * and are indistinguishable from a no-op by their return value by design.
 */

#include <stdbool.h>

typedef enum FsmState {
	FSM_STATE_IDLE = 0,
	FSM_STATE_MOVING_TO = 1,
	FSM_STATE_ACTING = 2,
	FSM_STATE_WAITING = 3,
	FSM_STATE_DONE = 4,
	FSM_STATE_COUNT = 5,
} FsmState;

typedef enum FsmEvent {
	FSM_EVENT_START = 0,
	FSM_EVENT_ARRIVED = 1,
	FSM_EVENT_DENIED = 2,
	FSM_EVENT_PHASE_DONE = 3,
	FSM_EVENT_INTERRUPTED = 4,
	FSM_EVENT_TIMEOUT = 5,
	FSM_EVENT_COUNT = 6,
} FsmEvent;

typedef enum FsmActionKind {
	FSM_ACTION_NONE = 0,
	FSM_ACTION_MOVE_TO = 1,
	FSM_ACTION_BEGIN_ACTIVITY = 2,
	FSM_ACTION_WAIT = 3,
	FSM_ACTION_FINISH_GOAL = 4,
} FsmActionKind;

/* The FSM's own state plus the caller-set payload fields a transition copies
 * into its result. goalX/goalZ is the START move target (a tile, which also
 * identifies the machine the brain bound); step is the activity step id;
 * waitReason/waitSeconds parameterise a WAIT. */
typedef struct Fsm {
	int state;		/* FsmState */
	int goalX;
	int goalZ;
	int step;
	int waitReason;
	float waitSeconds;
} Fsm;

/* One fsmStep result: the resulting state, the action kind and its payload
 * (zeroed for actions that carry none / for a no-op). */
typedef struct FsmResult {
	int state;		/* FsmState */
	int action;		/* FsmActionKind */
	int targetX;		/* FSM_ACTION_MOVE_TO payload */
	int targetZ;
	int step;		/* FSM_ACTION_BEGIN_ACTIVITY payload */
	int waitReason;		/* FSM_ACTION_WAIT payload */
	float waitSeconds;
} FsmResult;

/* Reset to IDLE with a zeroed payload. NULL is a no-op. */
void fsmInit(Fsm *fsm);

/* Record the START move target without applying any transition; the caller
 * then steps FSM_EVENT_START. NULL `fsm` is a no-op. */
void fsmSetGoal(Fsm *fsm, int targetX, int targetZ);

/* Set the goal and apply FSM_EVENT_START in one call (the common brain path).
 * Equivalent to fsmSetGoal then fsmStep(..., FSM_EVENT_START). Returns the
 * START transition's result; a NULL `fsm` yields a {IDLE, NONE} result. */
FsmResult fsmStart(Fsm *fsm, int targetX, int targetZ);

/* Apply one event: apply the table cell for (fsm->state, event), write the
 * next state back to fsm and return {next state, action, payload}. An illegal
 * pair (or NULL `fsm`, or an out-of-range event) is a no-op returning the
 * current state with FSM_ACTION_NONE and a zeroed payload. Pure apart from
 * writing fsm->state. */
FsmResult fsmStep(Fsm *fsm, int event);

/* Stable names for logging: "IDLE"/"MOVING_TO"/"ACTING"/"WAITING"/"DONE",
 * "START"/"ARRIVED"/..., "NONE"/"MOVE_TO"/... An unknown value returns
 * "?" (state/event) or "NONE" (action). */
const char *fsmStateName(int state);
const char *fsmEventName(int event);
const char *fsmActionName(int action);

/* True when `state`/`event` is one of the defined values. */
bool fsmStateValid(int state);
bool fsmEventValid(int event);

#endif /* ISOMATA_AI_FSM_H */
