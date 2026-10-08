/*
 * ui_scale tests (CTOL rung 1: unit + boundary).
 *
 * Covers: density clamping at [1.0, 3.0] with the invalid-density rule
 * (NaN, <= 0, ±inf -> 1.0), lroundf rounding in both conversion
 * directions including half-away-from-zero ties and negatives, the
 * identity rule for invalid scales, and the edge-consistent rect rule
 * (corners scaled, w/h derived) at 1x/2x/fractional/invalid scales plus
 * the adjacent-rect-no-gap property. NULL rect pointers are no-ops.
 *
 * Pure: links only ui_scale.c plus the Unity subset; no SDL.
 *
 * Harness convention: belongs to the ONE headless executable (test_pure).
 * No main()/setUp()/tearDown(); exposes run_test_ui_scale().
 */

#include "unity.h"

#include "ui/ui_scale.h"

#include <math.h>

/* --- density clamping ------------------------------------------------------ */

static void test_density_identity_and_passthrough(void)
{
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(1.0f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, uiScaleFromDensity(1.25f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, uiScaleFromDensity(1.5f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.75f, uiScaleFromDensity(2.75f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, uiScaleFromDensity(3.0f));
}

/* Below the floor clamps up to 1.0; above the ceiling clamps down to 3.0. */
static void test_density_clamps_to_range(void)
{
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(0.75f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(0.0001f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, uiScaleFromDensity(3.5f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, uiScaleFromDensity(100.0f));
}

/* NaN, <= 0 and ±inf all degrade to the identity scale 1.0 (NEVER NaN, and
 * inf does not clamp to the ceiling — it is not a usable scale). */
static void test_density_invalid_values_yield_identity(void)
{
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(0.0f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(-1.0f));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(nanf("")));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(INFINITY));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, uiScaleFromDensity(-INFINITY));
}

/* --- point conversions ------------------------------------------------------- */

static void test_conversion_rounding_rules(void)
{
	/* lroundf: nearest, ties away from zero. */
	TEST_ASSERT_EQUAL_INT(10, uiScaleVirtualToPhysical(10, 1.0f));
	TEST_ASSERT_EQUAL_INT(15, uiScaleVirtualToPhysical(10, 1.5f));
	TEST_ASSERT_EQUAL_INT(20, uiScaleVirtualToPhysical(10, 2.0f));
	TEST_ASSERT_EQUAL_INT(30, uiScaleVirtualToPhysical(10, 3.0f));
	TEST_ASSERT_EQUAL_INT(28, uiScaleVirtualToPhysical(10, 2.75f));
	TEST_ASSERT_EQUAL_INT(3, uiScaleVirtualToPhysical(2, 1.5f));	/* 3.0 exact */
	TEST_ASSERT_EQUAL_INT(3, uiScaleVirtualToPhysical(2, 1.25f));	/* 2.5 -> 3 */
	TEST_ASSERT_EQUAL_INT(-8, uiScaleVirtualToPhysical(-5, 1.5f));	/* -7.5 -> -8 */

	TEST_ASSERT_EQUAL_INT(15, uiScalePhysicalToVirtual(15, 1.0f));
	TEST_ASSERT_EQUAL_INT(10, uiScalePhysicalToVirtual(15, 1.5f));
	TEST_ASSERT_EQUAL_INT(5, uiScalePhysicalToVirtual(15, 3.0f));
	TEST_ASSERT_EQUAL_INT(4, uiScalePhysicalToVirtual(10, 2.75f));	/* 3.636 -> 4 */
	TEST_ASSERT_EQUAL_INT(4, uiScalePhysicalToVirtual(7, 2.0f));	/* 3.5 -> 4 */
	TEST_ASSERT_EQUAL_INT(-4, uiScalePhysicalToVirtual(-6, 1.5f));	/* -4 exact */
}

/* NaN, <= 0 (and inf) scales are identity: the point never moves. */
static void test_conversion_invalid_scale_is_identity(void)
{
	TEST_ASSERT_EQUAL_INT(10, uiScaleVirtualToPhysical(10, 0.0f));
	TEST_ASSERT_EQUAL_INT(10, uiScaleVirtualToPhysical(10, -2.0f));
	TEST_ASSERT_EQUAL_INT(10, uiScaleVirtualToPhysical(10, nanf("")));
	TEST_ASSERT_EQUAL_INT(7, uiScalePhysicalToVirtual(7, 0.0f));
	TEST_ASSERT_EQUAL_INT(7, uiScalePhysicalToVirtual(7, -2.0f));
	TEST_ASSERT_EQUAL_INT(7, uiScalePhysicalToVirtual(7, nanf("")));
}

/* --- rect scaling ------------------------------------------------------------ */

static void test_scale_rect_at_known_scales(void)
{
	int x, y, w, h;

	/* 1.0: exact identity. */
	x = 10; y = 20; w = 30; h = 40;
	uiScaleRect(&x, &y, &w, &h, 1.0f);
	TEST_ASSERT_EQUAL_INT(10, x);
	TEST_ASSERT_EQUAL_INT(20, y);
	TEST_ASSERT_EQUAL_INT(30, w);
	TEST_ASSERT_EQUAL_INT(40, h);

	/* 2.0: right edge 40*2 = 80 -> w = 80-20 = 60; bottom 60*2 = 120. */
	x = 10; y = 20; w = 30; h = 40;
	uiScaleRect(&x, &y, &w, &h, 2.0f);
	TEST_ASSERT_EQUAL_INT(20, x);
	TEST_ASSERT_EQUAL_INT(40, y);
	TEST_ASSERT_EQUAL_INT(60, w);
	TEST_ASSERT_EQUAL_INT(80, h);

	/* 1.5 fractional: x 3->5 (4.5 rounds away from zero), right
	 * 10*1.5 = 15 -> w 10; y 4->6, bottom 13*1.5 = 19.5 -> 20 -> h 14. */
	x = 3; y = 4; w = 7; h = 9;
	uiScaleRect(&x, &y, &w, &h, 1.5f);
	TEST_ASSERT_EQUAL_INT(5, x);
	TEST_ASSERT_EQUAL_INT(6, y);
	TEST_ASSERT_EQUAL_INT(10, w);
	TEST_ASSERT_EQUAL_INT(14, h);

	/* 2.75 fractional: x 1->3, right 4*2.75 = 11 -> w 8; y 2->6
	 * (5.5 away), bottom 6*2.75 = 16.5 -> 17 -> h 11. */
	x = 1; y = 2; w = 3; h = 4;
	uiScaleRect(&x, &y, &w, &h, 2.75f);
	TEST_ASSERT_EQUAL_INT(3, x);
	TEST_ASSERT_EQUAL_INT(6, y);
	TEST_ASSERT_EQUAL_INT(8, w);
	TEST_ASSERT_EQUAL_INT(11, h);
}

/* THE edge-consistency property: two virtual rects sharing an edge must
 * share the physical edge, however rounding falls. */
static void test_scale_rect_adjacent_never_gap(void)
{
	int ax, ay, aw, ah;
	int bx, by, bw, bh;
	int scales[3] = { 2, 15, 275 };	/* 1.0f, 1.5f, 2.75f as *100 */
	int i;
	float scale;

	for (i = 0; i < 3; i++) {
		scale = scales[i] / 100.0f;

		/* A and B share the edge x=4 (A right edge, B left edge). */
		ax = 0; ay = 0; aw = 4; ah = 10;
		uiScaleRect(&ax, &ay, &aw, &ah, scale);
		bx = 4; by = 0; bw = 5; bh = 10;
		uiScaleRect(&bx, &by, &bw, &bh, scale);
		TEST_ASSERT_EQUAL_INT(ax + aw, bx);	/* shared physical edge */

		/* A and B share the edge y=6. */
		ax = 0; ay = 0; aw = 10; ah = 6;
		uiScaleRect(&ax, &ay, &aw, &ah, scale);
		bx = 0; by = 6; bw = 10; bh = 4;
		uiScaleRect(&bx, &by, &bw, &bh, scale);
		TEST_ASSERT_EQUAL_INT(ay + ah, by);
	}
}

/* Invalid scales leave the rect untouched (same identity rule as the
 * point conversions). */
static void test_scale_rect_invalid_scale_is_identity(void)
{
	int x, y, w, h;

	x = 3; y = 4; w = 7; h = 9;
	uiScaleRect(&x, &y, &w, &h, 0.0f);
	uiScaleRect(&x, &y, &w, &h, -1.0f);
	uiScaleRect(&x, &y, &w, &h, nanf(""));
	uiScaleRect(&x, &y, &w, &h, INFINITY);
	TEST_ASSERT_EQUAL_INT(3, x);
	TEST_ASSERT_EQUAL_INT(4, y);
	TEST_ASSERT_EQUAL_INT(7, w);
	TEST_ASSERT_EQUAL_INT(9, h);
}

/* NULL pointers are a no-op (no crash, no write). */
static void test_scale_rect_null_pointers_are_noop(void)
{
	int x = 1, y = 2, w = 3, h = 4;

	uiScaleRect(NULL, &y, &w, &h, 2.0f);
	TEST_ASSERT_EQUAL_INT(2, y);
	TEST_ASSERT_EQUAL_INT(3, w);
	uiScaleRect(&x, NULL, &w, &h, 2.0f);
	TEST_ASSERT_EQUAL_INT(1, x);
	uiScaleRect(&x, &y, NULL, &h, 2.0f);
	TEST_ASSERT_EQUAL_INT(4, h);
	uiScaleRect(&x, &y, &w, NULL, 2.0f);
	TEST_ASSERT_EQUAL_INT(1, x);
	TEST_ASSERT_EQUAL_INT(2, y);
	TEST_ASSERT_EQUAL_INT(3, w);
}

void run_test_ui_scale(void);

void run_test_ui_scale(void)
{
	RUN_TEST(test_density_identity_and_passthrough);
	RUN_TEST(test_density_clamps_to_range);
	RUN_TEST(test_density_invalid_values_yield_identity);
	RUN_TEST(test_conversion_rounding_rules);
	RUN_TEST(test_conversion_invalid_scale_is_identity);
	RUN_TEST(test_scale_rect_at_known_scales);
	RUN_TEST(test_scale_rect_adjacent_never_gap);
	RUN_TEST(test_scale_rect_invalid_scale_is_identity);
	RUN_TEST(test_scale_rect_null_pointers_are_noop);
}
