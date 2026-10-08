/*
 * ui_bridge tests (CTOL rung 1: unit + boundary).
 *
 * Pins the Command -> UiIntent mapping (only nav/select/back map; rotate and
 * zoom are ignored), first-consumer-wins routing through uiHandleIntent, tap
 * routing through uiHandlePointer, the consumed return value, and NULL
 * safety. One integration case drives a real menu element to show nav/select
 * are consumed while cancel passes through.
 *
 * Pure: links only ui_bridge.c (+ input/element deps) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_ui_bridge().
 */

#include "unity.h"

#include "input/input.h"
#include "scenes/ui_bridge.h"
#include "ui/element.h"
#include "ui/element_menu.h"

#include <string.h>

/* A recording element: logs the intents/pointers routed to it and consumes
 * according to two flags. */
typedef struct BridgeRec {
	int intents[8];
	int intentCount;
	int lastX;
	int lastY;
	int pointerCount;
	bool consumeIntent;
	bool consumePointer;
} BridgeRec;

static bool rec_handleIntent(Element *self, UiIntent intent)
{
	BridgeRec *rec = uiElementPayload(self);

	if (rec->intentCount < 8)
		rec->intents[rec->intentCount] = (int)intent;
	rec->intentCount++;
	return rec->consumeIntent;
}

static bool rec_handlePointer(Element *self, int x, int y)
{
	BridgeRec *rec = uiElementPayload(self);

	rec->lastX = x;
	rec->lastY = y;
	rec->pointerCount++;
	return rec->consumePointer;
}

static const ElementVt recVt = {
	NULL, NULL, rec_handleIntent, rec_handlePointer, NULL,
	sizeof(BridgeRec), "rec",
};

static Element *makeRec(BridgeRec **out, bool consumeIntent, bool consumePointer)
{
	Element *e = uiCreateElement(&recVt);
	BridgeRec *rec;

	TEST_ASSERT_NOT_NULL(e);
	rec = uiElementPayload(e);
	memset(rec, 0, sizeof(*rec));
	rec->consumeIntent = consumeIntent;
	rec->consumePointer = consumePointer;
	*out = rec;
	return e;
}

static InputFrame frameWith(const Command *cmds, int n)
{
	InputFrame frame = { 0 };
	int i;

	frame.commandCount = n;
	for (i = 0; i < n; i++)
		frame.commands[i] = cmds[i];
	return frame;
}

static InputFrame frameTap(int x, int y)
{
	InputFrame frame = { 0 };

	frame.tap = true;
	frame.tapX = x;
	frame.tapY = y;
	return frame;
}

/* Every mapped command reaches the UI as its pinned intent and, when
 * consumed, reports consumption. */
static void test_command_intent_mapping(void)
{
	static const struct {
		Command cmd;
		UiIntent intent;
	} cases[] = {
		{ CMD_NAV_UP, UI_NAV_UP },
		{ CMD_NAV_DOWN, UI_NAV_DOWN },
		{ CMD_NAV_LEFT, UI_NAV_LEFT },
		{ CMD_NAV_RIGHT, UI_NAV_RIGHT },
		{ CMD_SELECT, UI_ACTIVATE },
		{ CMD_BACK, UI_CANCEL },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		BridgeRec *rec;
		Element *root = makeRec(&rec, true, false);
		InputFrame frame = frameWith(&cases[i].cmd, 1);

		TEST_ASSERT_TRUE(uiBridgeDispatch(root, &frame));
		TEST_ASSERT_EQUAL_INT(1, rec->intentCount);
		TEST_ASSERT_EQUAL_INT((int)cases[i].intent, rec->intents[0]);
		uiDestroyElement(root);
	}
}

