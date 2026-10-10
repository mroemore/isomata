/*
 * Interactable-machine tests (CTOL rung 1: unit + boundary).
 *
 * Covers the state machine (FREE/CLAIMED/RUNNING/DONE/BROKEN), the exclusivity
 * rule (a claim only wins from FREE; every occupied state and BROKEN refuse;
 * same-tick double-claim loses), the illegal-transition no-change rule, run
 * timing with exact fake-dt boundaries, the PHASE_DONE / INTERRUPTED wake
 * events (including the wake cap and the owner-clearing break), release from
 * every state, repair, registry capacity/iteration, the name helpers and
 * NULL-safety, and run-to-run determinism.
 *
 * Pure: links sim/machines.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_machines().
 */

#include "unity.h"

#include "sim/machines.h"

/* A FREE machine in slot 0 of a one-machine registry; returns the handle. */
static MachineHandle one(MachineRegistry *reg, float runSecs)
{
	machinesInit(reg);
	return machineCreate(reg, MACHINE_KIND_WASHER, 3, 2, 4, runSecs);
}

/* ---- registry ---------------------------------------------------------- */

static void test_registry_create_capacity_iterate(void)
{
	MachineRegistry reg;
	MachineHandle h;
	int i;
	int seen = 0;

	machinesInit(&reg);
	TEST_ASSERT_EQUAL_INT(0, machineCount(&reg));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID, machineFirst(&reg));

	for (i = 0; i < MACHINE_MAX; i++)
		TEST_ASSERT_EQUAL_INT(i,
			machineCreate(&reg, i, i, 1, 1, 5.0f));
	TEST_ASSERT_EQUAL_INT(MACHINE_MAX, machineCount(&reg));
	/* Full: the next create fails cleanly. */
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      machineCreate(&reg, 0, 0, 0, 0, 0.0f));
	TEST_ASSERT_EQUAL_INT(MACHINE_MAX, machineCount(&reg));

	for (h = machineFirst(&reg); h != MACHINE_INVALID;
	     h = machineNext(&reg, h)) {
		TEST_ASSERT_EQUAL_INT(seen, h);
		seen++;
	}
	TEST_ASSERT_EQUAL_INT(MACHINE_MAX, seen);

	/* Fields stored verbatim; initial state. */
	{
		const Machine *m = machineGetConst(&reg, 3);

		TEST_ASSERT_NOT_NULL(m);
		TEST_ASSERT_EQUAL_INT(3, m->kind);
		TEST_ASSERT_EQUAL_INT(3, m->tileX);
		TEST_ASSERT_EQUAL_INT(1, m->tileZ);
		TEST_ASSERT_EQUAL_INT(1, m->fee);
		TEST_ASSERT_FLOAT_WITHIN(1e-6f, 5.0f, m->runSecs);
		TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m->runLeft);
		TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
		TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
		TEST_ASSERT_EQUAL_INT(3, m->handle);
	}

	/* Dead / out-of-range handles. */
	TEST_ASSERT_FALSE(machineAlive(&reg, MACHINE_INVALID));
	TEST_ASSERT_FALSE(machineAlive(&reg, MACHINE_MAX));
	TEST_ASSERT_NULL(machineGet(&reg, MACHINE_INVALID));
	TEST_ASSERT_NULL(machineGetConst(&reg, MACHINE_MAX));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      machineNext(&reg, MACHINE_MAX - 1));
}

