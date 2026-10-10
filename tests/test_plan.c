/*
 * Plan-template tests (CTOL rung 1: unit + boundary).
 *
 * Pin the LEAVE_WITH_CLEAN_DRY expansion (exact task order, machine kinds and
 * garment transitions), the unknown-goal rejection (leaves the output alone),
 * NULL-safety, the task accessor bounds and the name helpers.
 *
 * Pure: links sim/plan.c + sim/garments.c + sim/machines.c + the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_plan().
 */

#include "unity.h"

#include "sim/plan.h"

static void test_expand_leave_clean_dry(void)
{
	Plan p;
	const PlanTask *t;

	TEST_ASSERT_TRUE(planExpand(PLAN_GOAL_LEAVE_WITH_CLEAN_DRY, &p));
	TEST_ASSERT_EQUAL_INT(3, planCount(&p));
	TEST_ASSERT_EQUAL_INT(3, p.count);

	/* Task 0: wash. */
	t = planTaskAt(&p, 0);
	TEST_ASSERT_NOT_NULL(t);
	TEST_ASSERT_EQUAL_INT(PLAN_TASK_USE_MACHINE, t->kind);
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_WASHER, t->machineKind);
	TEST_ASSERT_EQUAL_INT(GARMENT_DIRTY, t->garmentFrom);
	TEST_ASSERT_EQUAL_INT(GARMENT_WET_CLEAN, t->garmentTo);

	/* Task 1: dry. */
	t = planTaskAt(&p, 1);
	TEST_ASSERT_NOT_NULL(t);
	TEST_ASSERT_EQUAL_INT(PLAN_TASK_USE_MACHINE, t->kind);
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_DRYER, t->machineKind);
	TEST_ASSERT_EQUAL_INT(GARMENT_WET_CLEAN, t->garmentFrom);
	TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, t->garmentTo);

	/* Task 2: leave. */
	t = planTaskAt(&p, 2);
	TEST_ASSERT_NOT_NULL(t);
	TEST_ASSERT_EQUAL_INT(PLAN_TASK_LEAVE, t->kind);
}

static void test_expand_rejects_unknown_and_null(void)
{
	Plan p;
	int i;

	/* Pre-fill so an "unchanged" claim is meaningful. */
	for (i = 0; i < PLAN_MAX_TASKS; i++) {
		p.tasks[i].kind = 12345;
		p.tasks[i].machineKind = 12345;
		p.tasks[i].garmentFrom = 12345;
		p.tasks[i].garmentTo = 12345;
	}
	p.count = 99;

	TEST_ASSERT_FALSE(planExpand(-1, &p));
	TEST_ASSERT_FALSE(planExpand(PLAN_GOAL_COUNT, &p));
	TEST_ASSERT_FALSE(planExpand(77, &p));
	TEST_ASSERT_EQUAL_INT(99, p.count);
	TEST_ASSERT_EQUAL_INT(12345, p.tasks[0].kind);

	TEST_ASSERT_FALSE(planExpand(PLAN_GOAL_LEAVE_WITH_CLEAN_DRY, NULL));
	TEST_ASSERT_FALSE(planExpand(PLAN_GOAL_NONE, NULL));
}

static void test_accessors_and_counts(void)
{
	Plan p;

	TEST_ASSERT_EQUAL_INT(0, planCount(NULL));
	TEST_ASSERT_NULL(planTaskAt(NULL, 0));

	TEST_ASSERT_TRUE(planExpand(PLAN_GOAL_LEAVE_WITH_CLEAN_DRY, &p));
	TEST_ASSERT_NOT_NULL(planTaskAt(&p, 0));
	TEST_ASSERT_NOT_NULL(planTaskAt(&p, 2));
	TEST_ASSERT_NULL(planTaskAt(&p, 3));
	TEST_ASSERT_NULL(planTaskAt(&p, -1));
}

static void test_names(void)
{
	TEST_ASSERT_EQUAL_STRING("NONE", planGoalName(PLAN_GOAL_NONE));
	TEST_ASSERT_EQUAL_STRING("LEAVE_WITH_CLEAN_DRY",
				 planGoalName(PLAN_GOAL_LEAVE_WITH_CLEAN_DRY));
	TEST_ASSERT_EQUAL_STRING("?", planGoalName(42));

	TEST_ASSERT_EQUAL_STRING("USE_MACHINE",
				 planTaskKindName(PLAN_TASK_USE_MACHINE));
	TEST_ASSERT_EQUAL_STRING("LEAVE", planTaskKindName(PLAN_TASK_LEAVE));
	TEST_ASSERT_EQUAL_STRING("?", planTaskKindName(42));
}

void run_test_plan(void);

void run_test_plan(void)
{
	RUN_TEST(test_expand_leave_clean_dry);
	RUN_TEST(test_expand_rejects_unknown_and_null);
	RUN_TEST(test_accessors_and_counts);
	RUN_TEST(test_names);
}
