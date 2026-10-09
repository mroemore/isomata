/*
 * SDL_gpu render backend (see gpu_backend.h for the contract).
 *
 * Resource graph built at create time and torn down in reverse:
 *   device -> claimed window -> {pipeline, vertex buffers, textures, sampler}
 * Shaders are released as soon as the pipeline exists.
 *
 * Frame model (Task 9): one command buffer and one swapchain texture per
 * frame. BeginFrame acquires them and resets the draw batch; scenes stage
 * world vertices (gpuBackendDrawList) and screen-space UI quads
 * (gpuBackendDrawUiQuad) while no render pass is open (so ui_gpu can still
 * run its texture-upload copy passes); EndFrame uploads the staged
 * vertices, opens ONE render pass, replays world then UI, runs the optional
 * screenshot capture, and submits. The optional capture replays the same
 * draws into a swapchain-sized offscreen texture.
 */

#include "render/gpu_backend.h"

#include "render/atlas.h"
#include "render/materials.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_surface.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define GPU_VERTICES_PER_ITEM 6
#define GPU_SCREENSHOT_PATH_MAX 512
#define GPU_UI_QUAD_VERTICES 6
#define GPU_UI_DRAW_INITIAL 32
#define GPU_GRID_VERTICES 6
/* Grid line colour, carried as the grid quad's vertex tint (the grid fragment
 * shader uses vColor.rgb for the lines). A cool grey that reads on the dark
 * clear colour without competing with the map. */
#define GPU_GRID_TINT DRAW_TINT(150, 160, 182, 255)

typedef struct GpuVertex {
	float x;
	float y;
	float z;
	float u;
	float v;
	float r;
	float g;
	float b;
	float a;
	float alphaMode;	/* 1.0 = cutout (frag discards low alpha) */
} GpuVertex;

/* One batched UI draw: a run of UI vertices sharing one texture. */
typedef struct GpuUiDraw {
	SDL_GPUTexture *texture;
	size_t first;		/* first vertex in the UI vertex buffer */
	size_t count;
} GpuUiDraw;

struct GpuBackend {
	SDL_GPUDevice *device;
	SDL_Window *window;
	bool claimed;

	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUBuffer *vertexBuffer;
	SDL_GPUTransferBuffer *vertexTransfer;	/* staging for the world list */
	size_t vertexItemCapacity;		/* items the world buffers hold */
	SDL_GPUTexture *texture;		/* world atlas (packed materials) */
	SDL_GPUSampler *sampler;
	SDL_GPUTextureFormat colorFormat;
	MaterialTable materials;		/* name -> face UV rects + alpha */

	/* Infinite ground grid (tolerant: pipeline/buffers NULL if init failed). */
	SDL_GPUGraphicsPipeline *gridPipeline;
	SDL_GPUBuffer *gridVertexBuffer;
	SDL_GPUTransferBuffer *gridVertexTransfer;

	/* Optional screenshot capture (verification hook). */
	char screenshotPath[GPU_SCREENSHOT_PATH_MAX];
	bool screenshotPending;
	SDL_GPUTexture *captureTexture;
	SDL_GPUTransferBuffer *captureTransfer;
	Uint32 captureWidth;
	Uint32 captureHeight;

	/* Frame state. */
	bool frameActive;
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUTexture *swapchain;
	Uint32 swapW;
	Uint32 swapH;

	Mat4 worldViewProj;
	bool worldPending;
	size_t worldVertexCount;

	/* Grid frame state (staged by gpuBackendDrawGrid). */
	Mat4 gridViewProj;
	float gridCenterX;
	float gridCenterZ;
	float gridFadeStart;
	float gridFadeEnd;
	bool gridPending;

	/* UI batch: screen-space vertices staged on the CPU, plus the texture
	 * runs recorded while scenes draw. */
	GpuVertex *uiVerts;
	size_t uiVertCount;
	size_t uiVertCapacity;
	SDL_GPUBuffer *uiVertexBuffer;
	SDL_GPUTransferBuffer *uiVertexTransfer;
	size_t uiVertexCapacity;	/* vertices the UI buffers hold */
	GpuUiDraw *uiDraws;
	size_t uiDrawCount;
	size_t uiDrawCapacity;
};

/* --- small helpers ---------------------------------------------------- */

/* Read a whole file into a fresh SDL-allocated buffer. SDL_LoadFile resolves
 * APK assets on Android (the platform seam returns the bare relative asset
 * name there) and plain filesystem paths on desktop (the seam returns
 * <basePath>/assets/...). The old stdio path could not read APK assets, so
 * the Android GPU tier failed to find its SPIR-V. Caller frees with
 * SDL_free. */
static Uint8 *readFile(const char *path, size_t *outSize)
{
	size_t size = 0;
	void *data;

	*outSize = 0;
	data = SDL_LoadFile(path, &size);
	if (data == NULL || size == 0) {
		SDL_free(data);
		return NULL;
	}
	*outSize = size;
	return data;
}

/* Build the join of a directory and a file name (no allocation). */
static const char *joinPath(char *buffer, size_t size, const char *dir,
			    const char *name)
{
	int written = SDL_snprintf(buffer, size, "%s/%s", dir, name);

	if (written < 0 || (size_t)written >= size)
		return NULL;
	return buffer;
}

/* Create one SPIR-V shader stage. Logs the failing path and stage. */
static SDL_GPUShader *createShader(SDL_GPUDevice *device, const char *path,
				   SDL_GPUShaderStage stage, Uint32 uniformBuffers,
				   Uint32 samplers)
{
	size_t codeSize = 0;
	Uint8 *code = readFile(path, &codeSize);
	SDL_GPUShaderCreateInfo info = { 0 };
	SDL_GPUShader *shader;

	if (code == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: cannot read %s shader '%s'",
			     stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
			     path);
		return NULL;
	}
	info.code_size = codeSize;
	info.code = code;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage = stage;
	info.num_uniform_buffers = uniformBuffers;
	info.num_samplers = samplers;
	shader = SDL_CreateGPUShader(device, &info);
	if (shader == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_CreateGPUShader failed for %s shader '%s': %s",
			     stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
			     path, SDL_GetError());
	}
	SDL_free(code);
	return shader;
}

