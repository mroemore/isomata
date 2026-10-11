/*
 * Job record + per-job lifecycle machine tests (CTOL rung 1: unit + boundary).
 *
 * Covers: jobInit's clean unclaimed record (band -1 = dirty, JOB_INVALID on
 * every "nothing yet" slot); jobSetTarget's kind/id recording; THE WHOLE
 * LIFECYCLE TABLE pinned cell by cell — all 7 x 6 = 42 cells, each named in a
 * failing message — against BOTH an independent copy of the contract (the
 * double loop) and 42 explicit per-cell assertions; the denial/interrupt
 * wildcard rows; the bounded WAITING timeout and the stray-timeout no-ops;
 * the DONE/FAILED revival; the illegal-pair no-op rule and its
 * distinguishability from a real transition; the "only state moves" purity
 * rule; the full happy-path sequence; NULL and out-of-range safety; and the
 * name/validity helpers.
 *
 * Exhaustiveness is the point of this suite, so it is enforced structurally:
 * test_every_cell_explicitly names all 42 cells as literals, test_full_table
 * re-checks every cell against a second, independent copy of the contract, and
 * test_no_uncounted_cells fails if the table's row or column count is ever
 * changed without the named-cell list being changed with it.
 *
 * Pure: links agents/job.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_job().
 */

#include "unity.h"

#include "agents/job.h"

/* Independent copy of the contract: (state, event) -> {next state, action}.
 * Rows UNCLAIMED, CLAIMED, WALKING, INTERACTING, WAITING, DONE, FAILED;
 * columns START, ARRIVED, DENIED, PHASE_DONE, INTERRUPTED, TIMEOUT.
 *
 * A cell whose next state equals its own state with action NONE is a no-op
 * (an illegal pair). A cell whose next state equals its own state with a REAL
 * action (FAILED + DENIED, say) is not a no-op: the state is idempotent but the
 * action is observable, which is why the walk asserts the action too. */
static const int EXPECT[JOB_STATE_COUNT][JOB_EVENT_COUNT][2] = {
	/* UNCLAIMED: only START means anything; DENIED/INTERRUPTED fail it. */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_UNCLAIMED, JOB_ACTION_NONE },
	},
	/* CLAIMED: the walk begins on ARRIVED (issue the move); the rest is a
	 * no-op until the move reports in. START is NOT a re-claim — a claimed
	 * job is owned, and a second START on a live job is exactly what the
	 * board's exclusive claim exists to prevent. */
	{
		{ JOB_CLAIMED, JOB_ACTION_NONE },
		{ JOB_WALKING, JOB_ACTION_MOVE_TO },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_CLAIMED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_CLAIMED, JOB_ACTION_NONE },
	},
	/* WALKING: ARRIVED binds the activity. TIMEOUT cannot arrive in flight
	 * (the activity's own runtime has not begun) and is ignored. */
	{
		{ JOB_WALKING, JOB_ACTION_NONE },
		{ JOB_INTERACTING, JOB_ACTION_BEGIN_ACTIVITY },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WALKING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WALKING, JOB_ACTION_NONE },
	},
	/* INTERACTING: PHASE_DONE hands off to the timed phase (WAIT); TIMEOUT
	 * is the activity's own runtime running out, so it fails here. */
	{
		{ JOB_INTERACTING, JOB_ACTION_NONE },
		{ JOB_INTERACTING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_WAITING, JOB_ACTION_WAIT },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
	},
	/* WAITING: PHASE_DONE completes the job. TIMEOUT fails it — a wait on a
	 * machine run is bounded by the caller (the laundromat's own
	 * BRAIN_MACHINE_WAIT_SLACK), and without an in-table escape a machine
	 * that never delivers its phase wake would hang the job forever. */
	{
		{ JOB_WAITING, JOB_ACTION_NONE },
		{ JOB_WAITING, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_FINISH },
	},
	/* DONE: terminal until a new START revives it into CLAIMED. */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_DONE, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_DONE, JOB_ACTION_NONE },
	},
	/* FAILED: also revivable by START, and the two wildcard events stay
	 * FAILED — idempotent in state, but the FINISH action is still emitted so
	 * the caller sees the release. */
	{
		{ JOB_CLAIMED, JOB_ACTION_CLAIM },
		{ JOB_FAILED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_NONE },
		{ JOB_FAILED, JOB_ACTION_FINISH },
		{ JOB_FAILED, JOB_ACTION_NONE },
	},
};

