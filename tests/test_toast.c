/*
 * Toast tests (CTOL rung 1: unit + boundary).
 *
 * Covers: hidden initial state, show -> visible/alpha 1, hold boundary at
 * TOAST_HOLD_SECONDS, fade alpha progression and completion at
 * TOAST_FADE_SECONDS, a single large dt crossing both phases, re-show
 * resetting timer + replacing text, update on hidden, ignored
 * non-positive/non-finite dt, NULL safety of every entry point, the draw
 * fill/text with alpha-scaled colors (full and half), hidden draw no-op,
 * empty text, and long-text safety.
 *
 * Pure: links only toast.c + element.c plus the Unity subset.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_toast().
 */

#include "unity.h"

#include "ui/toast.h"

#include "ui_test_util.h"

#include <math.h>
#include <string.h>

static int measure_8x16(void *ctx, const char *text, int *w, int *h)
{
	(void)ctx;
	*w = (int)strlen(text) * 8;
	*h = 16;
	return 0;
}

static void test_initial_hidden(void)
{
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *toast = uiCreateToast(&style);

	TEST_ASSERT_NOT_NULL(toast);
	TEST_ASSERT_FALSE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

static void test_show_becomes_visible_full_alpha(void)
{
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "hello");
	TEST_ASSERT_TRUE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	/* Still showing just under the hold. */
	toastUpdate(toast, TOAST_HOLD_SECONDS - 0.01f);
	TEST_ASSERT_TRUE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

/* Crossing the hold boundary enters FADING at alpha 1 (surplus carried). */
static void test_hold_boundary_enters_fade(void)
{
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "x");
	toastUpdate(toast, TOAST_HOLD_SECONDS);
	TEST_ASSERT_TRUE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

/* Fade alpha decreases linearly; end of fade hides. */
static void test_fade_progression_and_completion(void)
{
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "x");
	toastUpdate(toast, TOAST_HOLD_SECONDS);		/* enter fade */
	toastUpdate(toast, TOAST_FADE_SECONDS / 2.0f);
	TEST_ASSERT_TRUE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, toastAlpha(toast));

	toastUpdate(toast, TOAST_FADE_SECONDS / 2.0f);
	TEST_ASSERT_FALSE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

/* One large dt can pass through both phases. */
static void test_large_dt_completes_in_one_update(void)
{
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "x");
	toastUpdate(toast, TOAST_HOLD_SECONDS + TOAST_FADE_SECONDS + 1.0f);
	TEST_ASSERT_FALSE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

/* Re-show while fading resets to SHOWING with fresh alpha 1. */
static void test_reshow_resets(void)
{
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "first");
	toastUpdate(toast, TOAST_HOLD_SECONDS + TOAST_FADE_SECONDS / 2.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, toastAlpha(toast));

	toastShow(toast, "second");
	TEST_ASSERT_TRUE(toastVisible(toast));
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	/* Timer was reset: a near-hold dt keeps it showing. */
	toastUpdate(toast, TOAST_HOLD_SECONDS - 0.01f);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

/* update on a hidden toast does nothing; bad dt is ignored. */
static void test_update_noops(void)
{
	Element *toast = uiCreateToast(NULL);

	toastUpdate(toast, 10.0f);		/* hidden: no-op */
	TEST_ASSERT_FALSE(toastVisible(toast));

	toastShow(toast, "x");
	toastUpdate(toast, 0.0f);		/* zero: ignored */
	toastUpdate(toast, -5.0f);		/* negative: ignored */
	toastUpdate(toast, nanf(""));		/* NaN: ignored */
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, toastAlpha(toast));

	uiDestroyElement(toast);
}

static void test_null_safety(void)
{
	toastShow(NULL, "x");
	toastUpdate(NULL, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, toastAlpha(NULL));
	TEST_ASSERT_FALSE(toastVisible(NULL));
}

static void test_draw_showing_full_alpha(void)
{
	static TestDrawLog log;
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *toast = uiCreateToast(&style);

	toastShow(toast, "hi");
	uiSetRect(toast, 3, 4, 100, 20);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(toast, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_BACKGROUND, log.fills[0].rgba);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(3, log.texts[0].x);
	TEST_ASSERT_EQUAL_INT(4, log.texts[0].y);
	TEST_ASSERT_EQUAL_STRING("hi", log.texts[0].text);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_TEXT, log.texts[0].rgba);

	uiDestroyElement(toast);
}

/* Half-fade: both colors' alpha bytes are scaled by 0.5. */
static void test_draw_half_fade_scales_alpha(void)
{
	static TestDrawLog log;
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "hi");
	toastUpdate(toast, TOAST_HOLD_SECONDS + TOAST_FADE_SECONDS / 2.0f);
	uiSetRect(toast, 0, 0, 100, 20);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(toast, &ctx);

	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_UINT(0x30303073u, log.fills[0].rgba);	/* 230*0.5 */
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_UINT(0xFFFFFF80u, log.texts[0].rgba);	/* 255*0.5 -> 128 */

	uiDestroyElement(toast);
}

/* Hidden toast draws nothing. */
static void test_draw_hidden_is_noop(void)
{
	static TestDrawLog log;
	Element *toast = uiCreateToast(NULL);

	uiSetRect(toast, 0, 0, 100, 20);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(toast, &ctx);
	TEST_ASSERT_EQUAL_INT(0, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(toast);
}

/* Empty text: fill only. */
static void test_draw_empty_text(void)
{
	static TestDrawLog log;
	Element *toast = uiCreateToast(NULL);

	toastShow(toast, "");
	uiSetRect(toast, 0, 0, 100, 20);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(toast, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nFills);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(toast);
}

/* Long text is copied safely (recorder caps at TDL_TEXT_MAX-1). */
static void test_long_text_safe(void)
{
	static TestDrawLog log;
	Element *toast = uiCreateToast(NULL);
	char big[UI_TEXT_MAX + 20];
	int i;

	for (i = 0; i < (int)sizeof(big) - 1; i++)
		big[i] = 'a' + (i % 26);
	big[sizeof(big) - 1] = '\0';

	toastShow(toast, big);
	uiSetRect(toast, 0, 0, 100, 20);
	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(toast, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(TDL_TEXT_MAX - 1, (int)strlen(log.texts[0].text));

	uiDestroyElement(toast);
}

void run_test_toast(void);

void run_test_toast(void)
{
	RUN_TEST(test_initial_hidden);
	RUN_TEST(test_show_becomes_visible_full_alpha);
	RUN_TEST(test_hold_boundary_enters_fade);
	RUN_TEST(test_fade_progression_and_completion);
	RUN_TEST(test_large_dt_completes_in_one_update);
	RUN_TEST(test_reshow_resets);
	RUN_TEST(test_update_noops);
	RUN_TEST(test_null_safety);
	RUN_TEST(test_draw_showing_full_alpha);
	RUN_TEST(test_draw_half_fade_scales_alpha);
	RUN_TEST(test_draw_hidden_is_noop);
	RUN_TEST(test_draw_empty_text);
	RUN_TEST(test_long_text_safe);
}
