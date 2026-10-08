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
