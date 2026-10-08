#ifndef ISOMATA_RENDER_GPU_BACKEND_H
#define ISOMATA_RENDER_GPU_BACKEND_H

/*
 * SDL_gpu render backend for the isometric slice. This is the SDL tier:
 * the ONLY render file that includes SDL, compiled into the app target
 * only (never isomata_pure, never the headless suite).
 *
 * Scope (Task 8 milestone): one graphics pipeline for textured quads
 * (world-space position + uv + vertex color), a single nearest-sampled
 * texture, alpha blending, and NO depth attachment — the pure drawlist
 * painter-sorts, so the backend stays depth-free. gpuBackendDrawList uploads
 * the sorted list as vertices and issues one draw call; the Task 7 hardcoded
 * quad is gone.
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
#include "render/math3d.h"

#include <SDL3/SDL.h>

typedef struct GpuBackend GpuBackend;

/* Create the GPU device, claim the window's swapchain, and build the quad
 * pipeline, placeholder texture, and sampler.
 *
 *   shaderDir      directory holding world.vert.spv / world.frag.spv
 *   texturePath    the placeholder PNG
 *   screenshotPath if non-NULL, the backend renders one offscreen frame and
 *                  writes it as a PNG here (verification hook)
 *
 * Returns NULL on failure; SDL_LogError carries the reason and the stage.
 */
GpuBackend *gpuBackendCreate(SDL_Window *window, const char *shaderDir,
			     const char *texturePath, const char *screenshotPath);

/* Release every GPU object, unclaim the window, and destroy the device.
 * NULL is a no-op. */
void gpuBackendDestroy(GpuBackend *gpu);

/* Acquire a swapchain texture, clear it, upload the draw list as vertices and
 * draw it in ONE call with the atlas texture, transformed by viewProj
 * (column-major, per math3d). The list must already be painter-sorted. The
 * backend keeps a vertex buffer sized to the list's capacity and grows it only
 * when a larger list arrives. Returns false on command-buffer, swapchain or
 * upload failure (logged). */
bool gpuBackendDrawList(GpuBackend *gpu, const Mat4 *viewProj,
			const DrawList *list);

#endif /* ISOMATA_RENDER_GPU_BACKEND_H */
