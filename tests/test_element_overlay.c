/*
 * Overlay tests (CTOL rung 1: unit + boundary).
 *
 * Covers: creation, the exact dim fill color over the whole rect,
 * children drawn after the backdrop in order, child layout still running
 * top-down through the overlay, zero-area draw safety, and that the
 * overlay consumes no intent or pointer (so UI_CANCEL and taps reach
 * children/owner).
 *
 * Pure: links only element_overlay.c + element.c plus the Unity subset.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_element_overlay().
 */

#include "unity.h"

#include "ui/element_overlay.h"
#include "ui/element_button.h"
#include "ui/layout.h"

#include "ui_test_util.h"

static void test_create(void)
{
	Element *overlay = uiCreateOverlay();

	TEST_ASSERT_NOT_NULL(overlay);
	uiDestroyElement(overlay);
}

static void test_draw_dim_fill(void)
{
	static TestDrawLog log;
	Element *overlay = uiCreateOverlay();

	uiSetRect(overlay, 4, 5, 200, 100);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(overlay, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(4, log.fills[0].x);
	TEST_ASSERT_EQUAL_INT(5, log.fills[0].y);
	TEST_ASSERT_EQUAL_INT(200, log.fills[0].w);
	TEST_ASSERT_EQUAL_INT(100, log.fills[0].h);
	TEST_ASSERT_EQUAL_UINT(0x00000099u, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_OVERLAY, log.fills[0].rgba);

	uiDestroyElement(overlay);
}

/* Backdrop first, then children in order. */
static void test_children_draw_after_backdrop(void)
{
	static TestDrawLog log;
	Element *overlay = uiCreateOverlay();
	Element *a = uiCreateOverlay();
	Element *b = uiCreateOverlay();

	uiSetRect(overlay, 0, 0, 50, 50);
	uiSetRect(a, 0, 0, 10, 10);
	uiSetRect(b, 10, 0, 10, 10);
	uiAppendChild(overlay, a);
	uiAppendChild(overlay, b);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(overlay, &ctx);

	TEST_ASSERT_EQUAL_INT(3, log.nFills);
	TEST_ASSERT_EQUAL_INT(50, log.fills[0].w);	/* backdrop */
	TEST_ASSERT_EQUAL_INT(10, log.fills[1].w);	/* a */
	TEST_ASSERT_EQUAL_INT(10, log.fills[2].x);	/* b */

	uiDestroyElement(overlay);
}

/* The overlay has no layout, but the top-down walk still lets a child
 * container arrange its own children inside its rect. */
static void test_child_layout_runs_through_overlay(void)
{
	Element *overlay = uiCreateOverlay();
	Element *pane = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	Element *leaf = uiCreateOverlay();
	int y, h;

	uiSetRect(overlay, 0, 0, 100, 100);
	uiSetRect(pane, 0, 0, 100, 100);
	uiSetWeight(leaf, 1);
	uiAppendChild(overlay, pane);
	uiAppendChild(pane, leaf);

	uiLayout(overlay);

	uiGetRect(leaf, NULL, &y, NULL, &h);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_EQUAL_INT(100, h);

	uiDestroyElement(overlay);
}

/* Purely visual: never consumes an intent or a tap. */
static void test_consumes_nothing(void)
{
	Element *overlay = uiCreateOverlay();

	uiSetRect(overlay, 0, 0, 100, 100);
	TEST_ASSERT_FALSE(uiHandleIntent(overlay, UI_ACTIVATE));
	TEST_ASSERT_FALSE(uiHandleIntent(overlay, UI_CANCEL));
	TEST_ASSERT_FALSE(uiHandleIntent(overlay, UI_NAV_DOWN));
	TEST_ASSERT_FALSE(uiHandlePointer(overlay, 50, 50));

	uiDestroyElement(overlay);
}

/* Zero-area overlay still issues exactly one fill. */
static void test_zero_area_draw(void)
{
	static TestDrawLog log;
	Element *overlay = uiCreateOverlay();

	uiSetRect(overlay, 0, 0, 0, 0);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(overlay, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.fills[0].w);

	uiDestroyElement(overlay);
}

void run_test_element_overlay(void);

void run_test_element_overlay(void)
{
	RUN_TEST(test_create);
	RUN_TEST(test_draw_dim_fill);
	RUN_TEST(test_children_draw_after_backdrop);
	RUN_TEST(test_child_layout_runs_through_overlay);
	RUN_TEST(test_consumes_nothing);
	RUN_TEST(test_zero_area_draw);
}
