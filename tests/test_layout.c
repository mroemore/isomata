/*
 * Layout (pane) tests (CTOL rung 1: unit + boundary).
 *
 * Covers: axis enum, padding/gap insets with negative clamps, weighted
 * distribution on vertical and horizontal axes (cumulative-floor rule,
 * exact tiling), fixed children by current extent, weighted/fixed mixes,
 * empty pane, over-full pane (weighted collapse, fixed keep size), nested
 * panes (child rect set before its own layout runs), draw fill + child
 * draw order via the recording ctx, and NULL safety.
 *
 * Pure: links only layout.c + element.c plus the Unity subset; no SDL.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_layout().
 */

#include "unity.h"

#include "ui/layout.h"

#include "ui_test_util.h"

static Element *makePaneV(UiAxis axis, int padding, int gap)
{
	Element *pane = uiCreatePane(axis, padding, gap);

	TEST_ASSERT_NOT_NULL(pane);
	return pane;
}

/* All three children weighted 1,2,1: cumulative floors give 25/50/25. */
static void test_vertical_weighted_distribution(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *c = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int x, y, w, h;

	uiSetRect(pane, 0, 0, 100, 100);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);
	uiAppendChild(pane, c);
	uiSetWeight(a, 1);
	uiSetWeight(b, 2);
	uiSetWeight(c, 1);

	uiLayout(pane);

	uiGetRect(a, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(0, x);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(100, w);
	TEST_ASSERT_EQUAL_INT(25, h);

	uiGetRect(b, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(25, y);
	TEST_ASSERT_EQUAL_INT(50, h);

	uiGetRect(c, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(75, y);
	TEST_ASSERT_EQUAL_INT(25, h);

	uiDestroyElement(pane);
}

/* Padding insets content on all sides; gap separates (never pads edges).
 * 80 - 5 = 75 available for two equal weights -> 37 / 38. */
static void test_padding_and_gap(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 10, 5);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int x, y, w, h;

	uiSetRect(pane, 0, 0, 100, 100);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);
	uiSetWeight(a, 1);
	uiSetWeight(b, 1);

	uiLayout(pane);

	uiGetRect(a, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(10, x);
	TEST_ASSERT_EQUAL_INT(10, y);
	TEST_ASSERT_EQUAL_INT(80, w);
	TEST_ASSERT_EQUAL_INT(37, h);

	uiGetRect(b, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(10, x);
	TEST_ASSERT_EQUAL_INT(10 + 37 + 5, y);	/* gap applied */
	TEST_ASSERT_EQUAL_INT(80, w);
	TEST_ASSERT_EQUAL_INT(38, h);	/* exact tiling: 37 + 38 == 75 */

	uiDestroyElement(pane);
}

/* Fixed child (h=30) takes its extent; the rest (70) splits 2:1 as
 * 46 (floor 70*2/3) and 24. */
static void test_fixed_then_weighted(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *fixed = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *c = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int y, h;

	uiSetRect(pane, 0, 0, 100, 100);
	uiSetRect(fixed, 0, 0, 100, 30);	/* requested main extent */
	uiAppendChild(pane, fixed);
	uiAppendChild(pane, b);
	uiAppendChild(pane, c);
	uiSetWeight(b, 2);
	uiSetWeight(c, 1);

	uiLayout(pane);

	uiGetRect(fixed, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(30, h);

	uiGetRect(b, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(30, y);
	TEST_ASSERT_EQUAL_INT(46, h);

	uiGetRect(c, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(76, y);
	TEST_ASSERT_EQUAL_INT(24, h);

	uiDestroyElement(pane);
}

/* Weight 0 and negative weights are FIXED (extent kept); a default child
 * (never given a rect) is 0-sized. */
static void test_zero_and_negative_weight_are_fixed(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int y, h;

	uiSetRect(pane, 0, 0, 100, 100);
	uiSetRect(a, 0, 0, 100, 20);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);
	uiSetWeight(a, 0);
	uiSetWeight(b, -3);	/* treated as fixed */

	uiLayout(pane);

	uiGetRect(a, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(20, h);

	uiGetRect(b, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(20, y);
	TEST_ASSERT_EQUAL_INT(0, h);

	uiDestroyElement(pane);
}

/* Horizontal axis distributes width; the cross axis (height) is full. */
static void test_horizontal_axis(void)
{
	Element *pane = makePaneV(UI_AXIS_HORIZONTAL, 0, 0);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int x, y, w, h;

	uiSetRect(pane, 0, 0, 100, 40);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);
	uiSetWeight(a, 1);
	uiSetWeight(b, 3);

	uiLayout(pane);

	uiGetRect(a, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(0, x);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(25, w);
	TEST_ASSERT_EQUAL_INT(40, h);

	uiGetRect(b, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(25, x);
	TEST_ASSERT_EQUAL_INT(75, w);
	TEST_ASSERT_EQUAL_INT(40, h);

	uiDestroyElement(pane);
}

/* Negative padding/gap are clamped to 0; content never exceeds the rect. */
static void test_negative_padding_and_gap_clamp(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, -5, -7);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int x, y, w, h;

	uiSetRect(pane, 0, 0, 100, 100);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);
	uiSetWeight(a, 1);
	uiSetWeight(b, 1);

	uiLayout(pane);

	uiGetRect(a, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(0, x);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(100, w);
	TEST_ASSERT_EQUAL_INT(50, h);

	uiGetRect(b, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(50, y);
	TEST_ASSERT_EQUAL_INT(50, h);

	uiDestroyElement(pane);
}

/* Empty pane: no children, layout is a safe no-op. */
static void test_empty_pane(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 4, 4);

	uiSetRect(pane, 1, 2, 3, 4);
	uiLayout(pane);
	uiDestroyElement(pane);
}

/* Over-full pane: fixed children keep their extent and overflow; weighted
 * children collapse to 0 rather than going negative. */
static void test_overfull_pane_weighted_collapse(void)
{
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *fixed = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *w = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int y, h;

	uiSetRect(pane, 0, 0, 100, 10);
	uiSetRect(fixed, 0, 0, 100, 30);	/* alone exceeds the pane */
	uiAppendChild(pane, fixed);
	uiAppendChild(pane, w);
	uiSetWeight(w, 1);

	uiLayout(pane);

	uiGetRect(fixed, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(30, h);

	uiGetRect(w, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(30, y);
	TEST_ASSERT_EQUAL_INT(0, h);

	uiDestroyElement(pane);
}

/* A weighted pane child is resized by its parent, then the top-down walk
 * lets it arrange its own children inside that fresh rect. */
static void test_nested_pane_layout(void)
{
	Element *outer = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *inner = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *leaf = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	int y, h;

	uiSetRect(outer, 0, 0, 100, 100);
	uiAppendChild(outer, inner);
	uiAppendChild(inner, leaf);
	uiSetWeight(inner, 1);

	uiLayout(outer);

	/* inner spans the whole outer content ... */
	uiGetRect(inner, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(100, h);

	/* ... and leaf (weight 0, unset) filled the inner content. */
	uiGetRect(leaf, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(0, h);
	TEST_ASSERT_EQUAL_INT(100, leaf->w);

	uiDestroyElement(outer);
}

/* Draw: the pane fills its rect, then children draw in order. */
static void test_draw_fill_and_child_order(void)
{
	static TestDrawLog log;
	Element *pane = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *a = makePaneV(UI_AXIS_VERTICAL, 0, 0);
	Element *b = makePaneV(UI_AXIS_VERTICAL, 0, 0);

	uiSetRect(pane, 1, 2, 30, 40);
	uiSetRect(a, 1, 2, 30, 20);
	uiSetRect(b, 1, 22, 30, 20);
	uiAppendChild(pane, a);
	uiAppendChild(pane, b);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(pane, &ctx);

	/* pane, a, b: three background fills in that order. */
	TEST_ASSERT_EQUAL_INT(3, log.nFills);
	TEST_ASSERT_EQUAL_INT(1, log.fills[0].x);
	TEST_ASSERT_EQUAL_INT(2, log.fills[0].y);
	TEST_ASSERT_EQUAL_INT(30, log.fills[0].w);
	TEST_ASSERT_EQUAL_INT(40, log.fills[0].h);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BACKGROUND, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_INT(20, log.fills[1].h);
	TEST_ASSERT_EQUAL_INT(22, log.fills[2].y);

	uiDestroyElement(pane);
}

/* Axis enum size; an out-of-range axis still creates a usable (vertical-
 * behaving) pane rather than failing. */
static void test_axis_enum_and_out_of_range(void)
{
	Element *pane;

	TEST_ASSERT_EQUAL_INT(2, (int)UI_AXIS_COUNT);

	pane = uiCreatePane((UiAxis)99, 0, 0);
	TEST_ASSERT_NOT_NULL(pane);
	uiDestroyElement(pane);
}

/* Acceptance: a header/body/footer screen stays logically stable across a
 * wide and a portrait resolution — same tree, same insets, fixed bands
 * kept, the weighted body absorbs the difference. */
static void buildScreen(Element *root, Element *header, Element *body,
			Element *footer)
{
	uiSetRect(root, 0, 0, 0, 0);
	uiAppendChild(root, header);
	uiAppendChild(root, body);
	uiAppendChild(root, footer);
	uiSetRect(header, 0, 0, 0, 48);	/* fixed bands */
	uiSetRect(footer, 0, 0, 0, 32);
	uiSetWeight(body, 1);
}

static void test_wide_and_portrait_stability(void)
{
	Element *root = uiCreatePane(UI_AXIS_VERTICAL, 16, 8);
	Element *header = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	Element *body = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	Element *footer = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	int y, h;

	buildScreen(root, header, body, footer);

	/* Wide: 1920x1080, content 1888x1048, avail 1048-16-80 = 952. */
	uiSetRect(root, 0, 0, 1920, 1080);
	uiLayout(root);

	uiGetRect(header, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(16, y);
	TEST_ASSERT_EQUAL_INT(48, h);
	uiGetRect(body, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(72, y);
	TEST_ASSERT_EQUAL_INT(952, h);
	uiGetRect(footer, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(1032, y);
	TEST_ASSERT_EQUAL_INT(32, h);
	TEST_ASSERT_EQUAL_INT(1064, y + h);	/* footer bottom = H - pad */

	/* Portrait: 1080x1920, content 1048x1888, avail 1888-16-80 = 1792. */
	uiSetRect(root, 0, 0, 1080, 1920);
	uiLayout(root);

	uiGetRect(header, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(16, y);
	TEST_ASSERT_EQUAL_INT(48, h);
	uiGetRect(body, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(72, y);
	TEST_ASSERT_EQUAL_INT(1792, h);
	uiGetRect(footer, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(1872, y);
	TEST_ASSERT_EQUAL_INT(1904, y + h);	/* H - pad again */

	uiDestroyElement(root);
}

/* Safe-area bounds: every child stays inside the padded content rect. */
static void test_safe_area_insets_confine_children(void)
{
	Element *root = uiCreatePane(UI_AXIS_VERTICAL, 20, 6);
	Element *fixed = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	Element *weighted = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	Element *child;
	int x, y, w, h;

	uiSetRect(root, 0, 0, 400, 300);
	uiSetRect(fixed, 0, 0, 0, 40);
	uiAppendChild(root, fixed);
	uiAppendChild(root, weighted);
	uiSetWeight(weighted, 1);
	uiLayout(root);

	for (child = uiFirstChild(root); child != NULL; child = uiNextSibling(child)) {
		uiGetRect(child, &x, &y, &w, &h);
		TEST_ASSERT_TRUE(x >= 20);
		TEST_ASSERT_TRUE(y >= 20);
		TEST_ASSERT_TRUE(x + w <= 400 - 20);
		TEST_ASSERT_TRUE(y + h <= 300 - 20);
	}

	uiDestroyElement(root);
}

void run_test_layout(void);

void run_test_layout(void)
{
	RUN_TEST(test_vertical_weighted_distribution);
	RUN_TEST(test_padding_and_gap);
	RUN_TEST(test_fixed_then_weighted);
	RUN_TEST(test_zero_and_negative_weight_are_fixed);
	RUN_TEST(test_horizontal_axis);
	RUN_TEST(test_negative_padding_and_gap_clamp);
	RUN_TEST(test_empty_pane);
	RUN_TEST(test_overfull_pane_weighted_collapse);
	RUN_TEST(test_nested_pane_layout);
	RUN_TEST(test_draw_fill_and_child_order);
	RUN_TEST(test_axis_enum_and_out_of_range);
	RUN_TEST(test_wide_and_portrait_stability);
	RUN_TEST(test_safe_area_insets_confine_children);
}