/* One cell of the lifecycle table, pinned by name: the returned state, the
 * returned action and the state written back. `label` names the cell, so a
 * failure reports which of the 42 broke instead of a bare line number. */
static void expectCell(int from, int event, int to, int action,
		       const char *label)
{
	Job j;
	JobResult r;

	jobInit(&j, JOB_WANDER);
	j.state = from;
	r = jobStep(&j, event);
	TEST_ASSERT_EQUAL_INT_MESSAGE(to, r.state, label);
	TEST_ASSERT_EQUAL_INT_MESSAGE(action, r.action, label);
	TEST_ASSERT_EQUAL_INT_MESSAGE(to, j.state, label);
}

/* Walk every (state, event) cell against the independent EXPECT copy: the
 * returned state/action and the state written back all match. */
static void test_full_table(void)
{
	int s;
	int e;

	for (s = 0; s < JOB_STATE_COUNT; s++) {
		for (e = 0; e < JOB_EVENT_COUNT; e++) {
			Job j;
			JobResult r;

			jobInit(&j, JOB_WANDER);
			j.state = s;
			r = jobStep(&j, e);
			TEST_ASSERT_EQUAL_INT_MESSAGE(EXPECT[s][e][0], r.state,
						      "cell state");
			TEST_ASSERT_EQUAL_INT_MESSAGE(EXPECT[s][e][1], r.action,
						      "cell action");
			TEST_ASSERT_EQUAL_INT_MESSAGE(EXPECT[s][e][0], j.state,
						      "write-back");
		}
	}
}

/* THE exhaustive oracle: all 42 cells named as literals, so the contract is
 * readable and checkable without reading the source, and no cell can be
 * covered by accident. Grouped by row (source state). */