static void test_registry_null_and_reuse(void)
{
	machinesInit(NULL);				/* no crash */
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      machineCreate(NULL, 0, 0, 0, 0, 0.0f));
	TEST_ASSERT_EQUAL_INT(0, machineCount(NULL));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID, machineFirst(NULL));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID, machineNext(NULL, -1));
	TEST_ASSERT_FALSE(machineAlive(NULL, 0));
	TEST_ASSERT_NULL(machineGet(NULL, 0));
	TEST_ASSERT_NULL(machineGetConst(NULL, 0));

	/* A valid in-range handle on a dead slot is not alive. */
	{
		MachineRegistry empty;

		machinesInit(&empty);
		TEST_ASSERT_FALSE(machineAlive(&empty, 0));
		TEST_ASSERT_NULL(machineGet(&empty, 0));
	}

	/* A freed slot is handed out again (there is no destroy in v1, so this
	 * only checks create picks the first dead slot). */
	{
		MachineRegistry reg;

		machinesInit(&reg);
		TEST_ASSERT_EQUAL_INT(0,
			machineCreate(&reg, 0, 0, 0, 0, 0.0f));
		reg.alive[0] = false;
		reg.liveCount--;
		TEST_ASSERT_EQUAL_INT(0,
			machineCreate(&reg, 1, 1, 1, 1, 1.0f));
		TEST_ASSERT_EQUAL_INT(1, machineCount(&reg));
	}
}

/* ---- claim / exclusivity ---------------------------------------------- */

static void test_claim_from_free_only(void)
{
	MachineRegistry reg;
	MachineHandle h = one(&reg, 5.0f);
	Machine *m = machineGet(&reg, h);
	static const int states[] = {
		MACHINE_STATE_CLAIMED,
		MACHINE_STATE_RUNNING,
		MACHINE_STATE_DONE,
		MACHINE_STATE_BROKEN,
	};

	/* The first claim wins and binds the owner. */
	TEST_ASSERT_TRUE(machineClaim(m, 7));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_CLAIMED, m->state);
	TEST_ASSERT_EQUAL_INT(7, m->owner);

	/* Every occupied / broken state refuses a fresh claim and the owner is
	 * untouched (the exclusivity guarantee, same-tick or not). */
	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		m->state = states[i];
		m->owner = 7;
		TEST_ASSERT_FALSE(machineClaim(m, 8));
		TEST_ASSERT_EQUAL_INT(states[i], m->state);
		TEST_ASSERT_EQUAL_INT(7, m->owner);
	}

	/* An invalid owner is refused even from FREE. */
	m->state = MACHINE_STATE_FREE;
	m->owner = MACHINE_NO_OWNER;
	TEST_ASSERT_FALSE(machineClaim(m, MACHINE_NO_OWNER));
	TEST_ASSERT_FALSE(machineClaim(m, -5));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);

	/* NULL-safe. */
	TEST_ASSERT_FALSE(machineClaim(NULL, 1));
}

/* Two claims for the same machine in one "tick": the second loses. */
static void test_double_claim_same_tick(void)
{
	MachineRegistry reg;
	Machine *m = machineGet(&reg, one(&reg, 5.0f));

	TEST_ASSERT_TRUE(machineClaim(m, 1));
	TEST_ASSERT_FALSE(machineClaim(m, 2));
	TEST_ASSERT_EQUAL_INT(1, m->owner);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_CLAIMED, m->state);
}

/* ---- start run / release / illegal transitions ------------------------- */

