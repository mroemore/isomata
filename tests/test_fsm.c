/*
 * Executor FSM tests (CTOL rung 1: unit + boundary).
 *
 * Pin the whole transition table cell by cell against an independent copy of
 * the expected table, then the payloads (START target, BEGIN_ACTIVITY step,
 * WAIT reason/seconds), the illegal-pair no-op rule, the laundromat task
 * sequence, the name/validity helpers and NULL-safety.
 *
 * Pure: links ai/fsm.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_fsm().
 */

#include "unity.h"

#include "ai/fsm.h"

/* Independent copy of the contract: (state, event) -> {next state, action}.
 * Rows IDLE, MOVING_TO, ACTING, WAITING, DONE; columns START, ARRIVED, DENIED,
 * PHASE_DONE, INTERRUPTED, TIMEOUT. A cell whose state is unchanged with NONE
 * is a no-op (illegal pair or handled-but-ignored). */
static const int EXPECT[FSM_STATE_COUNT][FSM_EVENT_COUNT][2] = {
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
		{ FSM_STATE_IDLE, FSM_ACTION_NONE },
	},
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_ACTING, FSM_ACTION_BEGIN_ACTIVITY },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_MOVING_TO, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_MOVING_TO, FSM_ACTION_NONE },
	},
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_ACTING, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_WAITING, FSM_ACTION_WAIT },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_ACTING, FSM_ACTION_NONE },
	},
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_WAITING, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
		{ FSM_STATE_DONE, FSM_ACTION_FINISH_GOAL },
	},
	{
		{ FSM_STATE_MOVING_TO, FSM_ACTION_MOVE_TO },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
		{ FSM_STATE_DONE, FSM_ACTION_NONE },
	},
};

/* Walk every (state, event) cell: the returned state/action and the state
 * written back both match the table; an action that carries no payload (NONE
 * / FINISH_GOAL) has a zeroed payload. */
static void test_full_table_every_cell(void)
{
	int s;
	int e;

	for (s = 0; s < FSM_STATE_COUNT; s++) {
		for (e = 0; e < FSM_EVENT_COUNT; e++) {
			Fsm f;
			FsmResult r;

			fsmInit(&f);
			f.state = s;
			f.goalX = 41;
			f.goalZ = 42;
			f.step = 7;
			f.waitReason = 3;
			f.waitSeconds = 2.5f;
			r = fsmStep(&f, e);
			TEST_ASSERT_EQUAL_INT(EXPECT[s][e][0], r.state);
			TEST_ASSERT_EQUAL_INT(EXPECT[s][e][1], r.action);
			TEST_ASSERT_EQUAL_INT(EXPECT[s][e][0], f.state);
			if (r.action == FSM_ACTION_NONE ||
			    r.action == FSM_ACTION_FINISH_GOAL) {
				TEST_ASSERT_EQUAL_INT(0, r.targetX);
				TEST_ASSERT_EQUAL_INT(0, r.targetZ);
				TEST_ASSERT_EQUAL_INT(0, r.step);
				TEST_ASSERT_EQUAL_INT(0, r.waitReason);
				TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.waitSeconds);
			}
		}
	}
}

/* A legal transition changes the state; a no-op leaves it exactly as it was. */
static void test_state_change_rule(void)
{
	Fsm f;

	fsmInit(&f);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
	fsmStep(&f, FSM_EVENT_START);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_MOVING_TO, f.state);

	/* ARRIVED in IDLE is a no-op: still IDLE. */
	fsmInit(&f);
	fsmStep(&f, FSM_EVENT_ARRIVED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
}

/* START payload: the move target is copied from the recorded goal, and every
 * state accepts START as a begin / re-target. */