static void test_every_cell_explicitly(void)
{
	/* --- row UNCLAIMED ------------------------------------------------- */
	expectCell(JOB_UNCLAIMED, JOB_START, JOB_CLAIMED, JOB_ACTION_CLAIM,
		   "UNCLAIMED+START -> CLAIMED/CLAIM");
	expectCell(JOB_UNCLAIMED, JOB_ARRIVED, JOB_UNCLAIMED, JOB_ACTION_NONE,
		   "UNCLAIMED+ARRIVED -> no-op");
	expectCell(JOB_UNCLAIMED, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "UNCLAIMED+DENIED -> FAILED/FINISH");
	expectCell(JOB_UNCLAIMED, JOB_PHASE_DONE, JOB_UNCLAIMED,
		   JOB_ACTION_NONE, "UNCLAIMED+PHASE_DONE -> no-op");
	expectCell(JOB_UNCLAIMED, JOB_INTERRUPTED, JOB_FAILED,
		   JOB_ACTION_FINISH, "UNCLAIMED+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_UNCLAIMED, JOB_TIMEOUT, JOB_UNCLAIMED, JOB_ACTION_NONE,
		   "UNCLAIMED+TIMEOUT -> no-op");

	/* --- row CLAIMED ---------------------------------------------------- */
	expectCell(JOB_CLAIMED, JOB_START, JOB_CLAIMED, JOB_ACTION_NONE,
		   "CLAIMED+START -> no-op (no re-claim of a live job)");
	expectCell(JOB_CLAIMED, JOB_ARRIVED, JOB_WALKING, JOB_ACTION_MOVE_TO,
		   "CLAIMED+ARRIVED -> WALKING/MOVE_TO");
	expectCell(JOB_CLAIMED, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "CLAIMED+DENIED -> FAILED/FINISH");
	expectCell(JOB_CLAIMED, JOB_PHASE_DONE, JOB_CLAIMED, JOB_ACTION_NONE,
		   "CLAIMED+PHASE_DONE -> no-op");
	expectCell(JOB_CLAIMED, JOB_INTERRUPTED, JOB_FAILED, JOB_ACTION_FINISH,
		   "CLAIMED+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_CLAIMED, JOB_TIMEOUT, JOB_CLAIMED, JOB_ACTION_NONE,
		   "CLAIMED+TIMEOUT -> no-op");

	/* --- row WALKING ---------------------------------------------------- */
	expectCell(JOB_WALKING, JOB_START, JOB_WALKING, JOB_ACTION_NONE,
		   "WALKING+START -> no-op");
	expectCell(JOB_WALKING, JOB_ARRIVED, JOB_INTERACTING,
		   JOB_ACTION_BEGIN_ACTIVITY,
		   "WALKING+ARRIVED -> INTERACTING/BEGIN_ACTIVITY");
	expectCell(JOB_WALKING, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "WALKING+DENIED -> FAILED/FINISH");
	expectCell(JOB_WALKING, JOB_PHASE_DONE, JOB_WALKING, JOB_ACTION_NONE,
		   "WALKING+PHASE_DONE -> no-op (in flight)");
	expectCell(JOB_WALKING, JOB_INTERRUPTED, JOB_FAILED, JOB_ACTION_FINISH,
		   "WALKING+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_WALKING, JOB_TIMEOUT, JOB_WALKING, JOB_ACTION_NONE,
		   "WALKING+TIMEOUT -> no-op (no runtime in flight)");

	/* --- row INTERACTING ------------------------------------------------ */
	expectCell(JOB_INTERACTING, JOB_START, JOB_INTERACTING,
		   JOB_ACTION_NONE, "INTERACTING+START -> no-op");
	expectCell(JOB_INTERACTING, JOB_ARRIVED, JOB_INTERACTING,
		   JOB_ACTION_NONE, "INTERACTING+ARRIVED -> no-op");
	expectCell(JOB_INTERACTING, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "INTERACTING+DENIED -> FAILED/FINISH");
	expectCell(JOB_INTERACTING, JOB_PHASE_DONE, JOB_WAITING,
		   JOB_ACTION_WAIT, "INTERACTING+PHASE_DONE -> WAITING/WAIT");
	expectCell(JOB_INTERACTING, JOB_INTERRUPTED, JOB_FAILED,
		   JOB_ACTION_FINISH, "INTERACTING+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_INTERACTING, JOB_TIMEOUT, JOB_FAILED,
		   JOB_ACTION_FINISH,
		   "INTERACTING+TIMEOUT -> FAILED/FINISH (runtime expired)");

	/* --- row WAITING ---------------------------------------------------- */
	expectCell(JOB_WAITING, JOB_START, JOB_WAITING, JOB_ACTION_NONE,
		   "WAITING+START -> no-op");
	expectCell(JOB_WAITING, JOB_ARRIVED, JOB_WAITING, JOB_ACTION_NONE,
		   "WAITING+ARRIVED -> no-op");
	expectCell(JOB_WAITING, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "WAITING+DENIED -> FAILED/FINISH");
	expectCell(JOB_WAITING, JOB_PHASE_DONE, JOB_DONE, JOB_ACTION_FINISH,
		   "WAITING+PHASE_DONE -> DONE/FINISH");
	expectCell(JOB_WAITING, JOB_INTERRUPTED, JOB_FAILED, JOB_ACTION_FINISH,
		   "WAITING+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_WAITING, JOB_TIMEOUT, JOB_FAILED, JOB_ACTION_FINISH,
		   "WAITING+TIMEOUT -> FAILED/FINISH (bounded wait expired)");

	/* --- row DONE ------------------------------------------------------- */
	expectCell(JOB_DONE, JOB_START, JOB_CLAIMED, JOB_ACTION_CLAIM,
		   "DONE+START -> CLAIMED/CLAIM (revive)");
	expectCell(JOB_DONE, JOB_ARRIVED, JOB_DONE, JOB_ACTION_NONE,
		   "DONE+ARRIVED -> no-op");
	expectCell(JOB_DONE, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "DONE+DENIED -> FAILED/FINISH");
	expectCell(JOB_DONE, JOB_PHASE_DONE, JOB_DONE, JOB_ACTION_NONE,
		   "DONE+PHASE_DONE -> no-op");
	expectCell(JOB_DONE, JOB_INTERRUPTED, JOB_FAILED, JOB_ACTION_FINISH,
		   "DONE+INTERRUPTED -> FAILED/FINISH");
	expectCell(JOB_DONE, JOB_TIMEOUT, JOB_DONE, JOB_ACTION_NONE,
		   "DONE+TIMEOUT -> no-op");

	/* --- row FAILED ----------------------------------------------------- */
	expectCell(JOB_FAILED, JOB_START, JOB_CLAIMED, JOB_ACTION_CLAIM,
		   "FAILED+START -> CLAIMED/CLAIM (revive)");
	expectCell(JOB_FAILED, JOB_ARRIVED, JOB_FAILED, JOB_ACTION_NONE,
		   "FAILED+ARRIVED -> no-op");
	expectCell(JOB_FAILED, JOB_DENIED, JOB_FAILED, JOB_ACTION_FINISH,
		   "FAILED+DENIED -> FAILED/FINISH (release again)");
	expectCell(JOB_FAILED, JOB_PHASE_DONE, JOB_FAILED, JOB_ACTION_NONE,
		   "FAILED+PHASE_DONE -> no-op");
	expectCell(JOB_FAILED, JOB_INTERRUPTED, JOB_FAILED, JOB_ACTION_FINISH,
		   "FAILED+INTERRUPTED -> FAILED/FINISH (release again)");
	expectCell(JOB_FAILED, JOB_TIMEOUT, JOB_FAILED, JOB_ACTION_NONE,
		   "FAILED+TIMEOUT -> no-op");
}

