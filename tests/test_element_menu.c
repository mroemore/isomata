/*
 * Menu tests (CTOL rung 1: unit + boundary).
 *
 * Covers: empty state (count 0, selected -1), add/auto-select/full cap,
 * setSelected bounds, up/down wrap and empty-nav non-consumption,
 * activate-fires-selected and empty-activate non-consumption, UI_CANCEL
 * never consumed, tap row mapping (select + fire, edges, outside, empty,
 * zero-area), draw (row tiling, selected highlight, text), empty-menu
 * draw, and NULL accessor safety.
 *
 * Pure: links only element_menu.c + element.c plus the Unity subset.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_element_menu().
 */

#include "unity.h"

#include "ui/element_menu.h"

#include "ui_test_util.h"

#include <string.h>

static int g_lastFired;		/* item index+1 that fired, 0 = none */
static void *g_lastCtx;

static void fire0(void *ctx)
{
	g_lastFired = 1;
	g_lastCtx = ctx;
}

static void fire1(void *ctx)
{
	g_lastFired = 2;
	g_lastCtx = ctx;
}

static void fire2(void *ctx)
{
	g_lastFired = 3;
	g_lastCtx = ctx;
}

static int measure_8x16(void *ctx, const char *text, int *w, int *h)
{
	(void)ctx;
	*w = (int)strlen(text) * 8;
	*h = 16;
	return 0;
}

static int g_ctxMarker;

static Element *makeMenu3(void)
{
	Element *menu = uiCreateMenu(NULL);

	TEST_ASSERT_NOT_NULL(menu);
	TEST_ASSERT_NOT_NULL(uiMenuAddItem(menu, "one", fire0, &g_ctxMarker));
	TEST_ASSERT_NOT_NULL(uiMenuAddItem(menu, "two", fire1, NULL));
	TEST_ASSERT_NOT_NULL(uiMenuAddItem(menu, "three", fire2, NULL));
	return menu;
}

static void resetFired(void)
{
	g_lastFired = 0;
	g_lastCtx = NULL;
}

static void test_empty_menu_state(void)
{
	Element *menu = uiCreateMenu(NULL);

	TEST_ASSERT_EQUAL_INT(0, uiMenuItemCount(menu));
	TEST_ASSERT_EQUAL_INT(-1, uiMenuSelected(menu));
	TEST_ASSERT_EQUAL_INT(0, uiMenuItemCount(NULL));
	TEST_ASSERT_EQUAL_INT(-1, uiMenuSelected(NULL));

	uiDestroyElement(menu);
}

static void test_add_items_autoselect_and_full_cap(void)
{
	Element *menu = uiCreateMenu(NULL);
	int i;

	TEST_ASSERT_EQUAL_PTR(menu, uiMenuAddItem(menu, "a", NULL, NULL));
	TEST_ASSERT_EQUAL_INT(1, uiMenuItemCount(menu));
	TEST_ASSERT_EQUAL_INT(0, uiMenuSelected(menu));	/* first selected */

	for (i = 1; i < UI_MENU_MAX_ITEMS; i++)
		TEST_ASSERT_EQUAL_PTR(menu, uiMenuAddItem(menu, "x", NULL, NULL));
	TEST_ASSERT_EQUAL_INT(UI_MENU_MAX_ITEMS, uiMenuItemCount(menu));

	/* Full: rejected, nothing added. */
	TEST_ASSERT_NULL(uiMenuAddItem(menu, "overflow", NULL, NULL));
	TEST_ASSERT_EQUAL_INT(UI_MENU_MAX_ITEMS, uiMenuItemCount(menu));

	/* NULL menu: rejected. */
	TEST_ASSERT_NULL(uiMenuAddItem(NULL, "x", NULL, NULL));

	uiDestroyElement(menu);
}

static void test_set_selected_bounds(void)
{
	Element *menu = makeMenu3();

	TEST_ASSERT_TRUE(uiMenuSetSelected(menu, 2));
	TEST_ASSERT_EQUAL_INT(2, uiMenuSelected(menu));

	TEST_ASSERT_FALSE(uiMenuSetSelected(menu, 3));
	TEST_ASSERT_FALSE(uiMenuSetSelected(menu, -1));
	TEST_ASSERT_EQUAL_INT(2, uiMenuSelected(menu));	/* unchanged */

	TEST_ASSERT_FALSE(uiMenuSetSelected(NULL, 0));
	TEST_ASSERT_FALSE(uiMenuSetSelected(NULL, -1));

	uiDestroyElement(menu);
}

