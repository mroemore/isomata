/*
 * Command input core. See input.h for the invariant block: the Command and
 * InputKey vocabularies, the keyboard mapping table, the bounded command
 * queue, the frame lifecycle (inputBeginFrame resets the per-frame
 * accumulators, inputEndFrame freezes them), the physical -> virtual seam
 * through ui_scale.h, and the tap/drag/pinch gesture rules.
 *
 * Storage: one flat struct, no allocation beyond createInput's calloc. The
 * per-frame outputs are reset at begin and copied out at end; the gesture
 * state (pointer down / drag tracking) and the pinch remainder persist
 * across frames so a drag or a pinch may span them.
 */

#include "input/input.h"

#include "ui/ui_scale.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Keyboard mapping: one Command per physical key. The table is indexed by
 * InputKey, so a new key is one row; CMD_COUNT is never a value here. */
static const Command inputKeyMap[INPUT_KEY_COUNT] = {
	[INPUT_KEY_UP] = CMD_NAV_UP,
	[INPUT_KEY_DOWN] = CMD_NAV_DOWN,
	[INPUT_KEY_LEFT] = CMD_NAV_LEFT,
	[INPUT_KEY_RIGHT] = CMD_NAV_RIGHT,
	[INPUT_KEY_H] = CMD_NAV_LEFT,
	[INPUT_KEY_J] = CMD_NAV_DOWN,
	[INPUT_KEY_K] = CMD_NAV_UP,
	[INPUT_KEY_L] = CMD_NAV_RIGHT,
	[INPUT_KEY_ENTER] = CMD_SELECT,
	[INPUT_KEY_SPACE] = CMD_SELECT,
	[INPUT_KEY_ESCAPE] = CMD_BACK,
	[INPUT_KEY_BACKSPACE] = CMD_BACK,
	[INPUT_KEY_Q] = CMD_ROTATE_CCW,
	[INPUT_KEY_E] = CMD_ROTATE_CW,
	[INPUT_KEY_PLUS] = CMD_ZOOM_IN,
	[INPUT_KEY_EQUALS] = CMD_ZOOM_IN,
	[INPUT_KEY_MINUS] = CMD_ZOOM_OUT,
	[INPUT_KEY_PAGE_UP] = CMD_ZOOM_IN,
	[INPUT_KEY_PAGE_DOWN] = CMD_ZOOM_OUT,
	[INPUT_KEY_BACK] = CMD_BACK,
	[INPUT_KEY_R] = CMD_RESET,
	[INPUT_KEY_T] = CMD_TOGGLE_SMOOTH_LIGHT,
	[INPUT_KEY_F] = CMD_TOGGLE_LIGHT_DEBUG,
};

/*
 * Gamepad extension point: a controller would add its own normalized button
 * vocabulary and a parallel table resolved by the same pushCommand path (the
 * SDL glue is where an SDL_Gamepad event would be translated). No gamepad
 * vocabulary or implementation exists yet by design.
 */

struct Input {
	float uiScale;		/* set at inputBeginFrame, used at the seam */

	/* Persistent gesture state (spans frames). */
	bool pointerDown;
	bool dragging;
	bool pinchActive;	/* set by a pinch, cleared at next beginFrame */
	int downVx, downVy;	/* virtual down point */
	int lastVx, lastVy;	/* virtual last move point */
	float zoomRemainder;	/* pinch sub-step remainder, retained */

	/* Per-frame outputs (reset at inputBeginFrame). */
	Command commands[INPUT_MAX_COMMANDS];
	int commandCount;
	bool tap;
	int tapX, tapY;
	int panDx, panDy;
	int zoomSteps;
};

/* Physical -> virtual point through the ui_scale seam (the only conversion
 * path). Float device coords round to int first. */
static int toVirtual(const Input *input, float physical)
{
	return uiScalePhysicalToVirtual((int)lroundf(physical), input->uiScale);
}

/* Bounded append: overflow drops silently (see input.h). */
static void pushCommand(Input *input, Command command)
{
	if (input->commandCount < INPUT_MAX_COMMANDS)
		input->commands[input->commandCount++] = command;
}