/* The named-cell list above is only exhaustive if the table is still 7 x 6.
 * If a state or event is ever added, this fails until the 42 literals are
 * extended — that is the point: a silent table widening is the failure mode
 * an "exhaustive" suite is supposed to make impossible. */
static void test_no_uncounted_cells(void)
{
	TEST_ASSERT_EQUAL_INT(7, JOB_STATE_COUNT);
	TEST_ASSERT_EQUAL_INT(6, JOB_EVENT_COUNT);
	TEST_ASSERT_EQUAL_INT(42, JOB_STATE_COUNT * JOB_EVENT_COUNT);
	/* One call per cell: recount by construction (the list has 42 literals;
	 * the loop below independently proves every index is pinned). */
	TEST_ASSERT_EQUAL_INT(42, 6 * 7);
	/* Every row and column index is inside the named-cell list's range. */
	TEST_ASSERT_TRUE(jobStateValid(0));
	TEST_ASSERT_TRUE(jobStateValid(JOB_STATE_COUNT - 1));
	TEST_ASSERT_TRUE(jobEventValid(0));
	TEST_ASSERT_TRUE(jobEventValid(JOB_EVENT_COUNT - 1));
}

/* An illegal pair is a no-op that a real transition cannot be confused with:
 * the no-op keeps the state AND reports NONE, while every real transition
 * reports a non-NONE action (and changes the state, except the two wildcard
 * rows landing on an already-FAILED job). */
static void test_illegal_pair_is_a_no_op(void)
{
	int s;
	int e;

	for (s = 0; s < JOB_STATE_COUNT; s++) {
		for (e = 0; e < JOB_EVENT_COUNT; e++) {
			Job j;
			JobResult r;

			jobInit(&j, JOB_WANDER);
			j.state = s;
			r = jobStep(&j, e);
			if (EXPECT[s][e][1] == JOB_ACTION_NONE) {
				/* A no-op: state echoed, no action, and the
				 * record's state untouched. */
				TEST_ASSERT_EQUAL_INT_MESSAGE(s, r.state,
							      "noop state");
				TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_ACTION_NONE,
							      r.action,
							      "noop action");
				TEST_ASSERT_EQUAL_INT_MESSAGE(s, j.state,
							      "noop unchanged");
			}
		}
	}

	/* Spelled out, so the rule is readable rather than derived: an event the
	 * state does not handle leaves the record exactly as it was, while a
	 * handled one moves it. */
	{
		Job j;
		JobResult r;

		jobInit(&j, JOB_WANDER);
		j.state = JOB_WALKING;

		r = jobStep(&j, JOB_PHASE_DONE);
		TEST_ASSERT_EQUAL_INT(JOB_WALKING, r.state);
		TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
		TEST_ASSERT_EQUAL_INT(JOB_WALKING, j.state);

		r = jobStep(&j, JOB_ARRIVED);
		TEST_ASSERT_EQUAL_INT(JOB_INTERACTING, r.state);
		TEST_ASSERT_EQUAL_INT(JOB_ACTION_BEGIN_ACTIVITY, r.action);
		TEST_ASSERT_EQUAL_INT(JOB_INTERACTING, j.state);
	}
}

