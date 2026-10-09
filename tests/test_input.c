/*
 * input tests (CTOL rung 1: unit + boundary).
 *
 * Covers the pure command layer only (input.c): the full keyboard mapping
 * table (arrows + hjkl nav, enter/space select, escape/backspace/back back,
 * q/e rotate, r reset, +/-/page zoom), the bounded command queue, per-frame reset
 * semantics, tap vs drag at the 8px virtual slop (including coordinate
 * conversion at 1x/2x/1.5x and the negative clamp), pan accumulation
 * (only once past slop, full delta from the down point), wheel ->
 * zoom commands, pinch fractional accumulation with a retained remainder,
 * pinch suppressing a same-frame tap, and NULL tolerance.
 *
 * Pure: links only input.c (via isomata_pure) plus ui_scale.c and the Unity
 * subset; no SDL.
 *
 * Harness convention: belongs to the ONE headless executable (test_pure).
 * No main()/setUp()/tearDown(); exposes run_test_input().
 */

#include "unity.h"

#include "input/input.h"

/* --- helpers ---------------------------------------------------------------- */

/* One frame with no events: begin, end, return the outputs. */
static InputFrame emptyFrame(Input *in, float scale)
{
	InputFrame f;

	inputBeginFrame(in, scale);
	inputEndFrame(in, &f);
	return f;
}

/* --- lifecycle -------------------------------------------------------------- */

static void test_create_destroy_null_safe(void)
{
	Input *in = createInput();

	TEST_ASSERT_NOT_NULL(in);
	destroyInput(in);
	destroyInput(NULL);
}

/* Every entry point tolerates a NULL Input and a NULL out. */
static void test_null_input_tolerance(void)
{
	InputFrame f;

	inputBeginFrame(NULL, 1.0f);
	inputKeyDown(NULL, INPUT_KEY_UP);
	inputPointerDown(NULL, 1.0f, 2.0f);
	inputPointerMove(NULL, 3.0f, 4.0f);
	inputPointerUp(NULL, 5.0f, 6.0f);
	inputWheel(NULL, 1.0f);
	inputPinch(NULL, 0.5f);

	f.commandCount = 99;
	inputEndFrame(NULL, &f);
	TEST_ASSERT_EQUAL_INT(0, f.commandCount);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(0, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);

	{
		Input *in = createInput();
		inputEndFrame(in, NULL);	/* NULL out: no crash */
		destroyInput(in);
	}
}

/* --- keyboard mapping ------------------------------------------------------- */

static void test_keyboard_mapping_full_table(void)
{
	static const struct {
		InputKey key;
		Command cmd;
	} table[] = {
		{ INPUT_KEY_UP, CMD_NAV_UP },
		{ INPUT_KEY_DOWN, CMD_NAV_DOWN },
		{ INPUT_KEY_LEFT, CMD_NAV_LEFT },
		{ INPUT_KEY_RIGHT, CMD_NAV_RIGHT },
		{ INPUT_KEY_H, CMD_NAV_LEFT },
		{ INPUT_KEY_J, CMD_NAV_DOWN },
		{ INPUT_KEY_K, CMD_NAV_UP },
		{ INPUT_KEY_L, CMD_NAV_RIGHT },
		{ INPUT_KEY_ENTER, CMD_SELECT },
		{ INPUT_KEY_SPACE, CMD_SELECT },
		{ INPUT_KEY_ESCAPE, CMD_BACK },
		{ INPUT_KEY_BACKSPACE, CMD_BACK },
		{ INPUT_KEY_BACK, CMD_BACK },
		{ INPUT_KEY_Q, CMD_ROTATE_CCW },
		{ INPUT_KEY_E, CMD_ROTATE_CW },
		{ INPUT_KEY_R, CMD_RESET },
		{ INPUT_KEY_PLUS, CMD_ZOOM_IN },
		{ INPUT_KEY_EQUALS, CMD_ZOOM_IN },
		{ INPUT_KEY_PAGE_UP, CMD_ZOOM_IN },
		{ INPUT_KEY_MINUS, CMD_ZOOM_OUT },
		{ INPUT_KEY_PAGE_DOWN, CMD_ZOOM_OUT },
		{ INPUT_KEY_T, CMD_TOGGLE_SMOOTH_LIGHT },
		{ INPUT_KEY_F, CMD_TOGGLE_LIGHT_DEBUG },
	};
	size_t i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		Input *in = createInput();
		InputFrame f;

		inputBeginFrame(in, 1.0f);
		inputKeyDown(in, table[i].key);
		inputEndFrame(in, &f);
		TEST_ASSERT_EQUAL_INT(1, f.commandCount);
		TEST_ASSERT_EQUAL_INT(table[i].cmd, f.commands[0]);
		destroyInput(in);
	}
}

/* Keys outside the vocabulary are ignored (defensive bounds arm). */
static void test_key_out_of_range_ignored(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputKeyDown(in, (InputKey)-1);
	inputKeyDown(in, INPUT_KEY_COUNT);
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(0, f.commandCount);
	destroyInput(in);
}

