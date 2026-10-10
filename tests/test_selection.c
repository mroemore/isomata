/*
 * Machine-selection heuristic tests (CTOL rung 1: unit + boundary).
 *
 * Pin the exact integer scores of the three policies, the spread penalty
 * flipping the winner in a crafted world, the deterministic lowest-handle
 * tie-break, the candidate filters (kind, FREE-only, exclusion list), the
 * no-candidate and NULL paths and the policy name registry.
 *
 * Pure: links sim/selection.c + sim/machines.c + entities.c + easing.c + the
 * Unity subset. Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_selection().
 */

#include "unity.h"

#include "entities/entities.h"
#include "sim/machines.h"
#include "sim/selection.h"

/* A bare entity on a tile; selection only reads tileX/tileZ. */
static Entity atTile(int x, int z)
{
	Entity e = { 0 };

	e.tileX = x;
	e.tileZ = z;
	return e;
}

static MachineHandle addMachine(MachineRegistry *reg, int kind, int x, int z,
				int fee)
{
	return machineCreate(reg, kind, x, z, fee, 1.0f);
}

static void occupy(MachineRegistry *reg, MachineHandle h, int owner)
{
	Machine *m = machineGet(reg, h);

	m->owner = owner;
	m->state = MACHINE_STATE_RUNNING;
}

static void test_closest_exact_scores(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;
	MachineHandle a;
	MachineHandle b;

	machinesInit(&reg);
	a = addMachine(&reg, MACHINE_KIND_WASHER, 1, 0, 3);
	b = addMachine(&reg, MACHINE_KIND_WASHER, 4, 5, 3);
	selWorldInit(&w, &reg);

	TEST_ASSERT_EQUAL_INT(1, selScore(&e, machineGetConst(&reg, a), &w,
					  SEL_POLICY_CLOSEST));
	TEST_ASSERT_EQUAL_INT(9, selScore(&e, machineGetConst(&reg, b), &w,
					  SEL_POLICY_CLOSEST));
	/* Manhattan is absolute in both directions. */
	e = atTile(6, 6);
	TEST_ASSERT_EQUAL_INT(3, selScore(&e, machineGetConst(&reg, b), &w,
					  SEL_POLICY_CLOSEST));
	e = atTile(2, 3);	/* west and north of `a`: both deltas negative */
	TEST_ASSERT_EQUAL_INT(4, selScore(&e, machineGetConst(&reg, a), &w,
					  SEL_POLICY_CLOSEST));

	e = atTile(0, 0);
	TEST_ASSERT_EQUAL_INT(a, selChoose(&reg, &e, &w, SEL_POLICY_CLOSEST,
					   MACHINE_KIND_WASHER, NULL, 0));
}

static void test_cheapest_fee_first(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;
	MachineHandle near;
	MachineHandle far;

	machinesInit(&reg);
	near = addMachine(&reg, MACHINE_KIND_WASHER, 1, 0, 5);	/* close, dear */
	far = addMachine(&reg, MACHINE_KIND_WASHER, 6, 0, 2);	/* far, cheap */
	selWorldInit(&w, &reg);

	TEST_ASSERT_EQUAL_INT(5 * SEL_FEE_SCALE + 1, selScore(
		&e, machineGetConst(&reg, near), &w, SEL_POLICY_CHEAPEST));
	TEST_ASSERT_EQUAL_INT(2 * SEL_FEE_SCALE + 6, selScore(
		&e, machineGetConst(&reg, far), &w, SEL_POLICY_CHEAPEST));

	/* Fee dominates the farther distance. */
	TEST_ASSERT_EQUAL_INT(far, selChoose(&reg, &e, &w, SEL_POLICY_CHEAPEST,
					     MACHINE_KIND_WASHER, NULL, 0));

	/* Equal fees fall back to distance. */
	machineGet(&reg, far)->fee = 5;
	TEST_ASSERT_EQUAL_INT(near, selChoose(&reg, &e, &w, SEL_POLICY_CHEAPEST,
					      MACHINE_KIND_WASHER, NULL, 0));
}

