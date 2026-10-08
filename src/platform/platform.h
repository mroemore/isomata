/* isomata/src/platform/platform.h — the platform abstraction contract.
 *
 * This header is the ONLY seam between engine code and per-OS behavior.
 * Invariants (dotzy platform style):
 *   - Dependency-free: this header includes no SDL headers and defines no
 *     SDL types; signatures use standard C types only, so engine code can
 *     hold it without pulling SDL in.
 *   - Twin files implement exactly these signatures:
 *       platform.c         — desktop/portable SDL path (Linux and friends);
 *                            the only platform source the Meson build compiles.
 *       platform_android.c — Android NDK path; compiled only via
 *                            android/app/jni/CMakeLists.txt.
 *   - The no-ifdef rule: `__ANDROID__` conditionals live only in src/platform/
 *     (plus the single entry-point exception in src/main.c). Engine code never
 *     branches on platform; it calls these functions and trusts the twins to
 *     agree on semantics.
 *   - Twins must agree on semantics:
 *       * platformDisplayDensity(void *window) is the OS UI scale (1.0 when
 *         unknown), the value to multiply logical pixels by for physical
 *         pixels. `window` is an SDL_Window* passed as void* (this header
 *         stays SDL-free); the Android twin reads the window's display
 *         scale, the desktop twin ignores the window and keeps the display
 *         mode's pixel density.
 *       * platformUiScaleBoost() is a per-platform touch-target multiplier
 *         composed on TOP of the clamped density scale (1.0 on desktop, > 1
 *         on touch platforms). The caller composes:
 *             uiScale = uiScaleFromDensity(platformDisplayDensity(window))
 *                       * platformUiScaleBoost();
 *         The ui_scale.h [1, 3] clamp applies to the density step only, so a
 *         boosted scale may exceed 3.
 *       * platformDisplaySize() is the current display mode in *physical*
 *         pixels of the primary display; 0/0 when unavailable.
 *       * platformSafeArea() is the window's usable inset (notch / status
 *         bar / gesture bar) in *physical* pixels of the window's client
 *         area. When SDL reports no usable inset (or cannot answer) the twin
 *         returns the full client rect (0,0,size); for a NULL window it
 *         writes 0s. Callers therefore always receive a valid rect to lay
 *         the UI root in.
 *       * platformAssetPath() returns a SDL_IOStream()-compatible string for
 *         the given relative asset name, written into caller storage; NULL on
 *         failure or when the buffer is too small. Desktop twins prefix the
 *         sandbox/install dir; the Android twin returns the bare relative name
 *         because SDL resolves APK assets for it.
 */
#ifndef ISOMATA_PLATFORM_H
#define ISOMATA_PLATFORM_H

#include <stddef.h>

/* OS UI scale factor, 1.0 when unknown. `window` is an SDL_Window* passed as
 * void* (may be NULL); the desktop twin ignores it and reports the primary
 * display's pixel density, the Android twin prefers the window's display
 * scale (points -> pixels, the dp density Android itself uses). */
float platformDisplayDensity(void *window);

/* Touch-target multiplier composed on top of the clamped density scale:
 * 1.0 on desktop, > 1 on touch platforms (a named constant per twin). The
 * caller must apply uiScaleFromDensity() to the density BEFORE multiplying. */
float platformUiScaleBoost(void);

/* Current physical-pixel size of the primary display; out params may be NULL
 * to skip. Writes 0 for a value that is unavailable. */
void platformDisplaySize(int *outWidth, int *outHeight);

/* Safe-area inset of a window's client area, in physical pixels. `window` is
 * an SDL_Window* passed as void* so this header stays SDL-free; a NULL
 * window writes 0s. Writes the full client rect when SDL reports no usable
 * inset. Any out pointer may be NULL. */
void platformSafeArea(void *window, int *outX, int *outY, int *outW, int *outH);

/* Compose the SDL_IOStream()-compatible path for an asset name (no leading
 * slash, e.g. "fonts/test.ttf"). Returns buffer on success, NULL when
 * buffer/env are NULL or the name does not fit. */
const char *platformAssetPath(const char *relativeName, char *buffer, size_t bufferSize);

#endif /* ISOMATA_PLATFORM_H */
