/*
 * SDL3 glue for the pure command layer (input.h/input.c).
 *
 * This is the ONLY input file that includes SDL, and it is compiled into the
 * app target only (meson engine_sources) — never into isomata_pure, so the
 * headless suite stays device-free. It translates SDL_Event into the core
 * calls; all gesture and mapping logic lives in input.c.
 *
 * Device policy:
 * - Keyboard: SDL_KEY_DOWN (repeats ignored) -> the normalized InputKey via
 *   inputKeyFromSdlKeycode. Android's back key (SDLK_AC_BACK) maps to
 *   INPUT_KEY_BACK -> CMD_BACK.
 * - Mouse: a left button press/release/motion is a pointer gesture; wheel
 *   y is forwarded to inputWheel. Events synthesized from touch
 *   (which == SDL_TOUCH_MOUSEID) are ignored so a touch is not counted
 *   twice.
 * - Touch: finger down/motion/up (and canceled, treated as up) are
 *   normalized 0..1 in the event; the glue converts to physical pixels with
 *   the window's pixel size and feeds the pointer entry points.
 * - Pinch: SDL_EVENT_PINCH_UPDATE carries a per-update `scale` (change since
 *   the last update; > 1 zooms in, < 1 zooms out); the glue forwards
 *   scale - 1 as the core scaleDelta. BEGIN/END carry no delta.
 *
 * The function is declared here (no SDL-free header exposes it); the app
 * loop (Task 9) calls it between inputBeginFrame and inputEndFrame.
 */

#include "input/input.h"

#include <SDL3/SDL.h>

/* SDL_Keycode -> normalized physical key. Returns false when unmapped. */
static bool inputKeyFromSdlKeycode(SDL_Keycode key, InputKey *out)
{
	switch (key) {
	case SDLK_UP:
		*out = INPUT_KEY_UP;
		return true;
	case SDLK_DOWN:
		*out = INPUT_KEY_DOWN;
		return true;
	case SDLK_LEFT:
		*out = INPUT_KEY_LEFT;
		return true;
	case SDLK_RIGHT:
		*out = INPUT_KEY_RIGHT;
		return true;
	case SDLK_H:
		*out = INPUT_KEY_H;
		return true;
	case SDLK_J:
		*out = INPUT_KEY_J;
		return true;
	case SDLK_K:
		*out = INPUT_KEY_K;
		return true;
	case SDLK_L:
		*out = INPUT_KEY_L;
		return true;
	case SDLK_RETURN:
		*out = INPUT_KEY_ENTER;
		return true;
	case SDLK_SPACE:
		*out = INPUT_KEY_SPACE;
		return true;
	case SDLK_ESCAPE:
		*out = INPUT_KEY_ESCAPE;
		return true;
	case SDLK_BACKSPACE:
		*out = INPUT_KEY_BACKSPACE;
		return true;
	case SDLK_Q:
		*out = INPUT_KEY_Q;
		return true;
	case SDLK_E:
		*out = INPUT_KEY_E;
		return true;
	case SDLK_PLUS:
		*out = INPUT_KEY_PLUS;
		return true;
	case SDLK_EQUALS:
		*out = INPUT_KEY_EQUALS;
		return true;
	case SDLK_MINUS:
		*out = INPUT_KEY_MINUS;
		return true;
	case SDLK_PAGEUP:
		*out = INPUT_KEY_PAGE_UP;
		return true;
	case SDLK_PAGEDOWN:
		*out = INPUT_KEY_PAGE_DOWN;
		return true;
	case SDLK_AC_BACK:
		*out = INPUT_KEY_BACK;
		return true;
	default:
		return false;
	}
}

/* Finger events are normalized 0..1; scale them into window pixel space so
 * the core receives physical coordinates like the mouse path. */
static void inputHandleFinger(Input *input, const SDL_Event *event)
{
	SDL_Window *window;
	int w = 0;
	int h = 0;
	float x;
	float y;

	window = SDL_GetWindowFromID(event->tfinger.windowID);
	if (window == NULL || !SDL_GetWindowSizeInPixels(window, &w, &h))
		return;
	x = event->tfinger.x * (float)w;
	y = event->tfinger.y * (float)h;

	switch (event->type) {
	case SDL_EVENT_FINGER_DOWN:
		inputPointerDown(input, x, y);
		break;
	case SDL_EVENT_FINGER_MOTION:
		inputPointerMove(input, x, y);
		break;
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		inputPointerUp(input, x, y);
		break;
	default:
		break;
	}
}

void inputHandleSdlEvent(Input *input, const SDL_Event *event)
{
	if (input == NULL || event == NULL)
		return;

	switch (event->type) {
	case SDL_EVENT_KEY_DOWN: {
		InputKey key;

		if (event->key.repeat)
			break;	/* a held key is one command, not a stream */
		if (inputKeyFromSdlKeycode(event->key.key, &key))
			inputKeyDown(input, key);
		break;
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event->button.button == SDL_BUTTON_LEFT &&
		    event->button.which != SDL_TOUCH_MOUSEID)
			inputPointerDown(input, event->button.x, event->button.y);
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event->button.button == SDL_BUTTON_LEFT &&
		    event->button.which != SDL_TOUCH_MOUSEID)
			inputPointerUp(input, event->button.x, event->button.y);
		break;
	case SDL_EVENT_MOUSE_MOTION:
		if ((event->motion.state & SDL_BUTTON_LMASK) &&
		    event->motion.which != SDL_TOUCH_MOUSEID)
			inputPointerMove(input, event->motion.x, event->motion.y);
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		inputWheel(input, event->wheel.y);
		break;
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		inputHandleFinger(input, event);
		break;
	case SDL_EVENT_PINCH_UPDATE:
		inputPinch(input, event->pinch.scale - 1.0f);
		break;
	default:
		break;
	}
}
