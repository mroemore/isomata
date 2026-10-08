/* isomata/src/platform/platform_android.c — Android twin (NDK build).
 *
 * Compiled only by android/app/jni/CMakeLists.txt (never by Meson; the
 * Meson cross files under cross/ stay Linux-on-Linux). Implements the exact
 * contract in platform.h; differences from the desktop twin are confined to
 * platformAssetPath(), because SDL on Android resolves APK assets itself and
 * wants the bare relative name.
 */
#include "platform.h"

#include <SDL3/SDL.h>

static const SDL_DisplayMode *androidCurrentMode(void) {
	const SDL_DisplayID display = SDL_GetPrimaryDisplay();
	return SDL_GetCurrentDisplayMode(display);
}

float platformDisplayDensity(void) {
	const SDL_DisplayMode *mode = androidCurrentMode();
	if (!mode || mode->pixel_density <= 0.0f) {
		return 1.0f;
	}
	return mode->pixel_density;
}

void platformDisplaySize(int *outWidth, int *outHeight) {
	const SDL_DisplayMode *mode = androidCurrentMode();
	if (outWidth) {
		*outWidth = mode ? mode->w : 0;
	}
	if (outHeight) {
		*outHeight = mode ? mode->h : 0;
	}
}

const char *platformAssetPath(const char *relativeName, char *buffer, size_t bufferSize) {
	/* SDL_IOStream on Android resolves asset names through the APK's
	 * AAssetManager, so "fonts/foo.ttf" opens the packed asset directly. */
	if (!buffer || bufferSize == 0 || !relativeName || *relativeName == '\0') {
		return NULL;
	}
	const int written = SDL_snprintf(buffer, bufferSize, "%s", relativeName);
	if (written < 0 || (size_t)written >= bufferSize) {
		return NULL;
	}
	return buffer;
}
