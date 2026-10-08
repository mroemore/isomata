#ifndef ISOMATA_INPUT_H
#define ISOMATA_INPUT_H

/*
 * Cross-device command input: the headless half of the input layer.
 *
 * This module is PURE: no SDL header is included here or in input.c, and it
 * never creates a device. It owns the device-independent vocabulary
 * (Command, InputKey), the per-device mapping tables, the tap/drag/pinch
 * gesture state machine, and the physical -> virtual coordinate conversion.
 * The SDL glue lives in the app-only twin input_sdl.c, which translates
 * SDL_Event into the core calls below; it is the ONLY input file that
 * includes SDL.
 *
 * Commands, not devices, are the integration currency. Task 9 maps Command
 * to the UI's UiIntent at the scene boundary, so ui/ never depends on
 * input/ and input/ never depends on ui/ (only on ui_scale.h for the
 * conversion seam).
 *
 * Invariant block:
 *
 * - Command order is the plan's verbatim order; CMD_COUNT is the X_COUNT
 *   sentinel and is never a real command.
 * - InputKey is a normalized PHYSICAL-key vocabulary (INPUT_KEY_H is the
 *   H key, not "left"), so one mapping table per device stays independent
 *   of the gesture logic. INPUT_KEY_COUNT is the sentinel.
 * - Keyboard map (each InputKey -> exactly one Command):
 *     UP/DOWN/LEFT/RIGHT and K/J/H/L -> CMD_NAV_UP/DOWN/LEFT/RIGHT
 *       (hjkl: h=left, j=down, k=up, l=right)
 *     ENTER, SPACE                    -> CMD_SELECT
 *     ESCAPE, BACKSPACE, BACK         -> CMD_BACK   (BACK is Android's
 *                                        SDLK_AC_BACK; the glue feeds it)
 *     Q, E                            -> CMD_ROTATE_CCW, CMD_ROTATE_CW
 *     PLUS, EQUALS, PAGE_UP           -> CMD_ZOOM_IN
 *     MINUS, PAGE_DOWN                -> CMD_ZOOM_OUT
 *   A key outside [0, INPUT_KEY_COUNT) is ignored.
 * - Mouse: a left click is a pointer gesture (tap or drag, below); the
 *   wheel is COMMAND-ONLY: dy > 0 -> CMD_ZOOM_IN, dy < 0 -> CMD_ZOOM_OUT,
 *   dy == 0 -> nothing. zoomSteps carries PINCH magnitude only; wheel
 *   does NOT touch it (commands are the canonical channel — this is the
 *   one path chosen). A gamepad mapping table is a documented extension
 *   point (see input.c); no gamepad implementation exists yet.
 *
 * Frame lifecycle and ordering (tests pin it):
 *
 *   inputBeginFrame -> feed events -> inputEndFrame -> read out
 *
 * - inputBeginFrame(input, uiScale) records the frame's scale and resets
 *   the per-frame accumulators (command queue, tap, pan, zoomSteps, and the
 *   pinch latch). It does NOT reset the persistent gesture state (pointer
 *   down / drag tracking) or the retained pinch remainder, so a drag or a
 *   pinch may span frames. Calling it again without an intervening
 *   inputEndFrame simply discards the previous frame's outputs.
 * - inputEndFrame(input, out) freezes the current accumulators into out.
 *   It is idempotent (two calls report the same frame) and does not reset
 *   anything. A NULL out is a no-op; a NULL input with a non-NULL out
 *   zeroes out. Everything else is a no-op on NULL input.
 *
 * Coordinate seam:
 *
 * - Pointer entry points take PHYSICAL device coordinates. The outputs
 *   (tapX/tapY, panDx/panDy) are VIRTUAL (logical UI) coordinates,
 *   converted exclusively through ui_scale.h using the scale passed to
 *   inputBeginFrame. Float device coords are rounded to int with lroundf
 *   before uiScalePhysicalToVirtual; negative virtual tap coordinates are
 *   clamped to 0 (the only clamp — pan deltas may be negative). No other
 *   conversion path exists in this module.
 *
 * Gestures:
 *
 * - Tap vs drag: a pointer-down records the origin; moves accumulate pan
 *   once the origin is more than INPUT_TAP_SLOP_PX virtual px away
 *   (Euclidean, compared squared). From that first past-slop sample the
 *   pan carries the FULL delta from the down point, and later samples add
 *   their own deltas. A pointer-up within the slop of the down point with
 *   no pinch in the frame is a tap at the UP position (converted); an up
 *   past the slop is a drag (its delta added to the pan) and never a tap.
 *   A pinch during the frame suppresses a same-frame tap (two-finger
 *   gestures are not taps); the latch clears at the next inputBeginFrame.
 * - Command queue: bounded at INPUT_MAX_COMMANDS; overflow drops silently
 *   (no status channel — the frame carries the first INPUT_MAX_COMMANDS).
 * - Pinch: inputPinch accumulates scaleDelta into a retained remainder and
 *   emits one zoomSteps step per full unit (scaleDelta >= 1.0), keeping the
 *   fractional remainder for later frames (and symmetrically for negative
 *   deltas). zoomSteps is reset per frame but the remainder is not.
 */