/* --- pipeline / geometry / texture ------------------------------------ */

/* Build a quad graphics pipeline from an already-created vertex/fragment
 * shader pair: world-space position + uv + vertex color, alpha blending, no
 * depth, no culling (painter-sorted). Shared by the world and grid pipelines
 * so they cannot drift apart. Returns NULL on failure (caller logs). */
static SDL_GPUGraphicsPipeline *createQuadPipeline(SDL_GPUDevice *device,
						   SDL_GPUShader *vs,
						   SDL_GPUShader *fs,
						   SDL_GPUTextureFormat colorFormat)
{
	SDL_GPUVertexBufferDescription bufferDesc = { 0 };
	SDL_GPUVertexAttribute attributes[4] = { { 0 } };
	SDL_GPUVertexInputState vertexInput = { 0 };
	SDL_GPUColorTargetDescription colorTarget = { 0 };
	SDL_GPUGraphicsPipelineCreateInfo info = { 0 };

	bufferDesc.slot = 0;
	bufferDesc.pitch = sizeof(GpuVertex);
	bufferDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	bufferDesc.instance_step_rate = 0;
	attributes[0].location = 0;
	attributes[0].buffer_slot = 0;
	attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attributes[0].offset = offsetof(GpuVertex, x);
	attributes[1].location = 1;
	attributes[1].buffer_slot = 0;
	attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attributes[1].offset = offsetof(GpuVertex, u);
	attributes[2].location = 2;
	attributes[2].buffer_slot = 0;
	attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	attributes[2].offset = offsetof(GpuVertex, r);
	attributes[3].location = 3;
	attributes[3].buffer_slot = 0;
	attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
	attributes[3].offset = offsetof(GpuVertex, alphaMode);
	vertexInput.vertex_buffer_descriptions = &bufferDesc;
	vertexInput.num_vertex_buffers = 1;
	vertexInput.vertex_attributes = attributes;
	vertexInput.num_vertex_attributes = 4;

	/* Alpha blending, no depth (painter-sorted), no culling. */
	colorTarget.format = colorFormat;
	colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	colorTarget.blend_state.color_write_mask =
		SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
		SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
	colorTarget.blend_state.enable_blend = true;
	colorTarget.blend_state.enable_color_write_mask = true;

	info.vertex_shader = vs;
	info.fragment_shader = fs;
	info.vertex_input_state = vertexInput;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	info.rasterizer_state.enable_depth_clip = true;
	info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.depth_stencil_state.enable_depth_test = false;
	info.depth_stencil_state.enable_depth_write = false;
	info.target_info.color_target_descriptions = &colorTarget;
	info.target_info.num_color_targets = 1;
	info.target_info.has_depth_stencil_target = false;

	return SDL_CreateGPUGraphicsPipeline(device, &info);
}

/* Build the world pipeline (world.vert.spv + world.frag.spv). A failure here
 * is fatal to the backend (createPipeline returns false). */
static bool createPipeline(GpuBackend *gpu, const char *shaderDir,
			   SDL_GPUTextureFormat colorFormat)
{
	char path[GPU_SCREENSHOT_PATH_MAX];
	SDL_GPUShader *vs;
	SDL_GPUShader *fs;

	if (joinPath(path, sizeof(path), shaderDir, "world.vert.spv") == NULL ||
	    (vs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_VERTEX, 1, 0)) == NULL)
		return false;
	if (joinPath(path, sizeof(path), shaderDir, "world.frag.spv") == NULL ||
	    (fs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1)) == NULL) {
		SDL_ReleaseGPUShader(gpu->device, vs);
		return false;
	}

	gpu->pipeline = createQuadPipeline(gpu->device, vs, fs, colorFormat);
	SDL_ReleaseGPUShader(gpu->device, vs);
	SDL_ReleaseGPUShader(gpu->device, fs);
	if (gpu->pipeline == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_CreateGPUGraphicsPipeline failed: %s",
			     SDL_GetError());
		return false;
	}
	return true;
}

/* Build the grid pipeline (world.vert.spv reused verbatim + grid.frag.spv,
 * whose fragment UBO is at set 3). A failure is tolerated by the caller: the
 * grid is disabled and the app keeps running. */
static bool createGridPipeline(GpuBackend *gpu, const char *shaderDir,
			       SDL_GPUTextureFormat colorFormat)
{
	char path[GPU_SCREENSHOT_PATH_MAX];
	SDL_GPUShader *vs;
	SDL_GPUShader *fs;

	if (joinPath(path, sizeof(path), shaderDir, "world.vert.spv") == NULL ||
	    (vs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_VERTEX, 1, 0)) == NULL)
		return false;
	if (joinPath(path, sizeof(path), shaderDir, "grid.frag.spv") == NULL ||
	    (fs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0)) == NULL) {
		SDL_ReleaseGPUShader(gpu->device, vs);
		return false;
	}

	gpu->gridPipeline = createQuadPipeline(gpu->device, vs, fs, colorFormat);
	SDL_ReleaseGPUShader(gpu->device, vs);
	SDL_ReleaseGPUShader(gpu->device, fs);
	if (gpu->gridPipeline == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: grid pipeline creation failed: %s",
			     SDL_GetError());
		return false;
	}
	return true;
}

/* Create the grid's 6-vertex buffer and its staging transfer buffer. */
static bool createGridBuffers(GpuBackend *gpu)
{
	size_t bytes = GPU_GRID_VERTICES * sizeof(GpuVertex);

	gpu->gridVertexTransfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = (Uint32)bytes,
		});
	if (gpu->gridVertexTransfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: grid vertex transfer buffer failed: %s",
			     SDL_GetError());
		return false;
	}
	gpu->gridVertexBuffer = SDL_CreateGPUBuffer(gpu->device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
			.size = (Uint32)bytes,
		});
	if (gpu->gridVertexBuffer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: grid vertex buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->gridVertexTransfer);
		gpu->gridVertexTransfer = NULL;
		return false;
	}
	return true;
}