/* Rotate/zoom are not UI commands: ignored, nothing delivered, false. */
static void test_unmapped_commands_ignored(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, true, false);
	Command cmds[3] = { CMD_ROTATE_CW, CMD_ROTATE_CCW, CMD_ZOOM_IN };
	InputFrame frame = frameWith(cmds, 3);

	TEST_ASSERT_FALSE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(0, rec->intentCount);
	uiDestroyElement(root);
}

/* A delivered-but-unconsumed intent yields false. */
static void test_unconsumed_intent_returns_false(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, false, false);
	InputFrame frame = frameWith((Command[]){ CMD_SELECT }, 1);

	TEST_ASSERT_FALSE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(1, rec->intentCount);	/* still delivered */
	uiDestroyElement(root);
}

/* First-consumer-wins: once an intent is consumed, later commands in the
 * same frame are not delivered. */
static void test_first_consumer_wins(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, true, false);
	Command cmds[2] = { CMD_NAV_DOWN, CMD_SELECT };
	InputFrame frame = frameWith(cmds, 2);

	TEST_ASSERT_TRUE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(1, rec->intentCount);
	TEST_ASSERT_EQUAL_INT(UI_NAV_DOWN, rec->intents[0]);
	uiDestroyElement(root);
}

/* With nothing consumed, every command is delivered. */
static void test_all_commands_delivered_when_unconsumed(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, false, false);
	Command cmds[3] = { CMD_NAV_DOWN, CMD_NAV_UP, CMD_SELECT };
	InputFrame frame = frameWith(cmds, 3);

	TEST_ASSERT_FALSE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(3, rec->intentCount);
	uiDestroyElement(root);
}

/* A frame tap is routed with its virtual coordinates. */
static void test_tap_routed(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, false, true);
	InputFrame frame = frameTap(11, 22);

	TEST_ASSERT_TRUE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(1, rec->pointerCount);
	TEST_ASSERT_EQUAL_INT(11, rec->lastX);
	TEST_ASSERT_EQUAL_INT(22, rec->lastY);
	uiDestroyElement(root);
}

/* An unconsumed tap yields false; no tap means no pointer call. */
static void test_tap_unconsumed_and_absent(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, false, false);
	InputFrame tap = frameTap(3, 4);
	InputFrame none = { 0 };

	TEST_ASSERT_FALSE(uiBridgeDispatch(root, &tap));
	TEST_ASSERT_EQUAL_INT(1, rec->pointerCount);
	TEST_ASSERT_FALSE(uiBridgeDispatch(root, &none));
	TEST_ASSERT_EQUAL_INT(1, rec->pointerCount);	/* unchanged */
	uiDestroyElement(root);
}

/* A consumed command no longer swallows a same-frame tap: both the intent and
 * the tap are delivered, and the call reports consumption. */
static void test_tap_routes_after_consumed_command(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, true, true);
	InputFrame frame = frameWith((Command[]){ CMD_SELECT }, 1);

	frame.tap = true;
	frame.tapX = 7;
	frame.tapY = 9;
	TEST_ASSERT_TRUE(uiBridgeDispatch(root, &frame));
	TEST_ASSERT_EQUAL_INT(1, rec->intentCount);
	TEST_ASSERT_EQUAL_INT(UI_ACTIVATE, rec->intents[0]);
	TEST_ASSERT_EQUAL_INT(1, rec->pointerCount);
	TEST_ASSERT_EQUAL_INT(7, rec->lastX);
	TEST_ASSERT_EQUAL_INT(9, rec->lastY);
	uiDestroyElement(root);
}

/* NULL arguments are safe. */
static void test_null_safe(void)
{
	BridgeRec *rec;
	Element *root = makeRec(&rec, true, false);
	InputFrame frame = frameWith((Command[]){ CMD_SELECT }, 1);

	TEST_ASSERT_FALSE(uiBridgeDispatch(root, NULL));
	TEST_ASSERT_FALSE(uiBridgeDispatch(NULL, &frame));
	TEST_ASSERT_EQUAL_INT(0, rec->intentCount);
	uiDestroyElement(root);
}