static void test_spread_k_changes_the_winner(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;
	MachineHandle busy;
	MachineHandle near;
	MachineHandle far;

	machinesInit(&reg);
	busy = addMachine(&reg, MACHINE_KIND_WASHER, 2, 0, 3);	/* occupied */
	occupy(&reg, busy, 7);
	near = addMachine(&reg, MACHINE_KIND_WASHER, 1, 0, 3);	/* near busy */
	far = addMachine(&reg, MACHINE_KIND_WASHER, 6, 0, 3);	/* far away */
	selWorldInit(&w, &reg);		/* spreadK = 2 */

	/* closest prefers `near`; `near` is within K=2 of the occupied
	 * machine so spread_k penalises it and `far` wins. */
	TEST_ASSERT_EQUAL_INT(1, selScore(&e, machineGetConst(&reg, near), &w,
					  SEL_POLICY_CLOSEST));
	TEST_ASSERT_EQUAL_INT(near, selChoose(&reg, &e, &w, SEL_POLICY_CLOSEST,
					      MACHINE_KIND_WASHER, NULL, 0));
	TEST_ASSERT_EQUAL_INT(1 + SEL_SPREAD_PENALTY,
			      selScore(&e, machineGetConst(&reg, near), &w,
				       SEL_POLICY_SPREAD_K));
	TEST_ASSERT_EQUAL_INT(6, selScore(&e, machineGetConst(&reg, far), &w,
					  SEL_POLICY_SPREAD_K));
	TEST_ASSERT_EQUAL_INT(far, selChoose(&reg, &e, &w, SEL_POLICY_SPREAD_K,
					     MACHINE_KIND_WASHER, NULL, 0));

	/* K=0 disables the penalty: the plain nearest wins again. */
	w.spreadK = 0;
	TEST_ASSERT_EQUAL_INT(near, selChoose(&reg, &e, &w, SEL_POLICY_SPREAD_K,
					      MACHINE_KIND_WASHER, NULL, 0));

	/* K boundary: distance exactly K is still penalised. `near` is 1 tile
	 * from the occupied machine. */
	w.spreadK = 1;
	TEST_ASSERT_EQUAL_INT(1 + SEL_SPREAD_PENALTY,
			      selScore(&e, machineGetConst(&reg, near), &w,
				       SEL_POLICY_SPREAD_K));

	/* The candidate itself never counts as its own occupier: a free
	 * candidate scores unpenalised when no OTHER machine is occupied. */
	{
		MachineRegistry clean;
		MachineHandle only;

		machinesInit(&clean);
		only = addMachine(&clean, MACHINE_KIND_WASHER, 1, 0, 3);
		selWorldInit(&w, &clean);
		TEST_ASSERT_EQUAL_INT(1, selScore(
			&e, machineGetConst(&clean, only), &w,
			SEL_POLICY_SPREAD_K));
	}
}

static void test_spread_penalty_symmetry(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;
	MachineHandle busy;
	MachineHandle cand;

	machinesInit(&reg);
	/* Occupied machine BELOW the candidate in z, so selDistTiles takes the
	 * negative-dz arm. */
	busy = addMachine(&reg, MACHINE_KIND_WASHER, 1, 3, 3);
	occupy(&reg, busy, 5);
	cand = addMachine(&reg, MACHINE_KIND_WASHER, 1, 5, 3);
	selWorldInit(&w, &reg);

	TEST_ASSERT_EQUAL_INT(6 + SEL_SPREAD_PENALTY,
			      selScore(&e, machineGetConst(&reg, cand), &w,
				       SEL_POLICY_SPREAD_K));
}

static void test_tie_break_lowest_handle(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;

	machinesInit(&reg);
	/* Two equidistant free washers: (2,0) is handle 0, (0,2) handle 1. */
	addMachine(&reg, MACHINE_KIND_WASHER, 2, 0, 3);
	addMachine(&reg, MACHINE_KIND_WASHER, 0, 2, 3);
	selWorldInit(&w, &reg);

	TEST_ASSERT_EQUAL_INT(0, selChoose(&reg, &e, &w, SEL_POLICY_SPREAD_K,
					   MACHINE_KIND_WASHER, NULL, 0));
	TEST_ASSERT_EQUAL_INT(0, selChoose(&reg, &e, &w, SEL_POLICY_CHEAPEST,
					   MACHINE_KIND_WASHER, NULL, 0));
}

static void test_candidate_filters(void)
{
	MachineRegistry reg;
	Entity e = atTile(0, 0);
	SelWorld w;
	MachineHandle washer0;
	MachineHandle dryer0;
	MachineHandle washer1;
	MachineHandle washer2;
	MachineHandle excl[1];

	machinesInit(&reg);
	washer0 = addMachine(&reg, MACHINE_KIND_WASHER, 1, 0, 3);
	dryer0 = addMachine(&reg, MACHINE_KIND_DRYER, 1, 1, 2);
	washer1 = addMachine(&reg, MACHINE_KIND_WASHER, 3, 0, 3);
	washer2 = addMachine(&reg, MACHINE_KIND_WASHER, 4, 0, 3);
	selWorldInit(&w, &reg);

	/* Kind filter: a washer request never returns the dryer. */
	TEST_ASSERT_EQUAL_INT(washer0, selChoose(&reg, &e, &w,
						 SEL_POLICY_CLOSEST,
						 MACHINE_KIND_WASHER, NULL, 0));
	TEST_ASSERT_EQUAL_INT(dryer0, selChoose(&reg, &e, &w,
						SEL_POLICY_CLOSEST,
						MACHINE_KIND_DRYER, NULL, 0));

	/* FREE-only: an occupied or broken machine is not a candidate. */
	occupy(&reg, washer0, 4);
	TEST_ASSERT_EQUAL_INT(washer1, selChoose(&reg, &e, &w,
						 SEL_POLICY_CLOSEST,
						 MACHINE_KIND_WASHER, NULL, 0));
	machineGet(&reg, washer1)->state = MACHINE_STATE_BROKEN;
	TEST_ASSERT_EQUAL_INT(washer2, selChoose(&reg, &e, &w,
						 SEL_POLICY_CLOSEST,
						 MACHINE_KIND_WASHER, NULL, 0));

	/* Exclusion list drops a still-free machine. */
	excl[0] = washer2;
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      selChoose(&reg, &e, &w, SEL_POLICY_CLOSEST,
					MACHINE_KIND_WASHER, excl, 1));

	/* No candidates of the requested kind at all. */
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      selChoose(&reg, &e, &w, SEL_POLICY_CLOSEST, 99,
					NULL, 0));
}