/* Create a vertex buffer and its staging transfer buffer, sized to hold
 * `itemCapacity` draw items (6 vertices each). */
static bool createVertexBuffers(GpuBackend *gpu, size_t itemCapacity)
{
	size_t bytes = itemCapacity * GPU_VERTICES_PER_ITEM * sizeof(GpuVertex);

	gpu->vertexTransfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = (Uint32)bytes,
		});
	if (gpu->vertexTransfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex transfer buffer failed: %s",
			     SDL_GetError());
		return false;
	}
	gpu->vertexBuffer = SDL_CreateGPUBuffer(gpu->device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
			.size = (Uint32)bytes,
		});
	if (gpu->vertexBuffer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex buffer failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->vertexTransfer);
		gpu->vertexTransfer = NULL;
		return false;
	}
	gpu->vertexItemCapacity = itemCapacity;
	return true;
}

/* Grow the world vertex buffers to hold `itemCapacity` items. A no-op when
 * the current buffers already fit. */
static bool ensureVertexCapacity(GpuBackend *gpu, size_t itemCapacity)
{
	if (itemCapacity <= gpu->vertexItemCapacity)
		return true;
	if (gpu->vertexBuffer != NULL) {
		SDL_ReleaseGPUBuffer(gpu->device, gpu->vertexBuffer);
		gpu->vertexBuffer = NULL;
	}
	if (gpu->vertexTransfer != NULL) {
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->vertexTransfer);
		gpu->vertexTransfer = NULL;
	}
	gpu->vertexItemCapacity = 0;
	return createVertexBuffers(gpu, itemCapacity);
}

/* Create the UI vertex buffer + staging transfer, sized to `vertexCapacity`
 * vertices. */
static bool createUiVertexBuffers(GpuBackend *gpu, size_t vertexCapacity)
{
	size_t bytes = vertexCapacity * sizeof(GpuVertex);

	gpu->uiVertexTransfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = (Uint32)bytes,
		});
	if (gpu->uiVertexTransfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: ui vertex transfer buffer failed: %s",
			     SDL_GetError());
		return false;
	}
	gpu->uiVertexBuffer = SDL_CreateGPUBuffer(gpu->device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
			.size = (Uint32)bytes,
		});
	if (gpu->uiVertexBuffer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: ui vertex buffer failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->uiVertexTransfer);
		gpu->uiVertexTransfer = NULL;
		return false;
	}
	gpu->uiVertexCapacity = vertexCapacity;
	return true;
}

static bool ensureUiVertexCapacity(GpuBackend *gpu, size_t vertexCapacity)
{
	/* CPU staging first: gpuBackendDrawUiQuad appends into it. */
	if (vertexCapacity > gpu->uiVertCapacity) {
		GpuVertex *grown = realloc(gpu->uiVerts,
					   vertexCapacity * sizeof(GpuVertex));

		if (grown == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: ui vertex staging grow failed");
			return false;
		}
		gpu->uiVerts = grown;
		gpu->uiVertCapacity = vertexCapacity;
	}
	if (vertexCapacity <= gpu->uiVertexCapacity)
		return true;
	if (gpu->uiVertexBuffer != NULL) {
		SDL_ReleaseGPUBuffer(gpu->device, gpu->uiVertexBuffer);
		gpu->uiVertexBuffer = NULL;
	}
	if (gpu->uiVertexTransfer != NULL) {
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->uiVertexTransfer);
		gpu->uiVertexTransfer = NULL;
	}
	gpu->uiVertexCapacity = 0;
	return createUiVertexBuffers(gpu, vertexCapacity);
}

/* Create gpu->texture (R8G8B8A8_UNORM, nearest-sampled) and upload `pixels`
 * (RGBA8, `pitch` bytes per row). On failure gpu->texture may be set; the
 * backend's destroy releases it. */