/* Integration: a real menu consumes nav/select but lets UI_CANCEL through so
 * the owning scene can close it. */
static int g_activated;

static void onActivate(void *ctx)
{
	(void)ctx;
	g_activated++;
}

static void test_menu_integration(void)
{
	Element *menu = uiCreateMenu(NULL);
	InputFrame down = frameWith((Command[]){ CMD_NAV_DOWN }, 1);
	InputFrame select = frameWith((Command[]){ CMD_SELECT }, 1);
	InputFrame back = frameWith((Command[]){ CMD_BACK }, 1);

	TEST_ASSERT_NOT_NULL(menu);
	TEST_ASSERT_NOT_NULL(uiMenuAddItem(menu, "Start", onActivate, NULL));
	TEST_ASSERT_NOT_NULL(uiMenuAddItem(menu, "Quit", onActivate, NULL));
	g_activated = 0;

	TEST_ASSERT_TRUE(uiBridgeDispatch(menu, &down));	/* wraps 0 -> 1 */
	TEST_ASSERT_EQUAL_INT(1, uiMenuSelected(menu));
	TEST_ASSERT_TRUE(uiBridgeDispatch(menu, &select));
	TEST_ASSERT_EQUAL_INT(1, g_activated);
	TEST_ASSERT_FALSE(uiBridgeDispatch(menu, &back));	/* never consumed */

	uiDestroyElement(menu);
}

/* uiBridgeFrameHasBack reports CMD_BACK presence and is NULL-safe. */
static void test_frame_has_back(void)
{
	InputFrame none = frameWith((Command[]){ CMD_NAV_DOWN, CMD_SELECT }, 2);
	InputFrame one = frameWith((Command[]){ CMD_BACK }, 1);
	InputFrame among = frameWith((Command[]){ CMD_NAV_UP, CMD_BACK, CMD_ZOOM_IN }, 3);
	InputFrame tapOnly = frameTap(1, 2);
	InputFrame empty = { 0 };

	TEST_ASSERT_FALSE(uiBridgeFrameHasBack(NULL));
	TEST_ASSERT_FALSE(uiBridgeFrameHasBack(&none));
	TEST_ASSERT_FALSE(uiBridgeFrameHasBack(&tapOnly));
	TEST_ASSERT_FALSE(uiBridgeFrameHasBack(&empty));
	TEST_ASSERT_TRUE(uiBridgeFrameHasBack(&one));
	TEST_ASSERT_TRUE(uiBridgeFrameHasBack(&among));
}

/* Over-capacity frames exercise the loop's INPUT_MAX_COMMANDS guard's false
 * arm in both the dispatch and the presence check. */
static void test_over_capacity_frame_guard(void)
{
	InputFrame over = { 0 };
	int i;

	over.commandCount = INPUT_MAX_COMMANDS + 1;
	for (i = 0; i < INPUT_MAX_COMMANDS; i++)
		over.commands[i] = CMD_ROTATE_CW;

	TEST_ASSERT_FALSE(uiBridgeFrameHasBack(&over));
	TEST_ASSERT_FALSE(uiBridgeDispatch(NULL, &over));
}

void run_test_ui_bridge(void);

void run_test_ui_bridge(void)
{
	RUN_TEST(test_command_intent_mapping);
	RUN_TEST(test_unmapped_commands_ignored);
	RUN_TEST(test_unconsumed_intent_returns_false);
	RUN_TEST(test_first_consumer_wins);
	RUN_TEST(test_all_commands_delivered_when_unconsumed);
	RUN_TEST(test_tap_routed);
	RUN_TEST(test_tap_routes_after_consumed_command);
	RUN_TEST(test_tap_unconsumed_and_absent);
	RUN_TEST(test_null_safe);
	RUN_TEST(test_menu_integration);
	RUN_TEST(test_frame_has_back);
	RUN_TEST(test_over_capacity_frame_guard);
}