Input *createInput(void)
{
	return calloc(1, sizeof(Input));
}

void destroyInput(Input *input)
{
	free(input);
}

void inputBeginFrame(Input *input, float uiScale)
{
	if (input == NULL)
		return;
	input->uiScale = uiScale;
	input->commandCount = 0;
	input->tap = false;
	input->tapX = 0;
	input->tapY = 0;
	input->panDx = 0;
	input->panDy = 0;
	input->zoomSteps = 0;
	input->pinchActive = false;
}

void inputEndFrame(Input *input, InputFrame *out)
{
	if (out == NULL)
		return;
	if (input == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	memcpy(out->commands, input->commands, sizeof(out->commands));
	out->commandCount = input->commandCount;
	out->tap = input->tap;
	out->tapX = input->tapX;
	out->tapY = input->tapY;
	out->panDx = input->panDx;
	out->panDy = input->panDy;
	out->zoomSteps = input->zoomSteps;
}

void inputKeyDown(Input *input, InputKey key)
{
	if (input == NULL)
		return;
	if (key < 0 || key >= INPUT_KEY_COUNT)
		return;
	pushCommand(input, inputKeyMap[key]);
}

void inputPointerDown(Input *input, float x, float y)
{
	if (input == NULL)
		return;
	input->pointerDown = true;
	input->dragging = false;
	input->downVx = toVirtual(input, x);
	input->downVy = toVirtual(input, y);
	input->lastVx = input->downVx;
	input->lastVy = input->downVy;
}

void inputPointerMove(Input *input, float x, float y)
{
	int vx;
	int vy;
	int dx;
	int dy;

	if (input == NULL || !input->pointerDown)
		return;
	vx = toVirtual(input, x);
	vy = toVirtual(input, y);
	if (!input->dragging) {
		/* First sample past the slop starts the drag and carries the
		 * whole delta from the down point. */
		dx = vx - input->downVx;
		dy = vy - input->downVy;
		if (dx * dx + dy * dy > INPUT_TAP_SLOP_PX * INPUT_TAP_SLOP_PX) {
			input->dragging = true;
			input->panDx += dx;
			input->panDy += dy;
		}
	} else {
		input->panDx += vx - input->lastVx;
		input->panDy += vy - input->lastVy;
	}
	input->lastVx = vx;
	input->lastVy = vy;
}

void inputPointerUp(Input *input, float x, float y)
{
	int vx;
	int vy;

	if (input == NULL || !input->pointerDown)
		return;
	vx = toVirtual(input, x);
	vy = toVirtual(input, y);

	/* An up past the slop with no recorded move is still a drag. */
	if (!input->dragging) {
		int dx = vx - input->downVx;
		int dy = vy - input->downVy;
		if (dx * dx + dy * dy > INPUT_TAP_SLOP_PX * INPUT_TAP_SLOP_PX) {
			input->dragging = true;
			input->panDx += dx;
			input->panDy += dy;
		}
	}
	if (!input->dragging && !input->pinchActive) {
		input->tap = true;
		input->tapX = vx < 0 ? 0 : vx;	/* tap coords clamp at 0 */
		input->tapY = vy < 0 ? 0 : vy;
	}
	input->pointerDown = false;
	input->dragging = false;
}

void inputWheel(Input *input, float dy)
{
	if (input == NULL)
		return;
	/* Command-only: commands are the canonical channel (see input.h). */
	if (dy > 0.0f)
		pushCommand(input, CMD_ZOOM_IN);
	else if (dy < 0.0f)
		pushCommand(input, CMD_ZOOM_OUT);
}

void inputPinch(Input *input, float scaleDelta)
{
	if (input == NULL)
		return;
	input->pinchActive = true;
	if (!isfinite(scaleDelta))
		return;		/* a NaN/inf delta can never emit a step */
	input->zoomRemainder += scaleDelta;
	while (input->zoomRemainder >= 1.0f) {
		input->zoomSteps++;
		input->zoomRemainder -= 1.0f;
	}
	while (input->zoomRemainder <= -1.0f) {
		input->zoomSteps--;
		input->zoomRemainder += 1.0f;
	}
}