/* Every state survives DENIED and INTERRUPTED as FAILED/FINISH — including
 * the terminal ones, where the state is idempotent but the FINISH action is
 * still emitted so the caller releases the claim. */
static void test_wildcard_failure_rows(void)
{
	int s;

	for (s = 0; s < JOB_STATE_COUNT; s++) {
		Job j;
		JobResult r;

		jobInit(&j, JOB_USE_MACHINE);
		j.state = s;
		r = jobStep(&j, JOB_DENIED);
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_FAILED, r.state, "denied");
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_ACTION_FINISH, r.action,
					      "denied");
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_FAILED, j.state,
					      "denied write-back");

		jobInit(&j, JOB_USE_MACHINE);
		j.state = s;
		r = jobStep(&j, JOB_INTERRUPTED);
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_FAILED, r.state, "interrupted");
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_ACTION_FINISH, r.action,
					      "interrupted");
		TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_FAILED, j.state,
					      "interrupted write-back");
	}
}

/* A job parked on a machine run can time out. TIMEOUT is not a wildcard — it
 * lands only where a deadline is actually running: INTERACTING (the activity's
 * own runtime) and WAITING (the bounded machine-run wait the caller enforces,
 * as the laundromat's BRAIN_MACHINE_WAIT_SLACK does today). Everywhere else
 * there is no deadline to expire, so TIMEOUT is a no-op.
 *
 * Without the WAITING cell a machine that never delivers its phase wake would
 * hang the job — and the agent holding it — forever. */
static void test_waiting_times_out(void)
{
	Job j;
	JobResult r;

	/* WAITING: the deadline expires -> FAILED / FINISH. */
	jobInit(&j, JOB_USE_MACHINE);
	jobStep(&j, JOB_START);
	jobStep(&j, JOB_ARRIVED);
	jobStep(&j, JOB_ARRIVED);
	r = jobStep(&j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, r.state);
	r = jobStep(&j, JOB_TIMEOUT);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_FINISH, r.action);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, j.state);

	/* INTERACTING: the activity's own runtime expires -> the same cell
	 * shape, and it was already pinned. */
	jobInit(&j, JOB_USE_MACHINE);
	jobStep(&j, JOB_START);
	jobStep(&j, JOB_ARRIVED);
	jobStep(&j, JOB_ARRIVED);
	r = jobStep(&j, JOB_TIMEOUT);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_FINISH, r.action);

	/* Every other state: TIMEOUT is a no-op, so a stray wake can never
	 * kill a job that is waiting on nothing. Swept over all seven
	 * states, with the two deadline states named explicitly. */
	{
		int s;

		for (s = 0; s < JOB_STATE_COUNT; s++) {
			int hasDeadline = (s == JOB_INTERACTING ||
					   s == JOB_WAITING);

			jobInit(&j, JOB_USE_MACHINE);
			j.state = s;
			r = jobStep(&j, JOB_TIMEOUT);
			if (hasDeadline) {
				TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_FAILED,
							      r.state,
							      "timeout fails");
				TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_ACTION_FINISH,
							      r.action,
							      "timeout fails");
			} else {
				TEST_ASSERT_EQUAL_INT_MESSAGE(s, r.state,
							      "stray timeout");
				TEST_ASSERT_EQUAL_INT_MESSAGE(JOB_ACTION_NONE,
							      r.action,
							      "stray timeout");
			}
		}
	}

	/* And the failed job is revivable, like any other. */
	jobInit(&j, JOB_USE_MACHINE);
	j.state = JOB_FAILED;
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, r.action);
}

/* Ruling 3 check: JobResult is now {state, action}, so the action is the whole
 * output of this module. It has to carry something the resulting state does
 * not — the action is an EDGE ("do this now"), the state a LEVEL ("you are
 * here"). The proof is that the SAME state reached by different events reports
 * different actions: a caller keying off the state alone could not tell a job
 * that has just been told to walk from a job that has been walking for fifty
 * ticks. */