static bool uploadTexture(GpuBackend *gpu, const void *pixels, Uint32 width,
			  Uint32 height, Uint32 pitch)
{
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUCopyPass *copy;
	void *mapped;

	gpu->texture = SDL_CreateGPUTexture(gpu->device,
		&(SDL_GPUTextureCreateInfo){
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = width,
			.height = height,
			.layer_count_or_depth = 1,
			.num_levels = 1,
			.sample_count = SDL_GPU_SAMPLECOUNT_1,
		});
	if (gpu->texture == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: atlas texture creation failed: %s",
			     SDL_GetError());
		return false;
	}
	transfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = pitch * height,
		});
	if (transfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: atlas transfer buffer failed: %s",
			     SDL_GetError());
		return false;
	}
	mapped = SDL_MapGPUTransferBuffer(gpu->device, transfer, false);
	if (mapped == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: atlas transfer map failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	memcpy(mapped, pixels, (size_t)pitch * (size_t)height);
	SDL_UnmapGPUTransferBuffer(gpu->device, transfer);

	cmd = SDL_AcquireGPUCommandBuffer(gpu->device);
	if (cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: atlas upload command buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: atlas upload copy pass failed: %s",
			     SDL_GetError());
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	SDL_UploadToGPUTexture(copy,
		&(SDL_GPUTextureTransferInfo){ .transfer_buffer = transfer, .offset = 0,
					       .pixels_per_row = width,
					       .rows_per_layer = height },
		&(SDL_GPUTextureRegion){ .texture = gpu->texture, .w = width,
					 .h = height, .d = 1 },
		false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
	return true;
}

/* Index of `name` in the unique file list, or -1. */
static int findFileSlot(char (*names)[MATERIAL_PATH_MAX], int count,
			const char *name)
{
	int k;

	for (k = 0; k < count; k++)
		if (strcmp(names[k], name) == 0)
			return k;
	return -1;
}

/* Load texturesDir/materials.txt, load every referenced PNG, pack them into
 * ONE RGBA atlas, build the material table, and upload the atlas. Tolerant by
 * design: a missing/malformed manifest or a missing file logs a diagnostic
 * and degrades to a generated fallback; the backend still builds. */
static bool createAtlas(GpuBackend *gpu, const char *texturesDir)
{
	char path[GPU_SCREENSHOT_PATH_MAX];
	MaterialManifest manifest;
	Uint8 *mtext = NULL;
	size_t msize = 0;
	char (*names)[MATERIAL_PATH_MAX] = NULL;
	SDL_Surface **surfaces = NULL;
	int *sizes = NULL;
	int fileCount = 0;
	int maxSize = ATLAS_CELL_MIN;
	AtlasLayout layout;
	uint8_t *pixels = NULL;
	bool ok = false;
	int i;
	int cell;

	memset(&manifest, 0, sizeof(manifest));
	if (joinPath(path, sizeof(path), texturesDir, "materials.txt") != NULL)
		mtext = readFile(path, &msize);
	if (mtext == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: cannot read material manifest under '%s'",
			     texturesDir);
	} else if (!materialManifestParse((const char *)mtext, msize,
					  &manifest)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: material manifest '%s' is malformed",
			     path);
		manifest.count = 0;
	}
	SDL_free(mtext);

	names = calloc((size_t)MATERIAL_MAX * 6, sizeof(*names));
	if (names == NULL)
		goto done;
	for (i = 0; i < (int)manifest.count; i++) {
		int f;

		for (f = 0; f < 6; f++) {
			const char *fn = manifest.defs[i].file[f];

			if (findFileSlot(names, fileCount, fn) < 0) {
				if (fileCount >= ATLAS_MAX_SLOTS) {
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
						     "gpu_backend: too many atlas files (> %d)",
						     ATLAS_MAX_SLOTS);
					goto done;
				}
				SDL_snprintf(names[fileCount], MATERIAL_PATH_MAX,
					     "%s", fn);
				fileCount++;
			}
		}
	}

	surfaces = calloc((size_t)(fileCount > 0 ? fileCount : 1),
			  sizeof(*surfaces));
	sizes = calloc((size_t)(fileCount > 0 ? fileCount : 1), sizeof(*sizes));
	if (surfaces == NULL || sizes == NULL)
		goto done;
	for (i = 0; i < fileCount; i++) {
		SDL_Surface *s;

		sizes[i] = ATLAS_CELL_MIN;
		if (joinPath(path, sizeof(path), texturesDir, names[i]) == NULL)
			continue;
		s = SDL_LoadPNG(path);
		if (s == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: texture '%s' missing (%s); using fallback",
				     path, SDL_GetError());
			continue;
		}
		surfaces[i] = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(s);
		if (surfaces[i] == NULL)
			continue;
		sizes[i] = surfaces[i]->w < surfaces[i]->h
				   ? surfaces[i]->w
				   : surfaces[i]->h;
		if (sizes[i] < 1)
			sizes[i] = ATLAS_CELL_MIN;
		if (sizes[i] > maxSize)
			maxSize = sizes[i];
	}

	cell = atlasCellSizeFor(maxSize);
	if (!atlasComputeLayout(fileCount > 0 ? fileCount : 1, cell, &layout))
		goto done;
	pixels = calloc((size_t)layout.width * (size_t)layout.height * 4, 1);
	if (pixels == NULL)
		goto done;
	for (i = 0; i < fileCount; i++) {
		int x;
		int y;

		if (!atlasSlotOrigin(&layout, i, &x, &y))
			continue;
		if (surfaces[i] != NULL)
			atlasBlitPixels(pixels, layout.width, layout.height, x,
					y, surfaces[i]->pixels, surfaces[i]->w,
					surfaces[i]->h, (int)surfaces[i]->pitch);
		else
			atlasFillFallbackCell(pixels, layout.width, layout.height,
					      x, y, sizes[i]);
	}

	memset(&gpu->materials, 0, sizeof(gpu->materials));
	for (i = 0; i < (int)manifest.count; i++) {
		int id = materialTableAdd(&gpu->materials, manifest.defs[i].name,
					  manifest.defs[i].alpha);
		int f;

		if (id < 0)
			break;
		for (f = 0; f < 6; f++) {
			int slot = findFileSlot(names, fileCount,
						manifest.defs[i].file[f]);
			AtlasRect r;

			if (slot < 0)
				continue;
			if (atlasSlotRect(&layout, slot, sizes[slot], &r))
				materialTableSetRect(&gpu->materials, id,
						     (FaceId)f, r);
		}
	}

	if (!uploadTexture(gpu, pixels, (Uint32)layout.width,
			   (Uint32)layout.height, (Uint32)layout.width * 4))
		goto done;
	SDL_Log("gpu_backend: atlas %dx%d (%d material%s, %d file%s)",
		layout.width, layout.height, (int)gpu->materials.count,
		gpu->materials.count == 1 ? "" : "s", fileCount,
		fileCount == 1 ? "" : "s");
	ok = true;

done:
	if (surfaces != NULL) {
		for (i = 0; i < fileCount; i++)
			SDL_DestroySurface(surfaces[i]);
		free(surfaces);
	}
	free(sizes);
	free(names);
	free(pixels);
	return ok;
}


static bool createSampler(GpuBackend *gpu)
{
	gpu->sampler = SDL_CreateGPUSampler(gpu->device,
		&(SDL_GPUSamplerCreateInfo){
			.min_filter = SDL_GPU_FILTER_NEAREST,
			.mag_filter = SDL_GPU_FILTER_NEAREST,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.min_lod = 0.0f,
			.max_lod = 1000.0f,
		});
	if (gpu->sampler == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_CreateGPUSampler failed: %s",
			     SDL_GetError());
		return false;
	}
	return true;
}

/* --- public API ------------------------------------------------------- */

