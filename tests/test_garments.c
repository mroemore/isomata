/*
 * Garment (carried-load) state tests (CTOL rung 1: unit + boundary).
 *
 * Pin the DIRTY -> WET_CLEAN -> DRY_CLEAN chain, the kind that must drive each
 * transition, the kind/transition mismatch rejection (a washer cannot dry, a
 * dryer cannot wash), the terminal state, NULL-safety and the name helper.
 *
 * Pure: links sim/garments.c + the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_garments().
 */

#include "unity.h"

#include "sim/garments.h"

static void test_names(void)
{
	TEST_ASSERT_EQUAL_STRING("DIRTY", garmentStateName(GARMENT_DIRTY));
	TEST_ASSERT_EQUAL_STRING("WET_CLEAN",
				 garmentStateName(GARMENT_WET_CLEAN));
	TEST_ASSERT_EQUAL_STRING("DRY_CLEAN",
				 garmentStateName(GARMENT_DRY_CLEAN));
	TEST_ASSERT_EQUAL_STRING("?", garmentStateName(-1));
	TEST_ASSERT_EQUAL_STRING("?", garmentStateName(99));
}

static void test_validity(void)
{
	TEST_ASSERT_TRUE(garmentStateValid(GARMENT_DIRTY));
	TEST_ASSERT_TRUE(garmentStateValid(GARMENT_WET_CLEAN));
	TEST_ASSERT_TRUE(garmentStateValid(GARMENT_DRY_CLEAN));
	TEST_ASSERT_FALSE(garmentStateValid(-1));
	TEST_ASSERT_FALSE(garmentStateValid(GARMENT_COUNT));
}

static void test_required_kind(void)
{
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_WASHER,
			      garmentRequiredKind(GARMENT_DIRTY));
	TEST_ASSERT_EQUAL_INT(MACHINE_KIND_DRYER,
			      garmentRequiredKind(GARMENT_WET_CLEAN));
	/* Terminal / unknown: no further machine needed. */
	TEST_ASSERT_EQUAL_INT(-1, garmentRequiredKind(GARMENT_DRY_CLEAN));
	TEST_ASSERT_EQUAL_INT(-1, garmentRequiredKind(99));
}

static void test_next_state_matrix(void)
{
	/* The exact 3x2 matrix. */
	TEST_ASSERT_EQUAL_INT(GARMENT_WET_CLEAN,
			      garmentNextState(GARMENT_DIRTY,
					       MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(GARMENT_DIRTY,
						   MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN,
			      garmentNextState(GARMENT_WET_CLEAN,
					       MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(GARMENT_WET_CLEAN,
						   MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(GARMENT_DRY_CLEAN,
						   MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(GARMENT_DRY_CLEAN,
						   MACHINE_KIND_DRYER));
	/* Unknown kind / unknown state. */
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(GARMENT_DIRTY, 42));
	TEST_ASSERT_EQUAL_INT(-1, garmentNextState(99, MACHINE_KIND_WASHER));
}

static void test_can_advance(void)
{
	TEST_ASSERT_TRUE(garmentCanAdvance(GARMENT_DIRTY, MACHINE_KIND_WASHER));
	TEST_ASSERT_FALSE(garmentCanAdvance(GARMENT_DIRTY, MACHINE_KIND_DRYER));
	TEST_ASSERT_TRUE(garmentCanAdvance(GARMENT_WET_CLEAN,
					   MACHINE_KIND_DRYER));
	TEST_ASSERT_FALSE(garmentCanAdvance(GARMENT_WET_CLEAN,
					    MACHINE_KIND_WASHER));
	TEST_ASSERT_FALSE(garmentCanAdvance(GARMENT_DRY_CLEAN,
					    MACHINE_KIND_DRYER));
	TEST_ASSERT_FALSE(garmentCanAdvance(GARMENT_DRY_CLEAN, 7));
}

static void test_advance_applies_and_rejects(void)
{
	int state = GARMENT_DIRTY;

	/* A mismatch leaves the state untouched. */
	TEST_ASSERT_FALSE(garmentAdvance(&state, MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_INT(GARMENT_DIRTY, state);

	/* The full chain, one legal run at a time. */
	TEST_ASSERT_TRUE(garmentAdvance(&state, MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_INT(GARMENT_WET_CLEAN, state);
	TEST_ASSERT_FALSE(garmentAdvance(&state, MACHINE_KIND_WASHER));
	TEST_ASSERT_EQUAL_INT(GARMENT_WET_CLEAN, state);
	TEST_ASSERT_TRUE(garmentAdvance(&state, MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, state);

	/* Terminal: nothing advances it. */
	TEST_ASSERT_FALSE(garmentAdvance(&state, MACHINE_KIND_WASHER));
	TEST_ASSERT_FALSE(garmentAdvance(&state, MACHINE_KIND_DRYER));
	TEST_ASSERT_EQUAL_INT(GARMENT_DRY_CLEAN, state);

	/* NULL-safe. */
	TEST_ASSERT_FALSE(garmentAdvance(NULL, MACHINE_KIND_WASHER));
}

void run_test_garments(void);

void run_test_garments(void)
{
	RUN_TEST(test_names);
	RUN_TEST(test_validity);
	RUN_TEST(test_required_kind);
	RUN_TEST(test_next_state_matrix);
	RUN_TEST(test_can_advance);
	RUN_TEST(test_advance_applies_and_rejects);
}
