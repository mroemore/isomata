/* isomata/src/platform/platform_android.c — Android twin (NDK build).
 *
 * Compiled only by android/app/jni/CMakeLists.txt (never by Meson; the
 * Meson cross files under cross/ stay Linux-on-Linux). Implements the exact
 * contract in platform.h; differences from the desktop twin are
 * platformAssetPath() (SDL on Android resolves APK assets itself and wants
 * the bare relative name), platformDisplayDensity() (window display scale)
 * and platformUiScaleBoost() (touch targets).
 */
#include "platform.h"

#include <SDL3/SDL.h>

/* Touch targets read ~30% larger than the dp baseline on a phone; composed
 * on top of the clamped density scale (platform.h). */
#define ANDROID_UI_SCALE_BOOST 1.3f

static const SDL_DisplayMode *androidCurrentMode(void) {
	const SDL_DisplayID display = SDL_GetPrimaryDisplay();
	return SDL_GetCurrentDisplayMode(display);
}

float platformDisplayDensity(void *window) {
	SDL_Window *w = window;

	/* Prefer the window's display scale: a points -> pixels ratio (the dp
	 * density Android itself uses). The display mode's pixel_density is
	 * reported as 1.0 on the tested Android targets, so it is only the
	 * fallback, then 1.0. */
	if (w != NULL) {
		const float scale = SDL_GetWindowDisplayScale(w);

		if (scale > 0.0f) {
			return scale;
		}
	}
	{
		const SDL_DisplayMode *mode = androidCurrentMode();

		if (mode && mode->pixel_density > 0.0f) {
			return mode->pixel_density;
		}
	}
	return 1.0f;
}

float platformUiScaleBoost(void) {
	return ANDROID_UI_SCALE_BOOST;
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

void platformSafeArea(void *window, int *outX, int *outY, int *outW, int *outH) {
	SDL_Window *w = window;
	SDL_Rect rect = { 0, 0, 0, 0 };

	if (w != NULL) {
		int pw = 0;
		int ph = 0;

		if (SDL_GetWindowSizeInPixels(w, &pw, &ph)) {
			rect.w = pw;
			rect.h = ph;
			/* Android cutouts/status/navigation bars show up here;
			 * with none, SDL reports the full client rect. */
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