/* CMD_RESET is produced by exactly one key (R); every other key maps to
 * something else. */
static void test_only_r_maps_to_reset(void)
{
	int key;
	int resetKeys = 0;
	InputKey resetKey = INPUT_KEY_COUNT;

	for (key = 0; key < INPUT_KEY_COUNT; key++) {
		Input *in = createInput();
		InputFrame f;

		inputBeginFrame(in, 1.0f);
		inputKeyDown(in, (InputKey)key);
		inputEndFrame(in, &f);
		if (f.commandCount == 1 && f.commands[0] == CMD_RESET) {
			resetKeys++;
			resetKey = (InputKey)key;
		}
		destroyInput(in);
	}
	TEST_ASSERT_EQUAL_INT(1, resetKeys);
	TEST_ASSERT_EQUAL_INT(INPUT_KEY_R, resetKey);
}

/* The command queue is bounded at INPUT_MAX_COMMANDS; overflow drops. */
static void test_command_queue_bound(void)
{
	Input *in = createInput();
	InputFrame f;
	int i;

	inputBeginFrame(in, 1.0f);
	for (i = 0; i < INPUT_MAX_COMMANDS + 5; i++) {
		inputKeyDown(in, INPUT_KEY_UP);
	}
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(INPUT_MAX_COMMANDS, f.commandCount);
	for (i = 0; i < INPUT_MAX_COMMANDS; i++) {
		TEST_ASSERT_EQUAL_INT(CMD_NAV_UP, f.commands[i]);
	}
	destroyInput(in);
}

/* A frame starts empty: commands/tap/pan/zoom do not leak across frames. */
static void test_frame_reset_semantics(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputKeyDown(in, INPUT_KEY_ENTER);
	inputPointerDown(in, 10.0f, 10.0f);
	inputPointerUp(in, 10.0f, 10.0f);
	inputWheel(in, 1.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(2, f.commandCount);
	TEST_ASSERT_TRUE(f.tap);

	f = emptyFrame(in, 1.0f);
	TEST_ASSERT_EQUAL_INT(0, f.commandCount);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(0, f.tapX);
	TEST_ASSERT_EQUAL_INT(0, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.panDy);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);

	destroyInput(in);
}

/* --- tap vs drag ------------------------------------------------------------ */

/* Move/up with no active press are ignored (no tap, no pan). */
static void test_pointer_without_down_ignored(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerMove(in, 50.0f, 50.0f);
	inputPointerUp(in, 50.0f, 50.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(0, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.panDy);
	destroyInput(in);
}

/* Jitter under the slop still counts as a tap, at the UP position. */
static void test_tap_below_slop_at_up_position(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 100.0f, 50.0f);
	inputPointerMove(in, 103.0f, 52.0f);
	inputPointerUp(in, 104.0f, 54.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_TRUE(f.tap);
	TEST_ASSERT_EQUAL_INT(104, f.tapX);
	TEST_ASSERT_EQUAL_INT(54, f.tapY);
	TEST_ASSERT_EQUAL_INT(0, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.panDy);
	destroyInput(in);
}

/* Tap coordinates cross the virtual seam via ui_scale at 2x. */
static void test_tap_converted_2x(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 2.0f);
	inputPointerDown(in, 100.0f, 50.0f);
	inputPointerUp(in, 100.0f, 50.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_TRUE(f.tap);
	TEST_ASSERT_EQUAL_INT(50, f.tapX);
	TEST_ASSERT_EQUAL_INT(25, f.tapY);
	destroyInput(in);
}

/* 1.5x rounds half away from zero (66.67 -> 67, 33.33 -> 33). */
static void test_tap_converted_1_5x(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.5f);
	inputPointerDown(in, 100.0f, 50.0f);
	inputPointerUp(in, 100.0f, 50.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_TRUE(f.tap);
	TEST_ASSERT_EQUAL_INT(67, f.tapX);
	TEST_ASSERT_EQUAL_INT(33, f.tapY);
	destroyInput(in);
}

/* Negative virtual tap coordinates clamp to 0. */
static void test_tap_coords_clamp_negative(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, -10.0f, -20.0f);
	inputPointerUp(in, -10.0f, -20.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_TRUE(f.tap);
	TEST_ASSERT_EQUAL_INT(0, f.tapX);
	TEST_ASSERT_EQUAL_INT(0, f.tapY);
	destroyInput(in);
}

/* Past the slop it is a drag: pan accumulates, no tap. */
static void test_drag_beyond_slop_no_tap(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 100.0f, 100.0f);
	inputPointerMove(in, 120.0f, 100.0f);
	inputPointerUp(in, 120.0f, 100.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(20, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.panDy);
	destroyInput(in);
}

/* Pan starts only once past slop, and then carries the full delta from the
 * down point; later moves add their own deltas. */
static void test_pan_accumulates_after_slop(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 100.0f, 100.0f);
	inputPointerMove(in, 105.0f, 100.0f);	/* under slop: no pan yet */
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(0, f.panDx);

	inputBeginFrame(in, 1.0f);
	inputPointerMove(in, 115.0f, 100.0f);	/* 15px from down: drag starts */
	inputPointerMove(in, 125.0f, 110.0f);	/* +10,+10 */
	inputPointerUp(in, 125.0f, 110.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(25, f.panDx);	/* 15 (down->115) + 10 */
	TEST_ASSERT_EQUAL_INT(10, f.panDy);
	destroyInput(in);
}

/* A down/up with no move that ends past slop is still a drag. */
static void test_drag_up_without_move(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 0.0f, 0.0f);
	inputPointerUp(in, 50.0f, 0.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(50, f.panDx);
	TEST_ASSERT_EQUAL_INT(0, f.panDy);
	destroyInput(in);
}

/* --- wheel ------------------------------------------------------------------ */

/* Wheel is command-only (the canonical channel); zoomSteps stays pinch-only. */
static void test_wheel_emits_zoom_commands(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputWheel(in, 1.0f);
	inputWheel(in, -1.0f);
	inputWheel(in, 0.0f);	/* neutral: no command */
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(2, f.commandCount);
	TEST_ASSERT_EQUAL_INT(CMD_ZOOM_IN, f.commands[0]);
	TEST_ASSERT_EQUAL_INT(CMD_ZOOM_OUT, f.commands[1]);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);
	destroyInput(in);
}