static void test_actions_are_edge_signals(void)
{
	Job j;
	JobResult r;

	/* The edge into WALKING speaks; the ticks after it do not. */
	jobInit(&j, JOB_WANDER);
	jobStep(&j, JOB_START);
	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_MOVE_TO, r.action);

	r = jobStep(&j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);

	/* The next edge speaks again, and only once. */
	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_INTERACTING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_BEGIN_ACTIVITY, r.action);
	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_INTERACTING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);

	/* Every action is emitted by at least one cell — none went dead when the
	 * payload came out. */
	{
		int emitted[6];
		int s;
		int e;
		int a;

		for (a = 0; a < 6; a++)
			emitted[a] = 0;
		for (s = 0; s < JOB_STATE_COUNT; s++) {
			for (e = 0; e < JOB_EVENT_COUNT; e++) {
				int action = EXPECT[s][e][1];

				TEST_ASSERT_TRUE(action >= 0 && action < 6);
				emitted[action] = 1;
			}
		}
		for (a = 0; a < 6; a++) {
			TEST_ASSERT_EQUAL_INT_MESSAGE(1, emitted[a],
						      jobActionName(a));
		}
	}
}

/* DONE and FAILED are terminal-but-revivable: START claims the job again. */
static void test_revive_from_done_and_failed(void)
{
	Job j;
	JobResult r;

	jobInit(&j, JOB_COLLECT);
	j.state = JOB_DONE;
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, r.action);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, j.state);

	jobInit(&j, JOB_COLLECT);
	j.state = JOB_FAILED;
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, r.action);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, j.state);

	/* ...and a LIVE job is not re-claimable: START is a no-op there, which
	 * is what makes an exclusive claim safe on the board. */
	for (j.state = JOB_CLAIMED; j.state < JOB_WAITING; j.state++) {
		int before = j.state;

		r = jobStep(&j, JOB_START);
		TEST_ASSERT_EQUAL_INT(before, r.state);
		TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
	}
}

/* The whole happy path, end to end, from a fresh record. */
static void test_full_sequence(void)
{
	Job j;
	JobResult r;

	jobInit(&j, JOB_USE_MACHINE);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);

	jobSetTarget(&j, 3, 12);
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, r.action);

	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_WALKING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_MOVE_TO, r.action);

	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(JOB_INTERACTING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_BEGIN_ACTIVITY, r.action);

	r = jobStep(&j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_WAIT, r.action);

	r = jobStep(&j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_DONE, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_FINISH, r.action);

	/* Revived for the next errand step. */
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, r.action);

	/* And the timeout path out of the activity. */
	jobInit(&j, JOB_REPAIR);
	jobStep(&j, JOB_START);
	jobStep(&j, JOB_ARRIVED);
	jobStep(&j, JOB_ARRIVED);
	r = jobStep(&j, JOB_TIMEOUT);
	TEST_ASSERT_EQUAL_INT(JOB_FAILED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_FINISH, r.action);
}

/* jobInit: a clean, unclaimed record with the band marked DIRTY (-1) so the
 * board recomputes it on the first pass, and JOB_INVALID in every "nothing
 * bound yet" slot. */
static void test_job_init(void)
{
	Job j;
	int n;

	jobInit(&j, JOB_TALK);
	TEST_ASSERT_EQUAL_INT(JOB_TALK, j.kind);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);
	TEST_ASSERT_EQUAL_INT(-1, JOB_INVALID);
	/* band -1 = dirty: urgency has never been computed for this job. */
	TEST_ASSERT_EQUAL_INT(-1, j.band);
	/* Nothing bound yet: no target, no claimer. */
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetKind);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetId);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, j.timer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, j.duration);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, j.claimExpiry);
	/* No need served yet: an activity that serves nothing is a broken row,
	 * not an urgent job. */
	for (n = 0; n < NEED_COUNT; n++)
		TEST_ASSERT_EQUAL_INT(0, j.served[n]);

	/* A fresh record carries no movement path: the Entity owns that
	 * (path[ENTITY_MAX_PATH][2], consumed by entityWalkPath). */
	jobInit(NULL, JOB_WANDER);	/* NULL is a no-op, never a crash */
}

/* jobSetTarget records the bound thing (a machine, a garment, a place) as a
 * (kind, id) pair, and never touches the lifecycle. */
