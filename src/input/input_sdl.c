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
 *   twice. SDL mouse coordinates are "relative to window" (window
 *   coordinates / points), NOT physical pixels; the glue scales them by the
 *   window's pixel density before feeding the core, so the mouse and touch
 *   paths hand the core the same coordinate space (see inputMouseToPhysical).
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
	case SDLK_R:
		*out = INPUT_KEY_R;
		return true;
	case SDLK_T:
		*out = INPUT_KEY_T;
		return true;
	case SDLK_F:
		*out = INPUT_KEY_F;
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

/* Convert window coordinates (SDL mouse x/y, "relative to window") to
 * PHYSICAL pixels so the core receives the same space as the touch path.
 *
 * SDL_GetWindowPixelDensity() is documented in SDL_video.h as "a ratio of
 * pixel size to window size. For example, if the window is 1920x1080 and it
 * has a high density back buffer of 3840x2160 pixels, it would have a pixel
 * density of 2.0." That ratio is exactly the window->pixel factor. (The
 * similarly named SDL_GetWindowDisplayScale is the UI content scale, not this
 * ratio.) A window with no high-density back buffer reports 1.0; a lookup
 * failure (<= 0) falls back to identity so a missing window never zeroes a
 * coordinate. */
static void inputMouseToPhysical(SDL_Window *window, float *x, float *y)
{
	float density = window != NULL ? SDL_GetWindowPixelDensity(window) : 0.0f;

	if (!(density > 0.0f))
		density = 1.0f;
	*x *= density;
	*y *= density;
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
		    event->button.which != SDL_TOUCH_MOUSEID) {
			float x = event->button.x;
			float y = event->button.y;

			inputMouseToPhysical(
				SDL_GetWindowFromID(event->button.windowID), &x, &y);
			inputPointerDown(input, x, y);
		}
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event->button.button == SDL_BUTTON_LEFT &&
		    event->button.which != SDL_TOUCH_MOUSEID) {
			float x = event->button.x;
			float y = event->button.y;

			inputMouseToPhysical(
				SDL_GetWindowFromID(event->button.windowID), &x, &y);
			inputPointerUp(input, x, y);
		}
		break;
	case SDL_EVENT_MOUSE_MOTION:
		if ((event->motion.state & SDL_BUTTON_LMASK) &&
		    event->motion.which != SDL_TOUCH_MOUSEID) {
			float x = event->motion.x;
			float y = event->motion.y;

			inputMouseToPhysical(
				SDL_GetWindowFromID(event->motion.windowID), &x, &y);
			inputPointerMove(input, x, y);
		}
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
