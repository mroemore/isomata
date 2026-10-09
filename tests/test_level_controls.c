/*
 * Level controls tests (CTOL rung 1: unit + boundary).
 *
 * Covers: icon assignment at creation, the three verified layout configs
 * (1280 desktop, 316 phone, 246 default virtual px — no overlap, inside
 * the safe area, fallback arm taken exactly where expected), the desktop
 * rects byte-pinned, the debug icon swap driven by levelControlsToggleDebug
 * (the shared F-key / button path), the getter, and NULL safety.
 *
 * Pure: links level_controls.c + element_button.c + element.c plus Unity.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_level_controls().
 */

#include "unity.h"

#include "scenes/level_controls.h"

#include "ui/layout.h"
#include "ui_test_util.h"

typedef struct Rect {
	int x;
	int y;
	int w;
	int h;
} Rect;

static Rect rectOf(Element *e)
{
	Rect r;

	uiGetRect(e, &r.x, &r.y, &r.w, &r.h);
	return r;
}

static bool overlaps(Rect a, Rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
	       b.y < a.y + a.h;
}

static void assertInside(Rect r, int sx, int sy, int sw, int sh)
{
	TEST_ASSERT_TRUE(r.x >= sx);
	TEST_ASSERT_TRUE(r.y >= sy);
	TEST_ASSERT_TRUE(r.x + r.w <= sx + sw);
	TEST_ASSERT_TRUE(r.y + r.h <= sy + sh);
}

static void assertNoOverlap(LevelControls *c)
{
	Element *e[4] = { c->rotL, c->rotR, c->debug, c->reset };
	int i;
	int j;

	for (i = 0; i < 4; i++)
		for (j = i + 1; j < 4; j++)
			TEST_ASSERT_FALSE(overlaps(rectOf(e[i]), rectOf(e[j])));
}

/* Create a root pane with the four controls appended. Caller owns root. */
static Element *makeControls(LevelControls *c)
{
	Element *root = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);

	TEST_ASSERT_NOT_NULL(root);
	TEST_ASSERT_TRUE(levelControlsCreate(c, root, NULL, NULL, NULL, NULL,
					     NULL, NULL));
	return root;
}

static void test_create_assigns_icons_in_order(void)
{
	LevelControls c;
	Element *root = makeControls(&c);

	TEST_ASSERT_EQUAL_INT(UI_ICON_ROTATE_CCW, (int)uiButtonIcon(c.rotL));
	TEST_ASSERT_EQUAL_INT(UI_ICON_ROTATE_CW, (int)uiButtonIcon(c.rotR));
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB_OFF, (int)uiButtonIcon(c.debug));
	TEST_ASSERT_EQUAL_INT(UI_ICON_RESTORE, (int)uiButtonIcon(c.reset));

	/* Appended in draw order: ROT L, ROT R, DEBUG, RESET. */
	TEST_ASSERT_EQUAL_PTR(c.rotL, uiFirstChild(root));
	TEST_ASSERT_EQUAL_PTR(c.rotR, uiNextSibling(c.rotL));
	TEST_ASSERT_EQUAL_PTR(c.debug, uiNextSibling(c.rotR));
	TEST_ASSERT_EQUAL_PTR(c.reset, uiNextSibling(c.debug));
	TEST_ASSERT_NULL(uiNextSibling(c.reset));

	uiDestroyElement(root);
}

/* Desktop (uiScale 1, 1280x720): one row; RESET byte-identical to the
 * pre-debug layout, DEBUG one gap to its left. */
static void test_layout_desktop_1280(void)
{
	LevelControls c;
	Element *root = makeControls(&c);
	Rect r;

	levelControlsLayout(&c, 0, 0, 1280, 720);

	r = rectOf(c.rotL);
	TEST_ASSERT_EQUAL_INT(16, r.x);
	TEST_ASSERT_EQUAL_INT(720 - 16 - 48, r.y);
	TEST_ASSERT_EQUAL_INT(96, r.w);
	TEST_ASSERT_EQUAL_INT(48, r.h);

	r = rectOf(c.rotR);
	TEST_ASSERT_EQUAL_INT(16 + 96 + 8, r.x);
	TEST_ASSERT_EQUAL_INT(720 - 16 - 48, r.y);

	r = rectOf(c.reset);
	TEST_ASSERT_EQUAL_INT(1280 - 16 - 96, r.x);
	TEST_ASSERT_EQUAL_INT(720 - 16 - 48, r.y);

	r = rectOf(c.debug);
	TEST_ASSERT_EQUAL_INT(1280 - 16 - 96 - 8 - 96, r.x);
	TEST_ASSERT_EQUAL_INT(720 - 16 - 48, r.y);

	assertNoOverlap(&c);
	assertInside(rectOf(c.rotL), 0, 0, 1280, 720);
	assertInside(rectOf(c.rotR), 0, 0, 1280, 720);
	assertInside(rectOf(c.debug), 0, 0, 1280, 720);
	assertInside(rectOf(c.reset), 0, 0, 1280, 720);

	uiDestroyElement(root);
}

/* Phone (1080/3.41 ~= 316 virtual px): the right pair rides a second row
 * above the bottom-left pair, right-aligned, still inside the safe area. */
