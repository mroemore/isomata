/*
 * Easing tests (CTOL rung 1: unit + boundary).
 *
 * Pins every curve at t = 0, 0.25, 0.5, 0.75, 1 (exact values), the clamp
 * behaviour below 0 / above 1, the NaN collapse to 0, and the unknown-enum
 * fallback to linear. The exact pins are the contract the tween layer relies
 * on, so they are written as closed-form expected numbers, not recomputed.
 *
 * Pure: links easing.c and the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_easing().
 */

#include "unity.h"

#include "entities/easing.h"

#include <math.h>
#include <string.h>

#define EPS 1e-6f

/* Linear: identity on [0, 1]; clamps outside; NaN -> 0. */
static void test_easing_linear(void)
{
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(EASE_LINEAR, 0.0f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, easingApply(EASE_LINEAR, 0.25f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, easingApply(EASE_LINEAR, 0.5f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, easingApply(EASE_LINEAR, 0.75f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(EASE_LINEAR, 1.0f));
}

/* Ease-in = t^2. */
static void test_easing_in(void)
{
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(EASE_IN, 0.0f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0625f, easingApply(EASE_IN, 0.25f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, easingApply(EASE_IN, 0.5f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5625f, easingApply(EASE_IN, 0.75f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(EASE_IN, 1.0f));
}

/* Ease-out = 1 - (1 - t)^2. */
static void test_easing_out(void)
{
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(EASE_OUT, 0.0f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.4375f, easingApply(EASE_OUT, 0.25f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, easingApply(EASE_OUT, 0.5f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.9375f, easingApply(EASE_OUT, 0.75f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(EASE_OUT, 1.0f));
}

/* Smoothstep 3t^2 - 2t^3. */
static void test_easing_in_out(void)
{
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(EASE_IN_OUT, 0.0f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.15625f,
				 easingApply(EASE_IN_OUT, 0.25f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, easingApply(EASE_IN_OUT, 0.5f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.84375f,
				 easingApply(EASE_IN_OUT, 0.75f));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(EASE_IN_OUT, 1.0f));
}

/* t is clamped into [0, 1] before every curve; NaN collapses to 0. */
static void test_easing_clamp(void)
{
	EasingFn fn;

	for (fn = EASE_LINEAR; fn < EASE_FN_COUNT; fn++) {
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(fn, -5.0f));
		TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(fn, 5.0f));
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,
					 easingApply(fn, (float)NAN));
		/* The clamped endpoints are exact for every curve. */
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, easingApply(fn, 0.0f));
		TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, easingApply(fn, 1.0f));
	}
}

/* An unknown enum value is linear (defensive, never crashes). */
static void test_easing_unknown_is_linear(void)
{
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.4f,
				 easingApply((EasingFn)99, 0.4f));
}

/* Two identical calls return bit-identical results (determinism pin). */
static void test_easing_deterministic(void)
{
	EasingFn fn;

	for (fn = EASE_LINEAR; fn < EASE_FN_COUNT; fn++) {
		float a = easingApply(fn, 0.37f);
		float b = easingApply(fn, 0.37f);

		TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof(a)));
	}
}

void run_test_easing(void);

void run_test_easing(void)
{
	RUN_TEST(test_easing_linear);
	RUN_TEST(test_easing_in);
	RUN_TEST(test_easing_out);
	RUN_TEST(test_easing_in_out);
	RUN_TEST(test_easing_clamp);
	RUN_TEST(test_easing_unknown_is_linear);
	RUN_TEST(test_easing_deterministic);
}