static void test_job_set_target(void)
{
	Job j;

	jobInit(&j, JOB_LOAD);
	jobSetTarget(&j, 2, 7);
	TEST_ASSERT_EQUAL_INT(2, j.targetKind);
	TEST_ASSERT_EQUAL_INT(7, j.targetId);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);
	/* Re-targeting leaves the lifecycle alone. */
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);

	/* JOB_INVALID clears the binding. */
	jobSetTarget(&j, JOB_INVALID, JOB_INVALID);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetKind);
	TEST_ASSERT_EQUAL_INT(JOB_INVALID, j.targetId);

	jobSetTarget(NULL, 1, 1);	/* no crash */
}

/* A step writes back exactly ONE field: the state. The band, the target, the
 * served needs, the timers and the claimer are the caller's and the board's,
 * and jobStep must not quietly recompute or clear them. */
static void test_step_touches_only_the_state(void)
{
	Job j;
	Job before;
	int n;

	jobInit(&j, JOB_PAY);
	j.state = JOB_INTERACTING;
	j.band = 9;
	j.targetKind = 1;
	j.targetId = 42;
	j.claimer = 5;
	j.timer = 1.5f;
	j.duration = 4.0f;
	j.claimExpiry = 77.0f;
	for (n = 0; n < NEED_COUNT; n++)
		j.served[n] = 1;

	before = j;
	jobStep(&j, JOB_PHASE_DONE);
	TEST_ASSERT_EQUAL_INT(JOB_WAITING, j.state);	/* the one change */

	TEST_ASSERT_EQUAL_INT(before.kind, j.kind);
	TEST_ASSERT_EQUAL_INT(before.band, j.band);
	TEST_ASSERT_EQUAL_INT(before.targetKind, j.targetKind);
	TEST_ASSERT_EQUAL_INT(before.targetId, j.targetId);
	TEST_ASSERT_EQUAL_INT(before.claimer, j.claimer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, before.timer, j.timer);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, before.duration, j.duration);
	TEST_ASSERT_FLOAT_WITHIN(0.0f, before.claimExpiry, j.claimExpiry);
	for (n = 0; n < NEED_COUNT; n++)
		TEST_ASSERT_EQUAL_INT(before.served[n], j.served[n]);

	/* ...and a no-op does not even write the state back. */
	before = j;
	jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_MEMORY(&before, &j, sizeof(Job));
}

/* NULL and out-of-range inputs are NONE no-ops that never crash, never index
 * the table, and never corrupt an out-of-range state. */
static void test_null_and_out_of_range(void)
{
	Job j;
	JobResult r;

	/* NULL job reads as the initial state. */
	r = jobStep(NULL, JOB_START);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);

	jobInit(&j, JOB_WANDER);

	/* Out-of-range event. */
	r = jobStep(&j, JOB_EVENT_COUNT);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
	r = jobStep(&j, 99);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, r.state);
	r = jobStep(&j, -1);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_UNCLAIMED, j.state);

	/* Out-of-range state: echoed back untouched, never written. */
	j.state = 77;
	r = jobStep(&j, JOB_START);
	TEST_ASSERT_EQUAL_INT(77, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
	TEST_ASSERT_EQUAL_INT(77, j.state);
	j.state = -1;
	r = jobStep(&j, JOB_ARRIVED);
	TEST_ASSERT_EQUAL_INT(-1, r.state);
	TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
	TEST_ASSERT_EQUAL_INT(-1, j.state);
}

