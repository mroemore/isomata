#ifndef ISOMATA_UI_GPU_H
#define ISOMATA_UI_GPU_H

/*
 * SDL_gpu implementation of the abstract UiDrawCtx (element.h). This is the
 * SDL tier: it includes SDL3/SDL3_ttf and is compiled into the app target
 * only, never isomata_pure.
 *
 * Rendering:
 * - fillRect scales the virtual rect to physical pixels (ui_scale) and
 *   submits one screen-space quad sampling a dedicated 1x1 WHITE texture
 *   through the backend's quad path, tinted by the requested RGBA. (The
 *   placeholder atlas has no white texel, so ui_gpu owns this one.)
 * - drawText renders the string with SDL3_ttf's SURFACE engine at
 *   pixelSize * uiScale (so text stays crisp at density), converts to
 *   RGBA32, uploads it as a GPU texture and draws it as a tinted quad
 *   (nearest sampling, matching the pipeline). Textures are cached in a
 *   small fixed cache (UI_GPU_TEXT_CACHE entries); when full the oldest
 *   entry is destroyed and its slot reused (FIFO eviction).
 * - drawImage draws one of the five UI icons. uiGpuCreate best-effort loads
 *   them from assets/icons/ (resolved through platformAssetPath, so Android
 *   APK assets work) as nearest-sampled textures and releases them in
 *   uiGpuDestroy. A missing/unreadable PNG is logged once and that icon
 *   simply draws nothing — it never fails uiGpuCreate or crashes a frame.
 * - No SDL_Renderer is used anywhere.
 *
 * The UiDrawCtx passed to uiDraw is owned by the UiGpu; the UiGpu embeds
 * the ctx as its first member so the vtable callbacks recover it.
 */

#include "ui/element.h"

#include <stdbool.h>

/* Small bounded text-texture cache; FIFO eviction when full. */
#define UI_GPU_TEXT_CACHE 32

typedef struct GpuBackend GpuBackend;
typedef struct UiGpu UiGpu;

/* Create the draw context over a live backend. `fontPath` is the TTF asset
 * and `fontPixelSize` the design pixel size (scaled per draw by uiScale);
 * both must be valid. Returns NULL on failure (SDL_LogError carries the
 * reason). */
UiGpu *uiGpuCreate(GpuBackend *gpu, const char *fontPath, int fontPixelSize);

/* Release the cached textures, the white texture, the font and self. NULL is
 * a no-op. */
void uiGpuDestroy(UiGpu *ui);

/* The vtable-backed draw context, owned by ui. NULL for NULL ui. */
UiDrawCtx *uiGpuDrawCtx(UiGpu *ui);

/* Physical-pixel scale (DPI) used to size rects and text. 1.0 when unset. */
void uiGpuSetScale(UiGpu *ui, float scale);

#endif /* ISOMATA_UI_GPU_H */