GpuBackend *gpuBackendCreate(SDL_Window *window, const char *shaderDir,
			     const char *texturesDir, const char *screenshotPath)
{
	GpuBackend *gpu;
	SDL_GPUTextureFormat colorFormat;
	bool debug = SDL_getenv("ISO_GPU_DEBUG") != NULL;

	if (window == NULL || shaderDir == NULL || texturesDir == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: create needs a window, shader dir and textures dir");
		return NULL;
	}
	gpu = calloc(1, sizeof(*gpu));
	if (gpu == NULL)
		return NULL;
	gpu->window = window;
	if (screenshotPath != NULL && *screenshotPath != '\0') {
		SDL_snprintf(gpu->screenshotPath, sizeof(gpu->screenshotPath), "%s",
			     screenshotPath);
		gpu->screenshotPending = true;
	}

	gpu->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, NULL);
	if (gpu->device == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_CreateGPUDevice (SPIR-V/Vulkan) failed: %s",
			     SDL_GetError());
		gpuBackendDestroy(gpu);
		return NULL;
	}
	if (!SDL_ClaimWindowForGPUDevice(gpu->device, window)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_ClaimWindowForGPUDevice failed: %s",
			     SDL_GetError());
		gpuBackendDestroy(gpu);
		return NULL;
	}
	gpu->claimed = true;
	colorFormat = SDL_GetGPUSwapchainTextureFormat(gpu->device, window);
	gpu->colorFormat = colorFormat;
	SDL_Log("gpu_backend: swapchain format enum %d", (int)colorFormat);

	if (!createPipeline(gpu, shaderDir, colorFormat) ||
	    !createVertexBuffers(gpu, 64) ||
	    !createAtlas(gpu, texturesDir) ||
	    !createSampler(gpu)) {
		gpuBackendDestroy(gpu);
		return NULL;
	}
	/* The grid is optional: a failed grid pipeline or buffer leaves it
	 * disabled (gpuBackendDrawGrid draws nothing) but the app runs. */
	if (!createGridPipeline(gpu, shaderDir, colorFormat) ||
	    !createGridBuffers(gpu))
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: grid disabled (init failed); continuing");
	SDL_Log("gpu_backend: ready (driver %s)", SDL_GetGPUDeviceDriver(gpu->device));
	return gpu;
}

void gpuBackendDestroy(GpuBackend *gpu)
{
	if (gpu == NULL)
		return;
	if (gpu->device != NULL) {
		if (gpu->captureTransfer != NULL)
			SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->captureTransfer);
		if (gpu->captureTexture != NULL)
			SDL_ReleaseGPUTexture(gpu->device, gpu->captureTexture);
		if (gpu->sampler != NULL)
			SDL_ReleaseGPUSampler(gpu->device, gpu->sampler);
		if (gpu->texture != NULL)
			SDL_ReleaseGPUTexture(gpu->device, gpu->texture);
		if (gpu->uiVertexBuffer != NULL)
			SDL_ReleaseGPUBuffer(gpu->device, gpu->uiVertexBuffer);
		if (gpu->uiVertexTransfer != NULL)
			SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->uiVertexTransfer);
		if (gpu->vertexBuffer != NULL)
			SDL_ReleaseGPUBuffer(gpu->device, gpu->vertexBuffer);
		if (gpu->vertexTransfer != NULL)
			SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->vertexTransfer);
		if (gpu->gridVertexBuffer != NULL)
			SDL_ReleaseGPUBuffer(gpu->device, gpu->gridVertexBuffer);
		if (gpu->gridVertexTransfer != NULL)
			SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->gridVertexTransfer);
		if (gpu->pipeline != NULL)
			SDL_ReleaseGPUGraphicsPipeline(gpu->device, gpu->pipeline);
		if (gpu->gridPipeline != NULL)
			SDL_ReleaseGPUGraphicsPipeline(gpu->device, gpu->gridPipeline);
		if (gpu->claimed)
			SDL_ReleaseWindowFromGPUDevice(gpu->device, gpu->window);
		SDL_DestroyGPUDevice(gpu->device);
	}
	free(gpu->uiVerts);
	free(gpu->uiDraws);
	free(gpu);
}

SDL_GPUDevice *gpuBackendDevice(GpuBackend *gpu)
{
	return gpu != NULL ? gpu->device : NULL;
}

const MaterialTable *gpuBackendMaterials(GpuBackend *gpu)
{
	return gpu != NULL ? &gpu->materials : NULL;
}

SDL_GPUCommandBuffer *gpuBackendFrameCommandBuffer(GpuBackend *gpu)
{
	if (gpu == NULL || !gpu->frameActive)
		return NULL;
	return gpu->cmd;
}

/* Expand one draw item into two triangles (corners 0-1-2 and 0-2-3), copying
 * world position, atlas UV and unpacked RGBA tint. Returns the new vertex
 * count. `out` must have room for 6 * list->count vertices. */
static Uint32 buildVertices(const DrawList *list, GpuVertex *out)
{
	static const int corner[GPU_VERTICES_PER_ITEM] = { 0, 1, 2, 0, 2, 3 };
	Uint32 n = 0;
	size_t i;
	int k;

	for (i = 0; i < list->count; i++) {
		const DrawItem *item = &list->items[i];
		float r = (float)((item->tint >> 24) & 0xffu) / 255.0f;
		float g = (float)((item->tint >> 16) & 0xffu) / 255.0f;
		float b = (float)((item->tint >> 8) & 0xffu) / 255.0f;
		float a = (float)(item->tint & 0xffu) / 255.0f;

		for (k = 0; k < GPU_VERTICES_PER_ITEM; k++) {
			int c = corner[k];
			GpuVertex *v = &out[n++];

			v->x = item->worldQuad[c][0];
			v->y = item->worldQuad[c][1];
			v->z = item->worldQuad[c][2];
			v->u = item->uv[c][0];
			v->v = item->uv[c][1];
			v->r = r;
			v->g = g;
			v->b = b;
			v->a = a;
			v->alphaMode = item->alphaMode == ALPHA_CUTOUT ? 1.0f
								       : 0.0f;
		}
	}
	return n;
}