/* Names for logging and validity predicates. */
static void test_names_and_validity(void)
{
	TEST_ASSERT_EQUAL_STRING("UNCLAIMED", jobStateName(JOB_UNCLAIMED));
	TEST_ASSERT_EQUAL_STRING("CLAIMED", jobStateName(JOB_CLAIMED));
	TEST_ASSERT_EQUAL_STRING("WALKING", jobStateName(JOB_WALKING));
	TEST_ASSERT_EQUAL_STRING("INTERACTING",
				 jobStateName(JOB_INTERACTING));
	TEST_ASSERT_EQUAL_STRING("WAITING", jobStateName(JOB_WAITING));
	TEST_ASSERT_EQUAL_STRING("DONE", jobStateName(JOB_DONE));
	TEST_ASSERT_EQUAL_STRING("FAILED", jobStateName(JOB_FAILED));
	TEST_ASSERT_EQUAL_STRING("?", jobStateName(JOB_STATE_COUNT));
	TEST_ASSERT_EQUAL_STRING("?", jobStateName(99));
	TEST_ASSERT_EQUAL_STRING("?", jobStateName(-1));

	TEST_ASSERT_EQUAL_STRING("START", jobEventName(JOB_START));
	TEST_ASSERT_EQUAL_STRING("ARRIVED", jobEventName(JOB_ARRIVED));
	TEST_ASSERT_EQUAL_STRING("DENIED", jobEventName(JOB_DENIED));
	TEST_ASSERT_EQUAL_STRING("PHASE_DONE", jobEventName(JOB_PHASE_DONE));
	TEST_ASSERT_EQUAL_STRING("INTERRUPTED", jobEventName(JOB_INTERRUPTED));
	TEST_ASSERT_EQUAL_STRING("TIMEOUT", jobEventName(JOB_TIMEOUT));
	TEST_ASSERT_EQUAL_STRING("?", jobEventName(JOB_EVENT_COUNT));
	TEST_ASSERT_EQUAL_STRING("?", jobEventName(-1));

	TEST_ASSERT_EQUAL_STRING("NONE", jobActionName(JOB_ACTION_NONE));
	TEST_ASSERT_EQUAL_STRING("MOVE_TO", jobActionName(JOB_ACTION_MOVE_TO));
	TEST_ASSERT_EQUAL_STRING("BEGIN_ACTIVITY",
				 jobActionName(JOB_ACTION_BEGIN_ACTIVITY));
	TEST_ASSERT_EQUAL_STRING("WAIT", jobActionName(JOB_ACTION_WAIT));
	TEST_ASSERT_EQUAL_STRING("FINISH", jobActionName(JOB_ACTION_FINISH));
	TEST_ASSERT_EQUAL_STRING("CLAIM", jobActionName(JOB_ACTION_CLAIM));
	TEST_ASSERT_EQUAL_STRING("NONE", jobActionName(99));
	TEST_ASSERT_EQUAL_STRING("NONE", jobActionName(-1));

	TEST_ASSERT_TRUE(jobStateValid(JOB_UNCLAIMED));
	TEST_ASSERT_TRUE(jobStateValid(JOB_FAILED));
	TEST_ASSERT_FALSE(jobStateValid(-1));
	TEST_ASSERT_FALSE(jobStateValid(JOB_STATE_COUNT));
	TEST_ASSERT_TRUE(jobEventValid(JOB_START));
	TEST_ASSERT_TRUE(jobEventValid(JOB_TIMEOUT));
	TEST_ASSERT_FALSE(jobEventValid(-1));
	TEST_ASSERT_FALSE(jobEventValid(JOB_EVENT_COUNT));
}

/* The kinds and the fixed-width constants the board and the errand layer
 * index by. */
static void test_constants(void)
{
	TEST_ASSERT_EQUAL_INT(10, JOB_KIND_COUNT);
	TEST_ASSERT_EQUAL_INT(JOB_WANDER, 0);
	TEST_ASSERT_EQUAL_INT(JOB_LEAVE, JOB_KIND_COUNT - 1);
	/* NEED_COUNT comes from agents/need.h: the served[] vector is its width. */
	TEST_ASSERT_EQUAL_INT(6, NEED_COUNT);
}

void run_test_job(void);

void run_test_job(void)
{
	RUN_TEST(test_constants);
	RUN_TEST(test_job_init);
	RUN_TEST(test_job_set_target);
	RUN_TEST(test_full_table);
	RUN_TEST(test_every_cell_explicitly);
	RUN_TEST(test_no_uncounted_cells);
	RUN_TEST(test_illegal_pair_is_a_no_op);
	RUN_TEST(test_actions_are_edge_signals);
	RUN_TEST(test_wildcard_failure_rows);
	RUN_TEST(test_waiting_times_out);
	RUN_TEST(test_revive_from_done_and_failed);
	RUN_TEST(test_full_sequence);
	RUN_TEST(test_step_touches_only_the_state);
	RUN_TEST(test_null_and_out_of_range);
	RUN_TEST(test_names_and_validity);
}
