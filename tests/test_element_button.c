/*
 * Button tests (CTOL rung 1: unit + boundary).
 *
 * Covers: pointer hit-test consumption (inside fires once, outside and
 * zero-area rects do nothing), UI_ACTIVATE firing and other intents
 * propagating, absent-callback safety across both activation paths, the
 * ctx round-trip, draw (fill color + centered text via the measure seam,
 * unmeasurable fallback, empty text), and NULL-argument creation.
 *
 * Pure: links only element_button.c + element.c plus the Unity subset.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_element_button().
 */

#include "unity.h"

#include "ui/element_button.h"

#include "ui_test_util.h"

#include <string.h>

static int g_fires;
static void *g_lastCtx;
static int g_ctxMarker;	/* identity token for the ctx round-trip */

static void countActivate(void *ctx)
{
	g_fires++;
	g_lastCtx = ctx;
}

static int measure_8x16(void *ctx, const char *text, int *w, int *h)
{
	(void)ctx;
	*w = (int)strlen(text) * 8;
	*h = 16;
	return 0;
}

/* A failing measure: the button must fall back to the top-left. */
static int measure_fails(void *ctx, const char *text, int *w, int *h)
{
	(void)ctx;
	(void)text;
	*w = 999;
	*h = 999;
	return 1;
}

static void resetFires(void)
{
	g_fires = 0;
	g_lastCtx = NULL;
}

static void test_pointer_inside_fires_once(void)
{
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *button = uiCreateButton("ok", &style, countActivate, &g_ctxMarker);

	uiSetRect(button, 10, 20, 100, 30);
	resetFires();

	TEST_ASSERT_TRUE(uiHandlePointer(button, 10, 20));	/* top-left edge */
	TEST_ASSERT_EQUAL_INT(1, g_fires);
	TEST_ASSERT_EQUAL_PTR(&g_ctxMarker, g_lastCtx);

	TEST_ASSERT_TRUE(uiHandlePointer(button, 109, 49));	/* last pixel */
	TEST_ASSERT_EQUAL_INT(2, g_fires);

	/* Outside: not consumed, no fire. */
	TEST_ASSERT_FALSE(uiHandlePointer(button, 110, 49));
	TEST_ASSERT_FALSE(uiHandlePointer(button, 9, 20));
	TEST_ASSERT_FALSE(uiHandlePointer(button, 10, 50));
	TEST_ASSERT_EQUAL_INT(2, g_fires);

	uiDestroyElement(button);
}

/* A zero-area button can never be tapped. */
static void test_pointer_zero_area_is_not_consumed(void)
{
	Element *button = uiCreateButton("x", NULL, countActivate, NULL);

	uiSetRect(button, 5, 5, 0, 0);
	resetFires();
	TEST_ASSERT_FALSE(uiHandlePointer(button, 5, 5));
	TEST_ASSERT_EQUAL_INT(0, g_fires);

	uiDestroyElement(button);
}

static void test_intent_activate_only(void)
{
	Element *button = uiCreateButton("go", NULL, countActivate, NULL);

	resetFires();
	TEST_ASSERT_TRUE(uiHandleIntent(button, UI_ACTIVATE));
	TEST_ASSERT_EQUAL_INT(1, g_fires);

	TEST_ASSERT_FALSE(uiHandleIntent(button, UI_NAV_UP));
	TEST_ASSERT_FALSE(uiHandleIntent(button, UI_NAV_DOWN));
	TEST_ASSERT_FALSE(uiHandleIntent(button, UI_NAV_LEFT));
	TEST_ASSERT_FALSE(uiHandleIntent(button, UI_NAV_RIGHT));
	TEST_ASSERT_FALSE(uiHandleIntent(button, UI_CANCEL));
	TEST_ASSERT_EQUAL_INT(1, g_fires);	/* none of those fired */

	uiDestroyElement(button);
}

/* Absent callback: both activation paths are still consumed, no crash. */
static void test_absent_callback_safe(void)
{
	Element *button = uiCreateButton("noop", NULL, NULL, NULL);

	uiSetRect(button, 0, 0, 10, 10);
	TEST_ASSERT_TRUE(uiHandlePointer(button, 5, 5));
	TEST_ASSERT_TRUE(uiHandleIntent(button, UI_ACTIVATE));

	uiDestroyElement(button);
}

static void test_draw_fill_and_centered_text(void)
{
	static TestDrawLog log;
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *button = uiCreateButton("ab", &style, NULL, NULL);

	uiSetRect(button, 0, 0, 100, 20);	/* text 16x16 -> (42, 2) */

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(button, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BUTTON, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_INT(0, log.fills[0].x);
	TEST_ASSERT_EQUAL_INT(0, log.fills[0].y);
	TEST_ASSERT_EQUAL_INT(100, log.fills[0].w);
	TEST_ASSERT_EQUAL_INT(20, log.fills[0].h);

	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(42, log.texts[0].x);
	TEST_ASSERT_EQUAL_INT(2, log.texts[0].y);
	TEST_ASSERT_EQUAL_STRING("ab", log.texts[0].text);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_TEXT, log.texts[0].rgba);

	uiDestroyElement(button);
}

/* Unmeasurable style: fill still happens, text falls back to top-left. */
static void test_draw_unmeasurable_fallback(void)
{
	static TestDrawLog log;
	TextStyle noMeasure = { 0 };
	TextStyle failing = { NULL, { measure_fails, NULL }, 16 };
	Element *a = uiCreateButton("hi", &noMeasure, NULL, NULL);
	Element *b = uiCreateButton("hi", &failing, NULL, NULL);

	uiSetRect(a, 3, 4, 50, 20);
	uiSetRect(b, 3, 4, 50, 20);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(a, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(3, log.texts[0].x);
	TEST_ASSERT_EQUAL_INT(4, log.texts[0].y);

	tdlReset(&log);
	ctx = tdlCtx(&log);
	uiDraw(b, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(3, log.texts[0].x);
	TEST_ASSERT_EQUAL_INT(4, log.texts[0].y);

	uiDestroyElement(a);
	uiDestroyElement(b);
}

/* Empty text: fill only, no drawText. */
static void test_draw_empty_text(void)
{
	static TestDrawLog log;
	Element *button = uiCreateButton("", NULL, NULL, NULL);

	uiSetRect(button, 0, 0, 10, 10);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(button, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(button);
}

/* NULL text/style/callback all tolerated at creation. */
static void test_create_all_null(void)
{
	Element *button = uiCreateButton(NULL, NULL, NULL, NULL);

	TEST_ASSERT_NOT_NULL(button);
	uiDestroyElement(button);
}

void run_test_element_button(void);

void run_test_element_button(void)
{
	RUN_TEST(test_pointer_inside_fires_once);
	RUN_TEST(test_pointer_zero_area_is_not_consumed);
	RUN_TEST(test_intent_activate_only);
	RUN_TEST(test_absent_callback_safe);
	RUN_TEST(test_draw_fill_and_centered_text);
	RUN_TEST(test_draw_unmeasurable_fallback);
	RUN_TEST(test_draw_empty_text);
	RUN_TEST(test_create_all_null);
}