#include <stdbool.h>

typedef enum {
	CMD_NAV_UP,
	CMD_NAV_DOWN,
	CMD_NAV_LEFT,
	CMD_NAV_RIGHT,
	CMD_SELECT,
	CMD_BACK,
	CMD_ROTATE_CW,
	CMD_ROTATE_CCW,
	CMD_ZOOM_IN,
	CMD_ZOOM_OUT,
	CMD_COUNT
} Command;

typedef enum {
	INPUT_KEY_UP = 0,
	INPUT_KEY_DOWN,
	INPUT_KEY_LEFT,
	INPUT_KEY_RIGHT,
	INPUT_KEY_H,
	INPUT_KEY_J,
	INPUT_KEY_K,
	INPUT_KEY_L,
	INPUT_KEY_ENTER,
	INPUT_KEY_SPACE,
	INPUT_KEY_ESCAPE,
	INPUT_KEY_BACKSPACE,
	INPUT_KEY_Q,
	INPUT_KEY_E,
	INPUT_KEY_PLUS,
	INPUT_KEY_EQUALS,
	INPUT_KEY_MINUS,
	INPUT_KEY_PAGE_UP,
	INPUT_KEY_PAGE_DOWN,
	INPUT_KEY_BACK,		/* Android SDLK_AC_BACK */
	INPUT_KEY_COUNT
} InputKey;

#define INPUT_MAX_COMMANDS 16
#define INPUT_TAP_SLOP_PX 8

typedef struct InputFrame {
	Command commands[INPUT_MAX_COMMANDS];
	int commandCount;
	bool tap;		/* one tap this frame, at tapX/tapY (virtual) */
	int tapX;
	int tapY;
	int panDx;		/* drag delta accumulated this frame (virtual px) */
	int panDy;
	int zoomSteps;		/* pinch zoom accumulated this frame */
} InputFrame;

typedef struct Input Input;

/* Lifecycle. createInput zeroes the machine; destroyInput frees it and
 * tolerates NULL. */
Input *createInput(void);
void destroyInput(Input *input);

/* Frame boundary (see the ordering rules above). */
void inputBeginFrame(Input *input, float uiScale);
void inputEndFrame(Input *input, InputFrame *out);

/* Device events, fed between begin and end. Physical pointer coordinates. */
void inputKeyDown(Input *input, InputKey key);
void inputPointerDown(Input *input, float x, float y);
void inputPointerMove(Input *input, float x, float y);
void inputPointerUp(Input *input, float x, float y);
void inputWheel(Input *input, float dy);
void inputPinch(Input *input, float scaleDelta);

#endif /* ISOMATA_INPUT_H */