/* --- pinch ------------------------------------------------------------------ */

/* One step per full unit of scale delta; the remainder is retained across
 * frames. */
static void test_pinch_accumulation_retains_remainder(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPinch(in, 0.6f);
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);	/* 0.6: below one step */

	f = emptyFrame(in, 1.0f);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);	/* remainder carries, no new input */

	inputBeginFrame(in, 1.0f);
	inputPinch(in, 0.6f);			/* 1.2 -> one step, 0.2 kept */
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(1, f.zoomSteps);

	inputBeginFrame(in, 1.0f);
	inputPinch(in, 0.8f);			/* 1.0 -> one step, 0.0 kept */
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(1, f.zoomSteps);

	f = emptyFrame(in, 1.0f);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);	/* remainder exhausted */

	destroyInput(in);
}

/* Negative deltas (zoom out) accumulate the same way. */
static void test_pinch_negative_accumulation(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPinch(in, -0.5f);
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);

	inputBeginFrame(in, 1.0f);
	inputPinch(in, -0.5f);
	inputEndFrame(in, &f);
	TEST_ASSERT_EQUAL_INT(-1, f.zoomSteps);
	destroyInput(in);
}

/* A pinch during the frame suppresses a same-frame tap. */
static void test_pinch_suppresses_same_frame_tap(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 10.0f, 10.0f);
	inputPinch(in, 0.3f);
	inputPointerUp(in, 10.0f, 10.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_FALSE(f.tap);
	TEST_ASSERT_EQUAL_INT(0, f.zoomSteps);
	destroyInput(in);
}

/* The pinch latch is frame-scoped: the next frame taps normally. */
static void test_pinch_latch_clears_next_frame(void)
{
	Input *in = createInput();
	InputFrame f;

	inputBeginFrame(in, 1.0f);
	inputPinch(in, 0.3f);
	inputEndFrame(in, &f);

	inputBeginFrame(in, 1.0f);
	inputPointerDown(in, 10.0f, 10.0f);
	inputPointerUp(in, 10.0f, 10.0f);
	inputEndFrame(in, &f);
	TEST_ASSERT_TRUE(f.tap);
	destroyInput(in);
}

void run_test_input(void);

void run_test_input(void)
{
	RUN_TEST(test_create_destroy_null_safe);
	RUN_TEST(test_null_input_tolerance);
	RUN_TEST(test_keyboard_mapping_full_table);
	RUN_TEST(test_key_out_of_range_ignored);
	RUN_TEST(test_only_r_maps_to_reset);
	RUN_TEST(test_command_queue_bound);
	RUN_TEST(test_frame_reset_semantics);
	RUN_TEST(test_pointer_without_down_ignored);
	RUN_TEST(test_tap_below_slop_at_up_position);
	RUN_TEST(test_tap_converted_2x);
	RUN_TEST(test_tap_converted_1_5x);
	RUN_TEST(test_tap_coords_clamp_negative);
	RUN_TEST(test_drag_beyond_slop_no_tap);
	RUN_TEST(test_pan_accumulates_after_slop);
	RUN_TEST(test_drag_up_without_move);
	RUN_TEST(test_wheel_emits_zoom_commands);
	RUN_TEST(test_pinch_accumulation_retains_remainder);
	RUN_TEST(test_pinch_negative_accumulation);
	RUN_TEST(test_pinch_suppresses_same_frame_tap);
	RUN_TEST(test_pinch_latch_clears_next_frame);
}