/* Upload `bytes` from a staging transfer buffer into a GPU buffer. */
static bool uploadBuffer(SDL_GPUCommandBuffer *cmd,
			 SDL_GPUBuffer *dst, SDL_GPUTransferBuffer *src,
			 size_t bytes)
{
	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

	if (copy == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex upload copy pass failed: %s",
			     SDL_GetError());
		return false;
	}
	SDL_UploadToGPUBuffer(copy,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = src, .offset = 0 },
		&(SDL_GPUBufferRegion){ .buffer = dst, .offset = 0,
					.size = (Uint32)bytes },
		true);
	SDL_EndGPUCopyPass(copy);
	return true;
}

/* Bind pipeline, viewport, one vertex buffer and a sampler, push a
 * view-projection and issue one draw. */
static void drawBatch(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd,
		      SDL_GPURenderPass *pass, SDL_GPUBuffer *vb,
		      SDL_GPUTexture *tex, const Mat4 *vp, Uint32 first,
		      Uint32 count, Uint32 width, Uint32 height)
{
	SDL_GPUViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.w = (float)width, .h = (float)height,
		.min_depth = 0.0f, .max_depth = 1.0f,
	};
	SDL_GPUBufferBinding binding = { .buffer = vb, .offset = 0 };
	SDL_GPUTextureSamplerBinding sampler = { .texture = tex,
						 .sampler = gpu->sampler };

	if (count == 0 || vb == NULL)
		return;
	SDL_BindGPUGraphicsPipeline(pass, gpu->pipeline);
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	SDL_PushGPUVertexUniformData(cmd, 0, vp->m, sizeof(vp->m));
	SDL_DrawGPUPrimitives(pass, count, 1, first, 0);
}

/* Bind the grid pipeline, push its vertex view-projection and fragment fade
 * block, and issue the one grid draw. No sampler: the grid fragment samples
 * nothing (its uniforms live at set 3, pushed with the fragment-uniform
 * call). */
static void drawGrid(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd,
		     SDL_GPURenderPass *pass, Uint32 width, Uint32 height)
{
	SDL_GPUViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.w = (float)width, .h = (float)height,
		.min_depth = 0.0f, .max_depth = 1.0f,
	};
	SDL_GPUBufferBinding binding = { .buffer = gpu->gridVertexBuffer,
					 .offset = 0 };
	float ubo[4];

	if (gpu->gridPipeline == NULL || gpu->gridVertexBuffer == NULL)
		return;
	SDL_BindGPUGraphicsPipeline(pass, gpu->gridPipeline);
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	SDL_PushGPUVertexUniformData(cmd, 0, gpu->gridViewProj.m,
				     sizeof(gpu->gridViewProj.m));
	ubo[0] = gpu->gridCenterX;
	ubo[1] = gpu->gridCenterZ;
	ubo[2] = gpu->gridFadeStart;
	ubo[3] = gpu->gridFadeEnd;
	SDL_PushGPUFragmentUniformData(cmd, 0, ubo, sizeof(ubo));
	SDL_DrawGPUPrimitives(pass, GPU_GRID_VERTICES, 1, 0, 0);
}

/* Replay the frame's world draw then every UI quad into `target` (a render
 * pass with the given clear colour). */
static void recordScene(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd,
			SDL_GPUTexture *target, Uint32 width, Uint32 height)
{
	SDL_GPUColorTargetInfo info = { 0 };
	SDL_GPURenderPass *pass;
	Mat4 screen;
	size_t i;

	info.texture = target;
	info.load_op = SDL_GPU_LOADOP_CLEAR;
	info.store_op = SDL_GPU_STOREOP_STORE;
	info.clear_color = (SDL_FColor){ 0.10f, 0.10f, 0.14f, 1.0f };
	pass = SDL_BeginGPURenderPass(cmd, &info, 1, NULL);
	if (pass == NULL)
		return;

	if (gpu->gridPending)
		drawGrid(gpu, cmd, pass, width, height);

	if (gpu->worldPending && gpu->worldVertexCount > 0)
		drawBatch(gpu, cmd, pass, gpu->vertexBuffer, gpu->texture,
			  &gpu->worldViewProj, 0, (Uint32)gpu->worldVertexCount,
			  width, height);

	/* Screen-space ortho: (0,0) top-left, y down, physical pixels. */
	screen = mat4Ortho(0.0f, (float)width, (float)height, 0.0f, 0.0f, 1.0f);
	for (i = 0; i < gpu->uiDrawCount; i++) {
		const GpuUiDraw *d = &gpu->uiDraws[i];

		drawBatch(gpu, cmd, pass, gpu->uiVertexBuffer, d->texture, &screen,
			  (Uint32)d->first, (Uint32)d->count, width, height);
	}
	SDL_EndGPURenderPass(pass);
}

/* (Re)create the offscreen capture texture sized to the current swapchain. */
static bool ensureCaptureTarget(GpuBackend *gpu, Uint32 width, Uint32 height)
{
	if (gpu->captureTexture != NULL && gpu->captureWidth == width &&
	    gpu->captureHeight == height)
		return true;
	if (gpu->captureTransfer != NULL)
		SDL_ReleaseGPUTransferBuffer(gpu->device, gpu->captureTransfer);
	if (gpu->captureTexture != NULL)
		SDL_ReleaseGPUTexture(gpu->device, gpu->captureTexture);
	gpu->captureTransfer = NULL;
	gpu->captureTexture = NULL;

	gpu->captureTexture = SDL_CreateGPUTexture(gpu->device,
		&(SDL_GPUTextureCreateInfo){
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = gpu->colorFormat,
			.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = width,
			.height = height,
			.layer_count_or_depth = 1,
			.num_levels = 1,
			.sample_count = SDL_GPU_SAMPLECOUNT_1,
		});
	gpu->captureTransfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
			.size = width * height * 4,
		});
	if (gpu->captureTexture == NULL || gpu->captureTransfer == NULL)
		return false;
	gpu->captureWidth = width;
	gpu->captureHeight = height;
	return true;
}