static void test_start_run_and_illegal(void)
{
	MachineRegistry reg;
	Machine *m = machineGet(&reg, one(&reg, 3.0f));

	/* Not claimable -> start refuses, state intact. */
	TEST_ASSERT_FALSE(machineStartRun(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);

	TEST_ASSERT_TRUE(machineClaim(m, 2));
	TEST_ASSERT_TRUE(machineStartRun(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, m->runLeft);

	/* Re-start refuses, timer intact. */
	TEST_ASSERT_FALSE(machineStartRun(m));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, m->runLeft);

	TEST_ASSERT_FALSE(machineStartRun(NULL));
}

static void test_release_from_each_state(void)
{
	MachineRegistry reg;
	Machine *m = machineGet(&reg, one(&reg, 3.0f));

	/* FREE refuses. */
	TEST_ASSERT_FALSE(machineRelease(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);

	/* CLAIMED / RUNNING / DONE all release to FREE and clear the owner. */
	m->state = MACHINE_STATE_CLAIMED;
	m->owner = 4;
	TEST_ASSERT_TRUE(machineRelease(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);

	m->state = MACHINE_STATE_RUNNING;
	m->owner = 4;
	m->runLeft = 1.5f;
	TEST_ASSERT_TRUE(machineRelease(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m->runLeft);

	m->state = MACHINE_STATE_DONE;
	m->owner = 4;
	TEST_ASSERT_TRUE(machineRelease(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);

	/* BROKEN refuses (needs repair). */
	m->state = MACHINE_STATE_BROKEN;
	TEST_ASSERT_FALSE(machineRelease(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN, m->state);

	TEST_ASSERT_FALSE(machineRelease(NULL));
}

/* ---- timers + wakes ---------------------------------------------------- */

static void test_run_completion_exact_boundary(void)
{
	MachineRegistry reg;
	MachineWake wakes[4];
	Machine *m = machineGet(&reg, one(&reg, 0.25f));
	int n;

	TEST_ASSERT_TRUE(machineClaim(m, 9));
	TEST_ASSERT_TRUE(machineStartRun(m));

	/* Three exact-binary 0.0625s steps: still running, no wake. */
	n = machinesUpdate(&reg, 0.0625f, wakes, 4);
	TEST_ASSERT_EQUAL_INT(0, n);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_TRUE(machinesUpdate(&reg, 0.0625f, wakes, 4) == 0);
	TEST_ASSERT_TRUE(machinesUpdate(&reg, 0.0625f, wakes, 4) == 0);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0625f, m->runLeft);

	/* The fourth lands exactly on t == runSecs: DONE + one PHASE_DONE wake
	 * carrying the machine handle and the owner. */
	n = machinesUpdate(&reg, 0.0625f, wakes, 4);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_DONE, m->state);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m->runLeft);
	TEST_ASSERT_EQUAL_INT(m->handle, wakes[0].machine);
	TEST_ASSERT_EQUAL_INT(9, wakes[0].owner);
	TEST_ASSERT_EQUAL_INT(MACHINE_WAKE_PHASE_DONE, wakes[0].kind);
	TEST_ASSERT_EQUAL_INT(9, m->owner);	/* owner valid in DONE */

	/* A further update does not re-wake a DONE machine. */
	n = machinesUpdate(&reg, 0.1f, wakes, 4);
	TEST_ASSERT_EQUAL_INT(0, n);
}

/* A non-power-of-two sequence: 3/32 + 3/32 lands exactly on the boundary. */
static void test_run_completion_uneven_steps(void)
{
	MachineRegistry reg;
	MachineWake wakes[2];
	Machine *m = machineGet(&reg, one(&reg, 0.1875f));

	TEST_ASSERT_TRUE(machineClaim(m, 1));
	TEST_ASSERT_TRUE(machineStartRun(m));
	TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&reg, 0.09375f, wakes, 2));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_EQUAL_INT(1, machinesUpdate(&reg, 0.09375f, wakes, 2));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_DONE, m->state);
}

static void test_update_dt_clamp_and_guards(void)
{
	MachineRegistry reg;
	MachineWake wakes[2];
	Machine *m = machineGet(&reg, one(&reg, 0.2f));

	TEST_ASSERT_TRUE(machineClaim(m, 1));
	TEST_ASSERT_TRUE(machineStartRun(m));

	/* A huge dt is clamped to MACHINE_MAX_DT: 0.2 -> 0.1 left. */
	TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&reg, 5.0f, wakes, 2));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_RUNNING, m->state);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, m->runLeft);

	/* A negative / NaN dt advances nothing (NaN-safe clamp). */
	TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&reg, -1.0f, wakes, 2));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, m->runLeft);
	{
		float nan = 0.0f;

		nan = nan / nan;	/* quiet NaN without <math.h> */
		TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&reg, nan, wakes, 2));
	}
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, m->runLeft);

	/* NULL registry / NULL out: no wakes; a NULL out still advances state. */
	TEST_ASSERT_EQUAL_INT(0, machinesUpdate(NULL, 0.1f, wakes, 2));
	TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&reg, 0.1f, NULL, 2));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_DONE, m->state);

	/* cap <= 0 with a fresh running machine: state advances, zero wakes. */
	{
		MachineRegistry r2;

		machinesInit(&r2);
		Machine *m2 = machineGet(&r2, one(&r2, 0.1f));

		TEST_ASSERT_TRUE(machineClaim(m2, 1));
		TEST_ASSERT_TRUE(machineStartRun(m2));
		TEST_ASSERT_EQUAL_INT(0, machinesUpdate(&r2, 0.1f, wakes, 0));
		TEST_ASSERT_EQUAL_INT(MACHINE_STATE_DONE, m2->state);
	}
}