static void test_layout_phone_316(void)
{
	LevelControls c;
	Element *root = makeControls(&c);
	Rect rotL;
	Rect rotR;
	Rect debug;
	Rect reset;

	levelControlsLayout(&c, 0, 0, 316, 700);

	rotL = rectOf(c.rotL);
	rotR = rectOf(c.rotR);
	debug = rectOf(c.debug);
	reset = rectOf(c.reset);

	/* Bottom-left pair unchanged. */
	TEST_ASSERT_EQUAL_INT(16, rotL.x);
	TEST_ASSERT_EQUAL_INT(120, rotR.x);
	TEST_ASSERT_EQUAL_INT(700 - 16 - 48, rotL.y);

	/* Right pair lifted one row + gap; RESET right-aligned. */
	TEST_ASSERT_EQUAL_INT(316 - 16 - 96, reset.x);
	TEST_ASSERT_EQUAL_INT(reset.x - 8 - 96, debug.x);
	TEST_ASSERT_EQUAL_INT(rotL.y - 48 - 8, debug.y);
	TEST_ASSERT_EQUAL_INT(debug.y, reset.y);

	assertNoOverlap(&c);
	assertInside(rotL, 0, 0, 316, 700);
	assertInside(rotR, 0, 0, 316, 700);
	assertInside(debug, 0, 0, 316, 700);
	assertInside(reset, 0, 0, 316, 700);

	uiDestroyElement(root);
}

/* Default virtual size 246: still the fallback, still inside/ non-overlap. */
static void test_layout_default_246(void)
{
	LevelControls c;
	Element *root = makeControls(&c);
	Rect rotR;
	Rect debug;
	Rect reset;

	levelControlsLayout(&c, 0, 0, 246, 700);

	rotR = rectOf(c.rotR);
	debug = rectOf(c.debug);
	reset = rectOf(c.reset);

	TEST_ASSERT_EQUAL_INT(246 - 16 - 96, reset.x);
	TEST_ASSERT_EQUAL_INT(reset.x - 8 - 96, debug.x);
	TEST_ASSERT_EQUAL_INT(rotR.y - 48 - 8, debug.y);	/* second row */

	assertNoOverlap(&c);
	assertInside(rectOf(c.rotL), 0, 0, 246, 700);
	assertInside(rotR, 0, 0, 246, 700);
	assertInside(debug, 0, 0, 246, 700);
	assertInside(reset, 0, 0, 246, 700);

	/* Non-zero safe-area origin is honoured too. */
	levelControlsLayout(&c, 30, 40, 246, 700);
	assertNoOverlap(&c);
	assertInside(rectOf(c.rotL), 30, 40, 246, 700);
	assertInside(rectOf(c.debug), 30, 40, 246, 700);
	assertInside(rectOf(c.reset), 30, 40, 246, 700);

	uiDestroyElement(root);
}

/* The shared toggle path (F key handler + DEBUG button): flips the flag and
 * swaps the icon both ways. */
static void test_toggle_debug_flips_flag_and_icon(void)
{
	LevelControls c;
	Element *root = makeControls(&c);
	bool flag = false;

	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB_OFF, (int)uiButtonIcon(c.debug));

	levelControlsToggleDebug(&c, &flag);
	TEST_ASSERT_TRUE(flag);
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB, (int)uiButtonIcon(c.debug));

	levelControlsToggleDebug(&c, &flag);
	TEST_ASSERT_FALSE(flag);
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB_OFF, (int)uiButtonIcon(c.debug));

	/* Explicit set (init / resync) matches the flag directly. */
	levelControlsSetDebugIcon(&c, true);
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB, (int)uiButtonIcon(c.debug));
	levelControlsSetDebugIcon(&c, false);
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB_OFF, (int)uiButtonIcon(c.debug));

	uiDestroyElement(root);
}

/* Drawing a control emits its icon (and no text) through the seam. */
static void test_draw_emits_icon(void)
{
	static TestDrawLog log;
	LevelControls c;
	Element *root = makeControls(&c);
	UiDrawCtx ctx;

	levelControlsLayout(&c, 0, 0, 1280, 720);

	tdlReset(&log);
	ctx = tdlCtx(&log);
	uiDraw(c.reset, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nImages);
	TEST_ASSERT_EQUAL_INT(UI_ICON_RESTORE, log.images[0].icon);
	TEST_ASSERT_EQUAL_INT(0, log.nTexts);

	tdlReset(&log);
	ctx = tdlCtx(&log);
	uiDraw(c.debug, &ctx);
	TEST_ASSERT_EQUAL_INT(1, log.nImages);
	TEST_ASSERT_EQUAL_INT(UI_ICON_BULB_OFF, log.images[0].icon);

	uiDestroyElement(root);
}

/* NULL/bad arguments never crash. */
static void test_null_safety(void)
{
	LevelControls c = { 0 };
	Element *root = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);

	TEST_ASSERT_FALSE(levelControlsCreate(NULL, root, NULL, NULL, NULL,
					      NULL, NULL, NULL));
	TEST_ASSERT_FALSE(levelControlsCreate(&c, NULL, NULL, NULL, NULL,
					      NULL, NULL, NULL));

	levelControlsLayout(NULL, 0, 0, 100, 100);
	levelControlsSetDebugIcon(NULL, true);
	levelControlsToggleDebug(NULL, NULL);
	levelControlsToggleDebug(&c, NULL);	/* NULL flag: no-op */

	uiDestroyElement(root);
}

void run_test_level_controls(void);

void run_test_level_controls(void)
{
	RUN_TEST(test_create_assigns_icons_in_order);
	RUN_TEST(test_layout_desktop_1280);
	RUN_TEST(test_layout_phone_316);
	RUN_TEST(test_layout_default_246);
	RUN_TEST(test_toggle_debug_flips_flag_and_icon);
	RUN_TEST(test_draw_emits_icon);
	RUN_TEST(test_null_safety);
}