/* Replay the frame into the offscreen texture and download it. */
static bool recordCapture(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd)
{
	SDL_GPUCopyPass *copy;

	if (gpu->swapW == 0 || gpu->swapH == 0)
		return false;
	if (!ensureCaptureTarget(gpu, gpu->swapW, gpu->swapH))
		return false;

	recordScene(gpu, cmd, gpu->captureTexture, gpu->captureWidth,
		    gpu->captureHeight);

	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL)
		return false;
	SDL_DownloadFromGPUTexture(copy,
		&(SDL_GPUTextureRegion){ .texture = gpu->captureTexture,
					 .w = gpu->captureWidth,
					 .h = gpu->captureHeight, .d = 1 },
		&(SDL_GPUTextureTransferInfo){ .transfer_buffer = gpu->captureTransfer,
					       .offset = 0,
					       .pixels_per_row = gpu->captureWidth,
					       .rows_per_layer = gpu->captureHeight });
	SDL_EndGPUCopyPass(copy);
	return true;
}

/* Map the downloaded pixels and write the PNG. Call after the fence. */
static void finishCapture(GpuBackend *gpu)
{
	void *pixels = SDL_MapGPUTransferBuffer(gpu->device, gpu->captureTransfer, false);
	SDL_Surface *surface;
	SDL_PixelFormat format;

	if (pixels == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: screenshot map failed: %s", SDL_GetError());
		return;
	}
	/* Match the downloaded byte order to the capture texture's format. */
	format = (gpu->colorFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
		  gpu->colorFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
			 ? SDL_PIXELFORMAT_BGRA32
			 : SDL_PIXELFORMAT_RGBA32;
	surface = SDL_CreateSurfaceFrom((int)gpu->captureWidth,
					(int)gpu->captureHeight, format, pixels,
					(int)(gpu->captureWidth * 4));
	if (surface == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: screenshot surface failed: %s", SDL_GetError());
	} else {
		if (!SDL_SavePNG(surface, gpu->screenshotPath)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: SDL_SavePNG failed for '%s': %s",
				     gpu->screenshotPath, SDL_GetError());
		} else {
			SDL_Log("gpu_backend: screenshot written to %s (%ux%u)",
				gpu->screenshotPath, gpu->captureWidth,
				gpu->captureHeight);
		}
		SDL_DestroySurface(surface);
	}
	SDL_UnmapGPUTransferBuffer(gpu->device, gpu->captureTransfer);
}

bool gpuBackendBeginFrame(GpuBackend *gpu)
{
	if (gpu == NULL || gpu->device == NULL)
		return false;
	if (gpu->frameActive)
		return false;

	gpu->worldPending = false;
	gpu->worldVertexCount = 0;
	gpu->gridPending = false;
	gpu->uiVertCount = 0;
	gpu->uiDrawCount = 0;
	gpu->swapchain = NULL;
	gpu->swapW = 0;
	gpu->swapH = 0;

	gpu->cmd = SDL_AcquireGPUCommandBuffer(gpu->device);
	if (gpu->cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_AcquireGPUCommandBuffer failed: %s",
			     SDL_GetError());
		return false;
	}
	if (!SDL_AcquireGPUSwapchainTexture(gpu->cmd, gpu->window, &gpu->swapchain,
					    &gpu->swapW, &gpu->swapH)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: swapchain acquire failed: %s", SDL_GetError());
		SDL_SubmitGPUCommandBuffer(gpu->cmd);
		gpu->cmd = NULL;
		return false;
	}
	gpu->frameActive = true;
	return true;
}

bool gpuBackendDrawList(GpuBackend *gpu, const Mat4 *viewProj,
			const DrawList *list)
{
	if (gpu == NULL || !gpu->frameActive || viewProj == NULL || list == NULL)
		return false;
	if (!ensureVertexCapacity(gpu, list->capacity))
		return false;

	gpu->worldVertexCount = 0;
	if (list->count > 0) {
		void *mapped = SDL_MapGPUTransferBuffer(gpu->device,
							gpu->vertexTransfer, false);

		if (mapped == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: vertex staging map failed: %s",
				     SDL_GetError());
			return false;
		}
		gpu->worldVertexCount = buildVertices(list, mapped);
		SDL_UnmapGPUTransferBuffer(gpu->device, gpu->vertexTransfer);
	}
	gpu->worldViewProj = *viewProj;
	gpu->worldPending = true;
	return true;
}

bool gpuBackendDrawGrid(GpuBackend *gpu, const Mat4 *viewProj,
			const GridQuad *quad)
{
	static const int corner[GPU_GRID_VERTICES] = { 0, 1, 2, 0, 2, 3 };
	GpuVertex *mapped;
	float r = (float)((GPU_GRID_TINT >> 24) & 0xffu) / 255.0f;
	float g = (float)((GPU_GRID_TINT >> 16) & 0xffu) / 255.0f;
	float b = (float)((GPU_GRID_TINT >> 8) & 0xffu) / 255.0f;
	float a = (float)(GPU_GRID_TINT & 0xffu) / 255.0f;
	int k;

	if (gpu == NULL || !gpu->frameActive || viewProj == NULL || quad == NULL)
		return false;
	/* Tolerant: a backend whose grid init failed draws nothing. */
	if (gpu->gridPipeline == NULL || gpu->gridVertexBuffer == NULL)
		return true;

	mapped = SDL_MapGPUTransferBuffer(gpu->device, gpu->gridVertexTransfer,
					  false);
	if (mapped == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: grid vertex staging map failed: %s",
			     SDL_GetError());
		return false;
	}
	for (k = 0; k < GPU_GRID_VERTICES; k++) {
		int c = corner[k];
		GpuVertex *v = &mapped[k];

		v->x = quad->corners[c][0];
		v->y = quad->corners[c][1];
		v->z = quad->corners[c][2];
		v->u = quad->uv[c][0];
		v->v = quad->uv[c][1];
		v->r = r;
		v->g = g;
		v->b = b;
		v->a = a;
		v->alphaMode = 0.0f;
	}
	SDL_UnmapGPUTransferBuffer(gpu->device, gpu->gridVertexTransfer);

	gpu->gridViewProj = *viewProj;
	gpu->gridCenterX = quad->centerX;
	gpu->gridCenterZ = quad->centerZ;
	gpu->gridFadeStart = quad->fadeStart;
	gpu->gridFadeEnd = quad->fadeEnd;
	gpu->gridPending = true;
	return true;
}