static void test_wake_cap_is_a_prefix(void)
{
	MachineRegistry reg;
	MachineWake wakes[2];
	int i;

	machinesInit(&reg);
	for (i = 0; i < 3; i++) {
		Machine *m = machineGet(&reg,
			machineCreate(&reg, i, i, 0, 0, 0.1f));

		TEST_ASSERT_TRUE(machineClaim(m, 100 + i));
		TEST_ASSERT_TRUE(machineStartRun(m));
	}
	/* Three complete, cap is 2: the return and buffer are the prefix in
	 * slot order, and every machine still ends DONE. */
	TEST_ASSERT_EQUAL_INT(2, machinesUpdate(&reg, 0.1f, wakes, 2));
	TEST_ASSERT_EQUAL_INT(0, wakes[0].machine);
	TEST_ASSERT_EQUAL_INT(100, wakes[0].owner);
	TEST_ASSERT_EQUAL_INT(1, wakes[1].machine);
	TEST_ASSERT_EQUAL_INT(101, wakes[1].owner);
	for (i = 0; i < 3; i++)
		TEST_ASSERT_EQUAL_INT(MACHINE_STATE_DONE,
			machineGetConst(&reg, i)->state);
}

/* ---- break / interrupt / repair --------------------------------------- */

static void test_break_mid_run_emits_interrupt_and_refuses(void)
{
	MachineRegistry reg;
	MachineWake wake;
	Machine *m = machineGet(&reg, one(&reg, 30.0f));

	TEST_ASSERT_TRUE(machineClaim(m, 5));
	TEST_ASSERT_TRUE(machineStartRun(m));

	wake.machine = 999;	/* prove it is overwritten */
	TEST_ASSERT_TRUE(machineBreak(m, &wake));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, m->runLeft);
	TEST_ASSERT_EQUAL_INT(m->handle, wake.machine);
	TEST_ASSERT_EQUAL_INT(5, wake.owner);
	TEST_ASSERT_EQUAL_INT(MACHINE_WAKE_INTERRUPTED, wake.kind);

	/* BROKEN refuses a claim. */
	TEST_ASSERT_FALSE(machineClaim(m, 6));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN, m->state);

	/* Breaking with no owner stamps machine == MACHINE_INVALID (no wake). */
	wake.machine = 5;
	TEST_ASSERT_TRUE(machineBreak(m, &wake));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID, wake.machine);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, wake.owner);

	/* NULL out is fine; NULL machine refuses. */
	TEST_ASSERT_TRUE(machineBreak(m, NULL));
	TEST_ASSERT_FALSE(machineBreak(NULL, &wake));
}

static void test_break_from_every_state(void)
{
	static const int states[] = {
		MACHINE_STATE_FREE, MACHINE_STATE_CLAIMED,
		MACHINE_STATE_RUNNING, MACHINE_STATE_DONE,
		MACHINE_STATE_BROKEN,
	};

	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		MachineRegistry reg;
		MachineWake wake;
		Machine *m = machineGet(&reg, one(&reg, 4.0f));

		m->state = states[i];
		m->owner = states[i] == MACHINE_STATE_FREE ||
			   states[i] == MACHINE_STATE_BROKEN
			   ? MACHINE_NO_OWNER : 3;
		TEST_ASSERT_TRUE(machineBreak(m, &wake));
		TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN, m->state);
		TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);
	}
}

static void test_repair_reenables(void)
{
	MachineRegistry reg;
	MachineWake wake;
	Machine *m = machineGet(&reg, one(&reg, 4.0f));

	/* Repair on a non-broken machine refuses, state intact. */
	TEST_ASSERT_FALSE(machineRepair(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);

	TEST_ASSERT_TRUE(machineClaim(m, 1));
	TEST_ASSERT_TRUE(machineBreak(m, &wake));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_BROKEN, m->state);

	TEST_ASSERT_TRUE(machineRepair(m));
	TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, m->state);
	TEST_ASSERT_EQUAL_INT(MACHINE_NO_OWNER, m->owner);

	/* Repaired: claim works again. */
	TEST_ASSERT_TRUE(machineClaim(m, 2));
	TEST_ASSERT_FALSE(machineRepair(NULL));
}

