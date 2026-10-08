/* isomata/src/platform/platform.c — desktop twin (Linux and friends).
 *
 * Compiled by the Meson build only. The Android twin is
 * platform_android.c (see platform.h for the contract). No __ANDROID__
 * conditionals here and none needed.
 */
#include "platform.h"

#include <SDL3/SDL.h>

const SDL_DisplayMode *platformCurrentMode(void) {
	const SDL_DisplayID display = SDL_GetPrimaryDisplay();
	return SDL_GetCurrentDisplayMode(display);
}

float platformDisplayDensity(void) {
	const SDL_DisplayMode *mode = platformCurrentMode();
	if (!mode || mode->pixel_density <= 0.0f) {
		return 1.0f;
	}
	return mode->pixel_density;
}

void platformDisplaySize(int *outWidth, int *outHeight) {
	const SDL_DisplayMode *mode = platformCurrentMode();
	if (outWidth) {
		*outWidth = mode ? mode->w : 0;
	}
	if (outHeight) {
		*outHeight = mode ? mode->h : 0;
	}
}

void platformSafeArea(void *window, int *outX, int *outY, int *outW, int *outH) {
	SDL_Window *w = window;
	SDL_Rect rect = { 0, 0, 0, 0 };

	if (w != NULL) {
		int pw = 0;
		int ph = 0;

		if (SDL_GetWindowSizeInPixels(w, &pw, &ph)) {
			rect.w = pw;
			rect.h = ph;
			/* No usable inset (or SDL cannot answer): fall back to
			 * the full client rect so the caller always gets a
			 * valid rect. */
			if (!SDL_GetWindowSafeArea(w, &rect) || rect.w <= 0 ||
			    rect.h <= 0) {
				rect.x = 0;
				rect.y = 0;
				rect.w = pw;
				rect.h = ph;
			}
		}
	}
	if (outX) {
		*outX = rect.x;
	}
	if (outY) {
		*outY = rect.y;
	}
	if (outW) {
		*outW = rect.w;
	}
	if (outH) {
		*outH = rect.h;
	}
}

const char *platformAssetPath(const char *relativeName, char *buffer, size_t bufferSize) {
	if (!buffer || bufferSize == 0 || !relativeName || *relativeName == '\0') {
		return NULL;
	}
	const char *basePath = SDL_GetBasePath();
	if (!basePath) {
		return NULL;
	}
	const int written = SDL_snprintf(buffer, bufferSize, "%sassets/%s", basePath, relativeName);
	if (written < 0 || (size_t)written >= bufferSize) {
		return NULL;
	}
	return buffer;
}