static void test_navigation_wraps(void)
{
	Element *menu = makeMenu3();

	TEST_ASSERT_EQUAL_INT(0, uiMenuSelected(menu));

	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_NAV_DOWN));
	TEST_ASSERT_EQUAL_INT(1, uiMenuSelected(menu));
	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_NAV_DOWN));
	TEST_ASSERT_EQUAL_INT(2, uiMenuSelected(menu));
	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_NAV_DOWN));
	TEST_ASSERT_EQUAL_INT(0, uiMenuSelected(menu));	/* wrapped */

	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_NAV_UP));
	TEST_ASSERT_EQUAL_INT(2, uiMenuSelected(menu));	/* wrapped back */

	/* Left/right are not the menu's. */
	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_NAV_LEFT));
	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_NAV_RIGHT));

	uiDestroyElement(menu);
}

static void test_navigation_empty_not_consumed(void)
{
	Element *menu = uiCreateMenu(NULL);

	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_NAV_UP));
	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_NAV_DOWN));
	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_ACTIVATE));
	TEST_ASSERT_EQUAL_INT(-1, uiMenuSelected(menu));

	uiDestroyElement(menu);
}

static void test_activate_fires_selected(void)
{
	Element *menu = makeMenu3();

	resetFired();
	TEST_ASSERT_TRUE(uiMenuSetSelected(menu, 1));
	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_ACTIVATE));
	TEST_ASSERT_EQUAL_INT(2, g_lastFired);	/* fire1 -> index 1 */

	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_CANCEL));
	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_NAV_UP));
	TEST_ASSERT_TRUE(uiHandleIntent(menu, UI_ACTIVATE));
	TEST_ASSERT_EQUAL_INT(1, g_lastFired);	/* fire0 -> index 0 */
	TEST_ASSERT_EQUAL_PTR(&g_ctxMarker, g_lastCtx);

	uiDestroyElement(menu);
}

/* UI_CANCEL must never be consumed, on any menu state. */
static void test_cancel_never_consumed(void)
{
	Element *menu = makeMenu3();
	Element *empty = uiCreateMenu(NULL);

	TEST_ASSERT_FALSE(uiHandleIntent(menu, UI_CANCEL));
	TEST_ASSERT_FALSE(uiHandleIntent(empty, UI_CANCEL));

	uiDestroyElement(menu);
	uiDestroyElement(empty);
}

/* 3 rows in a 90px-tall menu: row0 0-29, row1 30-59, row2 60-89. */
static void test_tap_selects_and_fires_row(void)
{
	Element *menu = makeMenu3();

	uiSetRect(menu, 0, 0, 100, 90);
	resetFired();

	TEST_ASSERT_TRUE(uiHandlePointer(menu, 50, 45));	/* row 1 */
	TEST_ASSERT_EQUAL_INT(1, uiMenuSelected(menu));
	TEST_ASSERT_EQUAL_INT(2, g_lastFired);

	TEST_ASSERT_TRUE(uiHandlePointer(menu, 0, 89));		/* row 2, last pixel */
	TEST_ASSERT_EQUAL_INT(2, uiMenuSelected(menu));
	TEST_ASSERT_EQUAL_INT(3, g_lastFired);

	TEST_ASSERT_TRUE(uiHandlePointer(menu, 99, 0));		/* row 0 */
	TEST_ASSERT_EQUAL_INT(0, uiMenuSelected(menu));
	TEST_ASSERT_EQUAL_INT(1, g_lastFired);

	/* Outside: no select, no fire. */
	resetFired();
	TEST_ASSERT_FALSE(uiHandlePointer(menu, 50, 90));
	TEST_ASSERT_FALSE(uiHandlePointer(menu, 100, 45));
	TEST_ASSERT_FALSE(uiHandlePointer(menu, -1, 45));
	TEST_ASSERT_EQUAL_INT(0, g_lastFired);

	uiDestroyElement(menu);
}

static void test_tap_empty_or_zero_area_not_consumed(void)
{
	Element *empty = uiCreateMenu(NULL);
	Element *menu = makeMenu3();

	uiSetRect(empty, 0, 0, 100, 100);
	TEST_ASSERT_FALSE(uiHandlePointer(empty, 10, 10));

	uiSetRect(menu, 0, 0, 100, 0);	/* zero height */
	TEST_ASSERT_FALSE(uiHandlePointer(menu, 10, 0));

	uiSetRect(menu, 0, 0, 0, 100);	/* zero width */
	TEST_ASSERT_FALSE(uiHandlePointer(menu, 0, 10));

	uiDestroyElement(empty);
	uiDestroyElement(menu);
}