/* Append a UI draw descriptor, merging into the previous one when it shares
 * the texture and is contiguous. */
static bool addUiDraw(GpuBackend *gpu, SDL_GPUTexture *texture, size_t first,
		      size_t count)
{
	if (gpu->uiDrawCount > 0) {
		GpuUiDraw *last = &gpu->uiDraws[gpu->uiDrawCount - 1];

		if (last->texture == texture && last->first + last->count == first) {
			last->count += count;
			return true;
		}
	}
	if (gpu->uiDrawCount >= gpu->uiDrawCapacity) {
		size_t cap = gpu->uiDrawCapacity == 0 ? GPU_UI_DRAW_INITIAL
						      : gpu->uiDrawCapacity * 2;
		GpuUiDraw *grown = realloc(gpu->uiDraws, cap * sizeof(*grown));

		if (grown == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: ui draw batch grow failed");
			return false;
		}
		gpu->uiDraws = grown;
		gpu->uiDrawCapacity = cap;
	}
	gpu->uiDraws[gpu->uiDrawCount++] =
		(GpuUiDraw){ texture, first, count };
	return true;
}

bool gpuBackendDrawUiQuad(GpuBackend *gpu, SDL_GPUTexture *texture,
			  float x, float y, float w, float h,
			  const float uv[4][2], uint32_t rgba)
{
	static const int corner[GPU_UI_QUAD_VERTICES] = { 0, 1, 2, 0, 2, 3 };
	const float cx[4] = { x, x + w, x + w, x };
	const float cy[4] = { y, y, y + h, y + h };
	float r = (float)((rgba >> 24) & 0xffu) / 255.0f;
	float g = (float)((rgba >> 16) & 0xffu) / 255.0f;
	float b = (float)((rgba >> 8) & 0xffu) / 255.0f;
	float a = (float)(rgba & 0xffu) / 255.0f;
	size_t first;
	int k;

	if (gpu == NULL || !gpu->frameActive || texture == NULL || uv == NULL)
		return false;
	if (!ensureUiVertexCapacity(gpu, gpu->uiVertCount + GPU_UI_QUAD_VERTICES))
		return false;

	first = gpu->uiVertCount;
	for (k = 0; k < GPU_UI_QUAD_VERTICES; k++) {
		int c = corner[k];
		GpuVertex *v = &gpu->uiVerts[gpu->uiVertCount++];

		v->x = cx[c];
		v->y = cy[c];
		v->z = 0.0f;
		v->u = uv[c][0];
		v->v = uv[c][1];
		v->r = r;
		v->g = g;
		v->b = b;
		v->a = a;
		v->alphaMode = 0.0f;
	}
	return addUiDraw(gpu, texture, first, GPU_UI_QUAD_VERTICES);
}

bool gpuBackendEndFrame(GpuBackend *gpu)
{
	SDL_GPUCommandBuffer *cmd;
	bool capturing;

	if (gpu == NULL || !gpu->frameActive)
		return false;
	cmd = gpu->cmd;

	if (gpu->worldVertexCount > 0 &&
	    !uploadBuffer(cmd, gpu->vertexBuffer,
			  gpu->vertexTransfer,
			  gpu->worldVertexCount * sizeof(GpuVertex))) {
		SDL_CancelGPUCommandBuffer(cmd);
		gpu->frameActive = false;
		gpu->cmd = NULL;
		return false;
	}

	if (gpu->gridPending &&
	    !uploadBuffer(cmd, gpu->gridVertexBuffer, gpu->gridVertexTransfer,
			  GPU_GRID_VERTICES * sizeof(GpuVertex))) {
		SDL_CancelGPUCommandBuffer(cmd);
		gpu->frameActive = false;
		gpu->cmd = NULL;
		return false;
	}

	if (gpu->uiVertCount > 0) {
		void *mapped;

		if (!ensureUiVertexCapacity(gpu, gpu->uiVertCount)) {
			SDL_CancelGPUCommandBuffer(cmd);
			gpu->frameActive = false;
			gpu->cmd = NULL;
			return false;
		}
		mapped = SDL_MapGPUTransferBuffer(gpu->device, gpu->uiVertexTransfer,
						  false);
		if (mapped == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: ui vertex staging map failed: %s",
				     SDL_GetError());
			SDL_CancelGPUCommandBuffer(cmd);
			gpu->frameActive = false;
			gpu->cmd = NULL;
			return false;
		}
		memcpy(mapped, gpu->uiVerts, gpu->uiVertCount * sizeof(GpuVertex));
		SDL_UnmapGPUTransferBuffer(gpu->device, gpu->uiVertexTransfer);
		if (!uploadBuffer(cmd, gpu->uiVertexBuffer,
				  gpu->uiVertexTransfer,
				  gpu->uiVertCount * sizeof(GpuVertex))) {
			SDL_CancelGPUCommandBuffer(cmd);
			gpu->frameActive = false;
			gpu->cmd = NULL;
			return false;
		}
	}

	if (gpu->swapchain != NULL)
		recordScene(gpu, cmd, gpu->swapchain, gpu->swapW, gpu->swapH);

	capturing = gpu->screenshotPending;
	if (capturing && !recordCapture(gpu, cmd))
		capturing = false;

	if (capturing) {
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);

		if (fence != NULL) {
			SDL_WaitForGPUFences(gpu->device, true, &fence, 1);
			SDL_ReleaseGPUFence(gpu->device, fence);
			finishCapture(gpu);
		} else {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: capture submit failed: %s",
				     SDL_GetError());
		}
		gpu->screenshotPending = false;
	} else {
		SDL_SubmitGPUCommandBuffer(cmd);
	}

	gpu->frameActive = false;
	gpu->cmd = NULL;
	gpu->swapchain = NULL;
	return true;
}