static void test_start_payload_and_retarget(void)
{
	Fsm f;
	FsmResult r;
	int s;

	for (s = 0; s < FSM_STATE_COUNT; s++) {
		fsmInit(&f);
		f.state = s;
		r = fsmStart(&f, 3, 5);
		TEST_ASSERT_EQUAL_INT(FSM_STATE_MOVING_TO, r.state);
		TEST_ASSERT_EQUAL_INT(FSM_ACTION_MOVE_TO, r.action);
		TEST_ASSERT_EQUAL_INT(3, r.targetX);
		TEST_ASSERT_EQUAL_INT(5, r.targetZ);
		TEST_ASSERT_EQUAL_INT(3, f.goalX);
		TEST_ASSERT_EQUAL_INT(5, f.goalZ);
		TEST_ASSERT_EQUAL_INT(FSM_STATE_MOVING_TO, f.state);
	}

	/* fsmSetGoal + fsmStep(START) is equivalent to fsmStart. */
	fsmInit(&f);
	fsmSetGoal(&f, -2, 9);
	r = fsmStep(&f, FSM_EVENT_START);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_MOVE_TO, r.action);
	TEST_ASSERT_EQUAL_INT(-2, r.targetX);
	TEST_ASSERT_EQUAL_INT(9, r.targetZ);
}

/* ARRIVED from MOVING_TO emits BEGIN_ACTIVITY carrying the activity step. */
static void test_arrived_binds_activity(void)
{
	Fsm f;
	FsmResult r;

	fsmInit(&f);
	fsmStart(&f, 1, 1);
	f.step = 4;
	r = fsmStep(&f, FSM_EVENT_ARRIVED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_ACTING, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_BEGIN_ACTIVITY, r.action);
	TEST_ASSERT_EQUAL_INT(4, r.step);
	/* The other payloads stay zeroed. */
	TEST_ASSERT_EQUAL_INT(0, r.targetX);
	TEST_ASSERT_EQUAL_INT(0, r.waitReason);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.waitSeconds);
}

/* PHASE_DONE from ACTING hands off to WAITING with the wait payload. */
static void test_phase_done_waits(void)
{
	Fsm f;
	FsmResult r;

	fsmInit(&f);
	fsmStart(&f, 1, 1);
	fsmStep(&f, FSM_EVENT_ARRIVED);
	f.waitReason = 2;
	f.waitSeconds = 1.5f;
	r = fsmStep(&f, FSM_EVENT_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_WAITING, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_WAIT, r.action);
	TEST_ASSERT_EQUAL_INT(2, r.waitReason);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, r.waitSeconds);
	TEST_ASSERT_EQUAL_INT(0, r.step);
}

/* The full laundromat task cycle and its abort paths. */
static void test_task_sequence(void)
{
	Fsm f;
	FsmResult r;

	/* Move -> arrive -> act -> wait -> done. */
	fsmInit(&f);
	fsmStart(&f, 10, 3);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_MOVING_TO, f.state);
	r = fsmStep(&f, FSM_EVENT_ARRIVED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_ACTING, r.state);
	r = fsmStep(&f, FSM_EVENT_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_WAITING, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_WAIT, r.action);
	r = fsmStep(&f, FSM_EVENT_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_DONE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_FINISH_GOAL, r.action);
	/* The next task starts from DONE. */
	r = fsmStart(&f, 5, 5);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_MOVING_TO, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_MOVE_TO, r.action);

	/* DENIED while moving aborts with FINISH_GOAL. */
	fsmInit(&f);
	fsmStart(&f, 1, 2);
	r = fsmStep(&f, FSM_EVENT_DENIED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_DONE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_FINISH_GOAL, r.action);

	/* INTERRUPTED while acting aborts. */
	fsmInit(&f);
	fsmStart(&f, 1, 2);
	fsmStep(&f, FSM_EVENT_ARRIVED);
	r = fsmStep(&f, FSM_EVENT_INTERRUPTED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_DONE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_FINISH_GOAL, r.action);

	/* TIMEOUT while waiting aborts. */
	fsmInit(&f);
	fsmStart(&f, 1, 2);
	fsmStep(&f, FSM_EVENT_ARRIVED);
	fsmStep(&f, FSM_EVENT_PHASE_DONE);
	r = fsmStep(&f, FSM_EVENT_TIMEOUT);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_DONE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_FINISH_GOAL, r.action);
}