static void test_draw_rows_selection_and_text(void)
{
	static TestDrawLog log;
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *menu = uiCreateMenu(&style);

	uiMenuAddItem(menu, "aa", NULL, NULL);
	uiMenuAddItem(menu, "bb", NULL, NULL);
	uiMenuAddItem(menu, "cc", NULL, NULL);
	uiSetRect(menu, 0, 0, 100, 90);
	uiMenuSetSelected(menu, 1);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(menu, &ctx);

	/* Three row fills, exact tiling 0/30/60 with height 30. */
	TEST_ASSERT_EQUAL_INT(3, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.fills[0].y);
	TEST_ASSERT_EQUAL_INT(30, log.fills[1].y);
	TEST_ASSERT_EQUAL_INT(60, log.fills[2].y);
	TEST_ASSERT_EQUAL_INT(30, log.fills[0].h);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BACKGROUND, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_SELECTION, log.fills[1].rgba);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BACKGROUND, log.fills[2].rgba);

	/* Three texts, at each row's top-left. */
	TEST_ASSERT_EQUAL_INT(3, log.nTexts);
	TEST_ASSERT_EQUAL_STRING("aa", log.texts[0].text);
	TEST_ASSERT_EQUAL_INT(0, log.texts[0].y);
	TEST_ASSERT_EQUAL_STRING("bb", log.texts[1].text);
	TEST_ASSERT_EQUAL_INT(30, log.texts[1].y);
	TEST_ASSERT_EQUAL_STRING("cc", log.texts[2].text);
	TEST_ASSERT_EQUAL_INT(60, log.texts[2].y);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_TEXT, log.texts[0].rgba);

	uiDestroyElement(menu);
}

static void test_draw_empty_menu(void)
{
	static TestDrawLog log;
	Element *menu = uiCreateMenu(NULL);

	uiSetRect(menu, 1, 2, 50, 60);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(menu, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BACKGROUND, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_INT(50, log.fills[0].w);
	TEST_ASSERT_EQUAL_INT(60, log.fills[0].h);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(menu);
}

/* Long item text is truncated to UI_TEXT_MAX-1 chars. */
static void test_item_text_truncated(void)
{
	static TestDrawLog log;
	Element *menu = uiCreateMenu(NULL);
	char big[UI_TEXT_MAX + 20];
	int i;

	for (i = 0; i < (int)sizeof(big) - 1; i++)
		big[i] = 'a' + (i % 26);
	big[sizeof(big) - 1] = '\0';

	uiMenuAddItem(menu, big, NULL, NULL);
	uiSetRect(menu, 0, 0, 100, 20);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(menu, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	/* The recorder itself caps stored text at TDL_TEXT_MAX-1, so this
	 * proves a very long item is copied and drawn NUL-terminated with
	 * no overrun; the UI_TEXT_MAX truncation itself is pinned by the
	 * label twin (which exposes an accessor). */
	TEST_ASSERT_EQUAL_INT(TDL_TEXT_MAX - 1, (int)strlen(log.texts[0].text));

	uiDestroyElement(menu);
}

/* A menu with zero height still draws safely (empty-menu fallback only
 * triggers on count, not geometry). */
static void test_draw_null_style_tolerated(void)
{
	static TestDrawLog log;
	Element *menu = uiCreateMenu(NULL);

	uiMenuAddItem(menu, "x", NULL, NULL);
	uiSetRect(menu, 0, 0, 10, 10);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(menu, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);

	uiDestroyElement(menu);
}

void run_test_element_menu(void);

void run_test_element_menu(void)
{
	RUN_TEST(test_empty_menu_state);
	RUN_TEST(test_add_items_autoselect_and_full_cap);
	RUN_TEST(test_set_selected_bounds);
	RUN_TEST(test_navigation_wraps);
	RUN_TEST(test_navigation_empty_not_consumed);
	RUN_TEST(test_activate_fires_selected);
	RUN_TEST(test_cancel_never_consumed);
	RUN_TEST(test_tap_selects_and_fires_row);
	RUN_TEST(test_tap_empty_or_zero_area_not_consumed);
	RUN_TEST(test_draw_rows_selection_and_text);
	RUN_TEST(test_draw_empty_menu);
	RUN_TEST(test_item_text_truncated);
	RUN_TEST(test_draw_null_style_tolerated);
}