/* ---- names / determinism ---------------------------------------------- */

static void test_names(void)
{
	TEST_ASSERT_EQUAL_STRING("FREE", machineStateName(MACHINE_STATE_FREE));
	TEST_ASSERT_EQUAL_STRING("CLAIMED",
				 machineStateName(MACHINE_STATE_CLAIMED));
	TEST_ASSERT_EQUAL_STRING("RUNNING",
				 machineStateName(MACHINE_STATE_RUNNING));
	TEST_ASSERT_EQUAL_STRING("DONE", machineStateName(MACHINE_STATE_DONE));
	TEST_ASSERT_EQUAL_STRING("BROKEN",
				 machineStateName(MACHINE_STATE_BROKEN));
	TEST_ASSERT_EQUAL_STRING("?", machineStateName(99));
	TEST_ASSERT_EQUAL_STRING("?", machineStateName(-1));

	TEST_ASSERT_EQUAL_STRING("WASHER",
				 machineKindName(MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_STRING("DRYER", machineKindName(MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_STRING("?", machineKindName(42));

	TEST_ASSERT_EQUAL_STRING("PHASE_DONE",
				 machineWakeKindName(MACHINE_WAKE_PHASE_DONE));
	TEST_ASSERT_EQUAL_STRING("INTERRUPTED",
				 machineWakeKindName(MACHINE_WAKE_INTERRUPTED));
	TEST_ASSERT_EQUAL_STRING("?", machineWakeKindName(7));
}

/* Apply one fixed script to a registry, collecting wakes; returns the count of
 * machine-update wakes (a break wake, if any, follows at w[count]). */
static int driveScript(MachineRegistry *reg, MachineWake *w)
{
	Machine *m0 = machineGet(reg, machineCreate(reg, 0, 1, 2, 3, 0.3f));
	Machine *m1 = machineGet(reg, machineCreate(reg, 1, 4, 5, 6, 0.05f));
	int n;

	machineClaim(m0, 10);
	machineStartRun(m0);
	machineClaim(m1, 11);
	machineStartRun(m1);
	n = machinesUpdate(reg, 0.1f, w, 4);	/* m1 (0.05s) completes */
	machineBreak(m0, &w[n]);		/* owner interrupted */
	return n;
}

/* Two identical registries driven identically end byte-identical. */
static void test_determinism(void)
{
	MachineRegistry a;
	MachineRegistry b;
	MachineWake wa[4] = { 0 };
	MachineWake wb[4] = { 0 };
	int na;
	int nb;

	machinesInit(&a);
	machinesInit(&b);
	na = driveScript(&a, wa);
	nb = driveScript(&b, wb);
	TEST_ASSERT_EQUAL_INT(na, nb);
	TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
	TEST_ASSERT_EQUAL_MEMORY(wa, wb, sizeof(wa));
}

void run_test_machines(void);

void run_test_machines(void)
{
	RUN_TEST(test_registry_create_capacity_iterate);
	RUN_TEST(test_registry_null_and_reuse);
	RUN_TEST(test_claim_from_free_only);
	RUN_TEST(test_double_claim_same_tick);
	RUN_TEST(test_start_run_and_illegal);
	RUN_TEST(test_release_from_each_state);
	RUN_TEST(test_run_completion_exact_boundary);
	RUN_TEST(test_run_completion_uneven_steps);
	RUN_TEST(test_update_dt_clamp_and_guards);
	RUN_TEST(test_wake_cap_is_a_prefix);
	RUN_TEST(test_break_mid_run_emits_interrupt_and_refuses);
	RUN_TEST(test_break_from_every_state);
	RUN_TEST(test_repair_reenables);
	RUN_TEST(test_names);
	RUN_TEST(test_determinism);
}
