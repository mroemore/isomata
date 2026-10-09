#ifndef ISOMATA_RENDER_GPU_BACKEND_H
#define ISOMATA_RENDER_GPU_BACKEND_H

/*
 * SDL_gpu render backend for the isometric slice. This is the SDL tier:
 * the ONLY render file that includes SDL, compiled into the app target
 * only (never isomata_pure, never the headless suite).
 *
 * Scope (Task 9): one graphics pipeline for textured quads (screen- or
 * world-space position + uv + vertex color), a single nearest-sampled
 * texture per draw, alpha blending, and NO depth attachment — the pure
 * drawlist painter-sorts, so the backend stays depth-free.
 *
 * Frame model (Task 9). A frame is one command buffer and one swapchain
 * texture, driven by the app as:
 *
 *   gpuBackendBeginFrame(gpu);                 // acquire cmd + swapchain
 *   ... drawSceneStackAll ...                  // scene draws record work
 *     gpuBackendDrawList(gpu, viewProj, list); // world list (one draw)
 *     gpuBackendDrawUiQuad(gpu, tex, ...);     // screen-space UI quad
 *   gpuBackendEndFrame(gpu);                   // upload, one pass, present
 *
 * BeginFrame does NOT open the render pass: UI text textures are created
 * and uploaded (copy passes) while scenes draw, and SDL_gpu forbids a copy
 * pass inside a render pass. EndFrame uploads the accumulated vertices,
 * opens ONE render pass (clear), replays the world list then every UI quad
 * in order, optionally captures a screenshot, and submits. UI quads are in
 * PHYSICAL pixels with a top-left origin; EndFrame pushes an orthographic
 * screen projection for them.
 *
 * Shader resource bindings follow SDL_gpu's SPIR-V convention (quoted from
 * /usr/include/SDL3/SDL_gpu.h, SDL_CreateGPUShader): vertex uniform buffers
 * at set 1, fragment sampled textures at set 2. The view-projection matrix
 * is pushed with SDL_PushGPUVertexUniformData(cmd, 0, ...).
 *
 * Failure policy: every create step logs a diagnostic via SDL_LogError that
 * names the stage and the path that failed, then unwinds and returns NULL.
 * A failed backend never leaves GPU objects behind.
 */

#include "render/drawlist.h"
#include "render/grid.h"
#include "render/materials.h"
#include "render/math3d.h"

#include <SDL3/SDL.h>

typedef struct GpuBackend GpuBackend;

/* Create the GPU device, claim the window's swapchain, and build the quad
 * pipeline, runtime texture atlas, and sampler.
 *
 *   shaderDir      directory holding world.vert.spv / world.frag.spv
 *   texturesDir    directory holding materials.txt and the material PNGs;
 *                  all referenced files are packed into ONE RGBA atlas (one
 *                  bind per frame). Missing/malformed entries fall back to a
 *                  generated texture and a diagnostic; the backend still
 *                  builds.
 *   screenshotPath if non-NULL, the next frame also renders offscreen at
 *                  the swapchain size and writes it as a PNG here
 *
 * Returns NULL on failure; SDL_LogError carries the reason and the stage.
 */
GpuBackend *gpuBackendCreate(SDL_Window *window, const char *shaderDir,
			     const char *texturesDir, const char *screenshotPath);

/* The material table built from texturesDir/materials.txt (name -> 6 face UV
 * rects + alpha mode). Never NULL for a live backend; its count may be 0 when
 * the manifest was unavailable (emitters then use the built-in fallback). */
const MaterialTable *gpuBackendMaterials(GpuBackend *gpu);

/* The 4-corner UV of the reserved white atlas cell, for the light-debug view.
 * Returns false (uv untouched) for a NULL backend/out. */
bool gpuBackendDebugUV(const GpuBackend *gpu, float uv[4][2]);

/* Release every GPU object, unclaim the window, and destroy the device.
 * NULL is a no-op. */
void gpuBackendDestroy(GpuBackend *gpu);

/* The device the backend owns, for SDL-tier helpers (ui_gpu) that create
 * their own textures on it. NULL for NULL. */
SDL_GPUDevice *gpuBackendDevice(GpuBackend *gpu);

/* The active frame's command buffer, or NULL when no frame is open. ui_gpu
 * uploads its text textures on it (a copy pass outside the render pass). */
SDL_GPUCommandBuffer *gpuBackendFrameCommandBuffer(GpuBackend *gpu);

/* Begin a frame: acquire the command buffer and swapchain texture and reset
 * the frame's draw batch. Returns false (and leaves no frame open) when the
 * device is missing or the command buffer/swapchain cannot be acquired. */
bool gpuBackendBeginFrame(GpuBackend *gpu);

/* End the frame: upload the staged world + UI vertices, open the swapchain
 * render pass, replay the world list then the UI quads in order, run the
 * optional screenshot capture, and submit. Returns false when no frame is
 * open. */
bool gpuBackendEndFrame(GpuBackend *gpu);

/* Stage a painter-sorted world DrawList for this frame's one world draw,
 * transformed by viewProj (column-major, per math3d). The list must already
 * be sorted. Returns false when no frame is open or on staging failure. */
bool gpuBackendDrawList(GpuBackend *gpu, const Mat4 *viewProj,
			const DrawList *list);

/* Stage the infinite ground-grid quad for this frame, transformed by viewProj.
 * It is drawn BEFORE the world list (painter's order), so map faces occlude
 * it and it shows only on empty/void tiles and beyond the map. The fragment
 * fade block (quad centre XZ + fadeStart/fadeEnd) is pushed to the grid
 * pipeline's set 3 binding 0. A backend built without a grid pipeline (a
 * tolerant init failure) accepts the call and draws nothing. Returns false
 * when no frame is open or on staging failure. */
bool gpuBackendDrawGrid(GpuBackend *gpu, const Mat4 *viewProj,
			const GridQuad *quad);

/* Stage one screen-space textured quad (PHYSICAL pixels, top-left origin)
 * into this frame. `uv` is the 4-corner UV in canonical order (top-left,
 * top-right, bottom-right, bottom-left) and `rgba` is the tint multiplied
 * into the sampled texel. Consecutive quads sharing a texture are batched
 * into one draw. Returns false when no frame is open. */
bool gpuBackendDrawUiQuad(GpuBackend *gpu, SDL_GPUTexture *texture,
			  float x, float y, float w, float h,
			  const float uv[4][2], uint32_t rgba);

#endif /* ISOMATA_RENDER_GPU_BACKEND_H */
