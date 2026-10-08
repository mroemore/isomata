/*
 * Label tests (CTOL rung 1: unit + boundary).
 *
 * Covers: create with/without text and style, bounded UI_TEXT_MAX
 * truncation that always NUL-terminates, setText replacement and NULL =
 * empty, uiLabelText NULL-element handling, NULL style tolerance, the
 * draw call (position, text, color, style passthrough), empty text
 * drawing nothing, and that layout() is absent (children are not touched).
 *
 * Pure: links only element_label.c + element.c plus the Unity subset.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_element_label().
 */

#include "unity.h"

#include "ui/element_label.h"

#include "ui_test_util.h"

#include <string.h>

static Element *makeLabel(const char *text, const TextStyle *style)
{
	Element *label = uiCreateLabel(text, style);

	TEST_ASSERT_NOT_NULL(label);
	return label;
}

static int measure_8x16(void *ctx, const char *text, int *w, int *h)
{
	(void)ctx;
	*w = (int)strlen(text) * 8;
	*h = 16;
	return 0;
}

static void test_create_and_text_accessor(void)
{
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *label = makeLabel("hello", &style);

	TEST_ASSERT_EQUAL_STRING("hello", uiLabelText(label));
	TEST_ASSERT_NULL(uiLabelText(NULL));

	uiDestroyElement(label);
}

/* NULL text is the empty string; NULL style is tolerated (zeroed). */
static void test_create_null_text_and_style(void)
{
	Element *label = makeLabel(NULL, NULL);

	TEST_ASSERT_EQUAL_STRING("", uiLabelText(label));

	uiDestroyElement(label);
}

/* One past UI_TEXT_MAX-1 chars is truncated, keeping the tail NUL. */
static void test_text_is_truncated_at_cap(void)
{
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *label = makeLabel("x", &style);
	char big[UI_TEXT_MAX + 40];
	int i;

	for (i = 0; i < (int)sizeof(big) - 1; i++)
		big[i] = 'a' + (i % 26);
	big[sizeof(big) - 1] = '\0';

	uiLabelSetText(label, big);
	TEST_ASSERT_EQUAL_INT((int)UI_TEXT_MAX - 1, (int)strlen(uiLabelText(label)));
	TEST_ASSERT_EQUAL_INT('a', (int)uiLabelText(label)[0]);

	uiDestroyElement(label);
}

static void test_set_text_replaces_and_null_clears(void)
{
	Element *label = makeLabel("first", NULL);

	uiLabelSetText(label, "second");
	TEST_ASSERT_EQUAL_STRING("second", uiLabelText(label));

	uiLabelSetText(label, NULL);
	TEST_ASSERT_EQUAL_STRING("", uiLabelText(label));

	uiLabelSetText(NULL, "ignored");	/* NULL element: no-op */

	uiDestroyElement(label);
}

static void test_draw_calls_draw_text_with_style_and_color(void)
{
	static TestDrawLog log;
	TextStyle style = { NULL, { measure_8x16, NULL }, 16 };
	Element *label = makeLabel("hi", &style);

	uiSetRect(label, 5, 7, 40, 16);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(label, &ctx);

	TEST_ASSERT_EQUAL_INT(0, log.nFills);
	TEST_ASSERT_EQUAL_INT(1, log.nTexts);
	TEST_ASSERT_EQUAL_INT(5, log.texts[0].x);
	TEST_ASSERT_EQUAL_INT(7, log.texts[0].y);
	TEST_ASSERT_EQUAL_STRING("hi", log.texts[0].text);
	TEST_ASSERT_EQUAL_UINT(UI_COLOR_TEXT, log.texts[0].rgba);
	/* Style passthrough: the label forwards its own TextStyle, not a
	 * default (pixelSize + the injected measure fn are the observable
	 * fields; font is NULL in a pure test). */
	TEST_ASSERT_EQUAL_INT(16, log.texts[0].stylePixelSize);
	TEST_ASSERT_NULL(log.texts[0].styleFont);
	TEST_ASSERT_TRUE(log.texts[0].styleMeasureFn == measure_8x16);

	uiDestroyElement(label);
}

/* Empty text draws nothing (no drawText call). */
static void test_draw_empty_text_is_noop(void)
{
	static TestDrawLog log;
	Element *label = makeLabel("", NULL);

	tdlReset(&log);
	UiDrawCtx ctx = tdlCtx(&log);
	uiDraw(label, &ctx);

	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	uiDestroyElement(label);
}

/* A label has no layout callback: uiLayout leaves its (zero) rect alone. */
static void test_layout_is_absent(void)
{
	Element *label = makeLabel("x", NULL);
	int x, y, w, h;

	uiSetRect(label, 1, 2, 3, 4);
	uiLayout(label);
	uiGetRect(label, &x, &y, &w, &h);
	TEST_ASSERT_EQUAL_INT(1, x);
	TEST_ASSERT_EQUAL_INT(2, y);
	TEST_ASSERT_EQUAL_INT(3, w);
	TEST_ASSERT_EQUAL_INT(4, h);

	uiDestroyElement(label);
}

void run_test_element_label(void);

void run_test_element_label(void)
{
	RUN_TEST(test_create_and_text_accessor);
	RUN_TEST(test_create_null_text_and_style);
	RUN_TEST(test_text_is_truncated_at_cap);
	RUN_TEST(test_set_text_replaces_and_null_clears);
	RUN_TEST(test_draw_calls_draw_text_with_style_and_color);
	RUN_TEST(test_draw_empty_text_is_noop);
	RUN_TEST(test_layout_is_absent);
}