/* Illegal / out-of-range inputs are safe NONE no-ops. */
static void test_illegal_and_out_of_range(void)
{
	Fsm f;
	FsmResult r;

	fsmInit(&f);
	/* Illegal pair: ARRIVED in IDLE. */
	r = fsmStep(&f, FSM_EVENT_ARRIVED);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_NONE, r.action);

	/* Out-of-range event. */
	r = fsmStep(&f, 99);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_NONE, r.action);
	r = fsmStep(&f, -1);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, r.state);

	/* Out-of-range state: echoed unchanged, struct untouched. */
	f.state = 77;
	r = fsmStep(&f, FSM_EVENT_START);
	TEST_ASSERT_EQUAL_INT(77, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_NONE, r.action);
	TEST_ASSERT_EQUAL_INT(77, f.state);

	/* NULL fsm. */
	r = fsmStep(NULL, FSM_EVENT_START);
	TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, r.state);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_NONE, r.action);
	r = fsmStart(NULL, 1, 1);
	TEST_ASSERT_EQUAL_INT(FSM_ACTION_NONE, r.action);
	fsmInit(NULL);
	fsmSetGoal(NULL, 1, 1);	/* no crash */
}

/* Names and validity flags. */
static void test_names_and_validity(void)
{
	TEST_ASSERT_EQUAL_STRING("IDLE", fsmStateName(FSM_STATE_IDLE));
	TEST_ASSERT_EQUAL_STRING("MOVING_TO", fsmStateName(FSM_STATE_MOVING_TO));
	TEST_ASSERT_EQUAL_STRING("ACTING", fsmStateName(FSM_STATE_ACTING));
	TEST_ASSERT_EQUAL_STRING("WAITING", fsmStateName(FSM_STATE_WAITING));
	TEST_ASSERT_EQUAL_STRING("DONE", fsmStateName(FSM_STATE_DONE));
	TEST_ASSERT_EQUAL_STRING("?", fsmStateName(99));
	TEST_ASSERT_EQUAL_STRING("?", fsmStateName(-1));

	TEST_ASSERT_EQUAL_STRING("START", fsmEventName(FSM_EVENT_START));
	TEST_ASSERT_EQUAL_STRING("ARRIVED", fsmEventName(FSM_EVENT_ARRIVED));
	TEST_ASSERT_EQUAL_STRING("DENIED", fsmEventName(FSM_EVENT_DENIED));
	TEST_ASSERT_EQUAL_STRING("PHASE_DONE", fsmEventName(FSM_EVENT_PHASE_DONE));
	TEST_ASSERT_EQUAL_STRING("INTERRUPTED",
				 fsmEventName(FSM_EVENT_INTERRUPTED));
	TEST_ASSERT_EQUAL_STRING("TIMEOUT", fsmEventName(FSM_EVENT_TIMEOUT));
	TEST_ASSERT_EQUAL_STRING("?", fsmEventName(99));

	TEST_ASSERT_EQUAL_STRING("NONE", fsmActionName(FSM_ACTION_NONE));
	TEST_ASSERT_EQUAL_STRING("MOVE_TO", fsmActionName(FSM_ACTION_MOVE_TO));
	TEST_ASSERT_EQUAL_STRING("BEGIN_ACTIVITY",
				 fsmActionName(FSM_ACTION_BEGIN_ACTIVITY));
	TEST_ASSERT_EQUAL_STRING("WAIT", fsmActionName(FSM_ACTION_WAIT));
	TEST_ASSERT_EQUAL_STRING("FINISH_GOAL",
				 fsmActionName(FSM_ACTION_FINISH_GOAL));
	TEST_ASSERT_EQUAL_STRING("NONE", fsmActionName(99));

	TEST_ASSERT_TRUE(fsmStateValid(FSM_STATE_IDLE));
	TEST_ASSERT_TRUE(fsmStateValid(FSM_STATE_DONE));
	TEST_ASSERT_FALSE(fsmStateValid(-1));
	TEST_ASSERT_FALSE(fsmStateValid(FSM_STATE_COUNT));
	TEST_ASSERT_TRUE(fsmEventValid(FSM_EVENT_START));
	TEST_ASSERT_TRUE(fsmEventValid(FSM_EVENT_TIMEOUT));
	TEST_ASSERT_FALSE(fsmEventValid(-1));
	TEST_ASSERT_FALSE(fsmEventValid(FSM_EVENT_COUNT));
}

void run_test_fsm(void);

void run_test_fsm(void)
{
	RUN_TEST(test_full_table_every_cell);
	RUN_TEST(test_state_change_rule);
	RUN_TEST(test_start_payload_and_retarget);
	RUN_TEST(test_arrived_binds_activity);
	RUN_TEST(test_phase_done_waits);
	RUN_TEST(test_task_sequence);
	RUN_TEST(test_illegal_and_out_of_range);
	RUN_TEST(test_names_and_validity);
}