static void test_no_candidate_and_null(void)
{
	MachineRegistry reg;
	MachineRegistry empty;
	Entity e = atTile(0, 0);
	SelWorld w;
	Machine m;

	machinesInit(&reg);
	machinesInit(&empty);
	selWorldInit(&w, &reg);
	m = (Machine){ 0 };
	m.kind = MACHINE_KIND_WASHER;
	m.tileX = 2;
	m.tileZ = 0;

	/* Empty registry / NULL registry / NULL entity -> nothing. */
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      selChoose(&empty, &e, &w, SEL_POLICY_CLOSEST,
					MACHINE_KIND_WASHER, NULL, 0));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      selChoose(NULL, &e, &w, SEL_POLICY_CLOSEST,
					MACHINE_KIND_WASHER, NULL, 0));
	TEST_ASSERT_EQUAL_INT(MACHINE_INVALID,
			      selChoose(&reg, NULL, &w, SEL_POLICY_CLOSEST,
					MACHINE_KIND_WASHER, NULL, 0));

	/* selScore guards. */
	TEST_ASSERT_EQUAL_INT(SEL_SCORE_INVALID,
			      selScore(NULL, &m, &w, SEL_POLICY_CLOSEST));
	TEST_ASSERT_EQUAL_INT(SEL_SCORE_INVALID,
			      selScore(&e, NULL, &w, SEL_POLICY_CLOSEST));
	TEST_ASSERT_EQUAL_INT(SEL_SCORE_INVALID,
			      selScore(&e, &m, &w, 99));
	TEST_ASSERT_EQUAL_INT(SEL_SCORE_INVALID,
			      selScore(&e, &m, &w, -1));

	/* A NULL world still scores (no occupancy data). */
	TEST_ASSERT_EQUAL_INT(2, selScore(&e, &m, NULL, SEL_POLICY_SPREAD_K));
	TEST_ASSERT_EQUAL_INT(2, selScore(&e, &m, NULL, SEL_POLICY_CLOSEST));
}

static void test_policy_names(void)
{
	TEST_ASSERT_EQUAL_STRING("closest", selPolicyName(SEL_POLICY_CLOSEST));
	TEST_ASSERT_EQUAL_STRING("spread_k", selPolicyName(SEL_POLICY_SPREAD_K));
	TEST_ASSERT_EQUAL_STRING("cheapest", selPolicyName(SEL_POLICY_CHEAPEST));
	TEST_ASSERT_EQUAL_STRING("?", selPolicyName(99));
	TEST_ASSERT_EQUAL_STRING("?", selPolicyName(-1));

	TEST_ASSERT_EQUAL_INT(SEL_POLICY_CLOSEST,
			      selPolicyByName("closest"));
	TEST_ASSERT_EQUAL_INT(SEL_POLICY_SPREAD_K,
			      selPolicyByName("spread_k"));
	TEST_ASSERT_EQUAL_INT(SEL_POLICY_CHEAPEST,
			      selPolicyByName("cheapest"));
	TEST_ASSERT_EQUAL_INT(-1, selPolicyByName("nope"));
	TEST_ASSERT_EQUAL_INT(-1, selPolicyByName(""));
	TEST_ASSERT_EQUAL_INT(-1, selPolicyByName(NULL));

	selWorldInit(NULL, NULL);	/* no crash */
	{
		SelWorld w;
		MachineRegistry reg;

		machinesInit(&reg);
		selWorldInit(&w, &reg);
		TEST_ASSERT_EQUAL_PTR(&reg, w.machines);
		TEST_ASSERT_EQUAL_INT(SEL_SPREAD_DEFAULT_K, w.spreadK);
	}

	TEST_ASSERT_TRUE(selPolicyValid(SEL_POLICY_CLOSEST));
	TEST_ASSERT_TRUE(selPolicyValid(SEL_POLICY_CHEAPEST));
	TEST_ASSERT_FALSE(selPolicyValid(-1));
	TEST_ASSERT_FALSE(selPolicyValid(SEL_POLICY_COUNT));
}

void run_test_selection(void);

void run_test_selection(void)
{
	RUN_TEST(test_closest_exact_scores);
	RUN_TEST(test_cheapest_fee_first);
	RUN_TEST(test_spread_k_changes_the_winner);
	RUN_TEST(test_spread_penalty_symmetry);
	RUN_TEST(test_tie_break_lowest_handle);
	RUN_TEST(test_candidate_filters);
	RUN_TEST(test_no_candidate_and_null);
	RUN_TEST(test_policy_names);
}
