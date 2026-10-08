/*
 * SDL_gpu render backend (see gpu_backend.h for the contract).
 *
 * Resource graph built at create time and torn down in reverse:
 *   device -> claimed window -> {pipeline, vertex buffer, texture, sampler}
 * Shaders are released as soon as the pipeline exists. The optional
 * offscreen capture path (screenshot verification) owns a color-target
 * texture and a download transfer buffer, released with the backend.
 *
 * The static quad is the Task 7 milestone: a 2x2 world-space square on the
 * ground plane (y = 0), textured and tinted white. Task 8 replaces it with
 * drawlist rendering; the pipeline, bindings and push-uniform plumbing
 * carry over unchanged.
 */

#include "render/gpu_backend.h"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_surface.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPU_QUAD_VERTEX_COUNT 6
#define GPU_CAPTURE_WIDTH 640
#define GPU_CAPTURE_HEIGHT 480
#define GPU_SCREENSHOT_PATH_MAX 512

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
} GpuVertex;

/* Two triangles, counter-clockwise seen from +Y. uv (0,1) at z=-1, so the
 * texture's top row sits on the far edge. */
static const GpuVertex QUAD_VERTICES[GPU_QUAD_VERTEX_COUNT] = {
	{ -1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ 1.0f, 0.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
};

struct GpuBackend {
	SDL_GPUDevice *device;
	SDL_Window *window;
	bool claimed;

	SDL_GPUGraphicsPipeline *pipeline;
	SDL_GPUBuffer *vertexBuffer;
	SDL_GPUTexture *texture;
	SDL_GPUSampler *sampler;
	SDL_GPUTextureFormat colorFormat;

	/* Optional screenshot capture (verification hook). */
	char screenshotPath[GPU_SCREENSHOT_PATH_MAX];
	bool screenshotPending;
	SDL_GPUTexture *captureTexture;
	SDL_GPUTransferBuffer *captureTransfer;

	/* The view-projection pushed to the vertex uniform slot this frame. */
	Mat4 frameViewProj;
};

/* --- small helpers ---------------------------------------------------- */

static Uint8 *readFile(const char *path, size_t *outSize)
{
	FILE *file;
	long length;
	Uint8 *data;
	size_t got;

	*outSize = 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	length = ftell(file);
	if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = malloc((size_t)length);
	if (data == NULL) {
		fclose(file);
		return NULL;
	}
	got = fread(data, 1, (size_t)length, file);
	fclose(file);
	if (got != (size_t)length) {
		free(data);
		return NULL;
	}
	*outSize = (size_t)length;
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
	free(code);
	return shader;
}

/* --- pipeline / geometry / texture ------------------------------------ */

static bool createPipeline(GpuBackend *gpu, const char *shaderDir,
			   SDL_GPUTextureFormat colorFormat)
{
	char path[GPU_SCREENSHOT_PATH_MAX];
	SDL_GPUShader *vs;
	SDL_GPUShader *fs;
	SDL_GPUVertexBufferDescription bufferDesc = { 0 };
	SDL_GPUVertexAttribute attributes[3] = { { 0 } };
	SDL_GPUVertexInputState vertexInput = { 0 };
	SDL_GPUColorTargetDescription colorTarget = { 0 };
	SDL_GPUGraphicsPipelineCreateInfo info = { 0 };

	if (joinPath(path, sizeof(path), shaderDir, "world.vert.spv") == NULL ||
	    (vs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_VERTEX, 1, 0)) == NULL)
		return false;
	if (joinPath(path, sizeof(path), shaderDir, "world.frag.spv") == NULL ||
	    (fs = createShader(gpu->device, path, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1)) == NULL) {
		SDL_ReleaseGPUShader(gpu->device, vs);
		return false;
	}

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
	vertexInput.vertex_buffer_descriptions = &bufferDesc;
	vertexInput.num_vertex_buffers = 1;
	vertexInput.vertex_attributes = attributes;
	vertexInput.num_vertex_attributes = 3;

	/* Alpha blending, no depth (Task 8 painter-sorts), no culling. */
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

	gpu->pipeline = SDL_CreateGPUGraphicsPipeline(gpu->device, &info);
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

static bool createVertexBuffer(GpuBackend *gpu)
{
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUCopyPass *copy;
	void *mapped;

	transfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = sizeof(QUAD_VERTICES),
		});
	if (transfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex transfer buffer failed: %s",
			     SDL_GetError());
		return false;
	}
	mapped = SDL_MapGPUTransferBuffer(gpu->device, transfer, false);
	if (mapped == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex transfer map failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	memcpy(mapped, QUAD_VERTICES, sizeof(QUAD_VERTICES));
	SDL_UnmapGPUTransferBuffer(gpu->device, transfer);

	gpu->vertexBuffer = SDL_CreateGPUBuffer(gpu->device,
		&(SDL_GPUBufferCreateInfo){
			.usage = SDL_GPU_BUFFERUSAGE_VERTEX,
			.size = sizeof(QUAD_VERTICES),
		});
	if (gpu->vertexBuffer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex buffer failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}

	cmd = SDL_AcquireGPUCommandBuffer(gpu->device);
	if (cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex upload command buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: vertex upload copy pass failed: %s",
			     SDL_GetError());
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	SDL_UploadToGPUBuffer(copy,
		&(SDL_GPUTransferBufferLocation){ .transfer_buffer = transfer, .offset = 0 },
		&(SDL_GPUBufferRegion){ .buffer = gpu->vertexBuffer, .offset = 0,
					.size = sizeof(QUAD_VERTICES) },
		false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
	return true;
}

static bool createTexture(GpuBackend *gpu, const char *texturePath)
{
	SDL_Surface *surface;
	SDL_Surface *rgba;
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUCopyPass *copy;
	void *mapped;
	Uint32 texWidth;
	Uint32 texHeight;
	Uint32 pitch;

	surface = SDL_LoadPNG(texturePath);
	if (surface == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_LoadPNG failed for '%s': %s",
			     texturePath, SDL_GetError());
		return false;
	}
	rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(surface);
	if (rgba == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_ConvertSurface failed for '%s': %s",
			     texturePath, SDL_GetError());
		return false;
	}
	texWidth = (Uint32)rgba->w;
	texHeight = (Uint32)rgba->h;
	pitch = (Uint32)rgba->pitch;

	gpu->texture = SDL_CreateGPUTexture(gpu->device,
		&(SDL_GPUTextureCreateInfo){
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = texWidth,
			.height = texHeight,
			.layer_count_or_depth = 1,
			.num_levels = 1,
			.sample_count = SDL_GPU_SAMPLECOUNT_1,
		});
	if (gpu->texture == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: texture creation failed for '%s': %s",
			     texturePath, SDL_GetError());
		SDL_DestroySurface(rgba);
		return false;
	}

	transfer = SDL_CreateGPUTransferBuffer(gpu->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = pitch * texHeight,
		});
	if (transfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: texture transfer buffer failed: %s",
			     SDL_GetError());
		SDL_DestroySurface(rgba);
		return false;
	}
	mapped = SDL_MapGPUTransferBuffer(gpu->device, transfer, false);
	if (mapped == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: texture transfer map failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		SDL_DestroySurface(rgba);
		return false;
	}
	memcpy(mapped, rgba->pixels, pitch * texHeight);
	SDL_UnmapGPUTransferBuffer(gpu->device, transfer);
	SDL_DestroySurface(rgba);

	cmd = SDL_AcquireGPUCommandBuffer(gpu->device);
	if (cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: texture upload command buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: texture upload copy pass failed: %s",
			     SDL_GetError());
		SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
		return false;
	}
	SDL_UploadToGPUTexture(copy,
		&(SDL_GPUTextureTransferInfo){ .transfer_buffer = transfer, .offset = 0,
					       .pixels_per_row = texWidth,
					       .rows_per_layer = texHeight },
		&(SDL_GPUTextureRegion){ .texture = gpu->texture, .w = texWidth,
					 .h = texHeight, .d = 1 },
		false);
	SDL_EndGPUCopyPass(copy);
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu->device, transfer);
	return true;
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
			     const char *texturePath, const char *screenshotPath)
{
	GpuBackend *gpu;
	SDL_GPUTextureFormat colorFormat;
	bool debug = SDL_getenv("ISO_GPU_DEBUG") != NULL;

	if (window == NULL || shaderDir == NULL || texturePath == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: create needs a window, shader dir and texture path");
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
	    !createVertexBuffer(gpu) ||
	    !createTexture(gpu, texturePath) ||
	    !createSampler(gpu)) {
		gpuBackendDestroy(gpu);
		return NULL;
	}
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
		if (gpu->vertexBuffer != NULL)
			SDL_ReleaseGPUBuffer(gpu->device, gpu->vertexBuffer);
		if (gpu->pipeline != NULL)
			SDL_ReleaseGPUGraphicsPipeline(gpu->device, gpu->pipeline);
		if (gpu->claimed)
			SDL_ReleaseWindowFromGPUDevice(gpu->device, gpu->window);
		SDL_DestroyGPUDevice(gpu->device);
	}
	free(gpu);
}

/* Bind pipeline, viewport, vertex buffer and sampler, push the frame's
 * view-projection, then draw the quad into an already-begun render pass.
 * The uniform push happens inside the pass, before the draw (the canonical
 * SDL_gpu ordering). */
static void drawQuad(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd,
		     SDL_GPURenderPass *pass, Uint32 width, Uint32 height)
{
	SDL_GPUViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.w = (float)width, .h = (float)height,
		.min_depth = 0.0f, .max_depth = 1.0f,
	};
	SDL_GPUBufferBinding binding = { .buffer = gpu->vertexBuffer, .offset = 0 };
	SDL_GPUTextureSamplerBinding sampler = { .texture = gpu->texture,
						 .sampler = gpu->sampler };

	SDL_BindGPUGraphicsPipeline(pass, gpu->pipeline);
	SDL_SetGPUViewport(pass, &viewport);
	SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
	SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
	SDL_PushGPUVertexUniformData(cmd, 0, gpu->frameViewProj.m,
				     sizeof(gpu->frameViewProj.m));
	SDL_DrawGPUPrimitives(pass, GPU_QUAD_VERTEX_COUNT, 1, 0, 0);
}

/* Encode the offscreen capture render + download into cmd. */
static bool recordCapture(GpuBackend *gpu, SDL_GPUCommandBuffer *cmd)
{
	SDL_GPUColorTargetInfo target = { 0 };
	SDL_GPURenderPass *pass;
	SDL_GPUCopyPass *copy;

	if (gpu->captureTexture == NULL) {
		gpu->captureTexture = SDL_CreateGPUTexture(gpu->device,
			&(SDL_GPUTextureCreateInfo){
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = gpu->colorFormat,
				.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
				.width = GPU_CAPTURE_WIDTH,
				.height = GPU_CAPTURE_HEIGHT,
				.layer_count_or_depth = 1,
				.num_levels = 1,
				.sample_count = SDL_GPU_SAMPLECOUNT_1,
			});
		gpu->captureTransfer = SDL_CreateGPUTransferBuffer(gpu->device,
			&(SDL_GPUTransferBufferCreateInfo){
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
				.size = GPU_CAPTURE_WIDTH * GPU_CAPTURE_HEIGHT * 4,
			});
	}
	if (gpu->captureTexture == NULL || gpu->captureTransfer == NULL)
		return false;

	target.texture = gpu->captureTexture;
	target.load_op = SDL_GPU_LOADOP_CLEAR;
	target.store_op = SDL_GPU_STOREOP_STORE;
	target.clear_color = (SDL_FColor){ 0.10f, 0.10f, 0.14f, 1.0f };
	pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
	if (pass == NULL)
		return false;
	drawQuad(gpu, cmd, pass, GPU_CAPTURE_WIDTH, GPU_CAPTURE_HEIGHT);
	SDL_EndGPURenderPass(pass);

	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL)
		return false;
	SDL_DownloadFromGPUTexture(copy,
		&(SDL_GPUTextureRegion){ .texture = gpu->captureTexture,
					 .w = GPU_CAPTURE_WIDTH, .h = GPU_CAPTURE_HEIGHT, .d = 1 },
		&(SDL_GPUTextureTransferInfo){ .transfer_buffer = gpu->captureTransfer,
					       .offset = 0,
					       .pixels_per_row = GPU_CAPTURE_WIDTH,
					       .rows_per_layer = GPU_CAPTURE_HEIGHT });
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
	surface = SDL_CreateSurfaceFrom(GPU_CAPTURE_WIDTH, GPU_CAPTURE_HEIGHT,
					format, pixels, GPU_CAPTURE_WIDTH * 4);
	if (surface == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: screenshot surface failed: %s", SDL_GetError());
	} else {
		if (!SDL_SavePNG(surface, gpu->screenshotPath)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "gpu_backend: SDL_SavePNG failed for '%s': %s",
				     gpu->screenshotPath, SDL_GetError());
		} else {
			SDL_Log("gpu_backend: screenshot written to %s (%dx%d)",
				gpu->screenshotPath, GPU_CAPTURE_WIDTH, GPU_CAPTURE_HEIGHT);
		}
		SDL_DestroySurface(surface);
	}
	SDL_UnmapGPUTransferBuffer(gpu->device, gpu->captureTransfer);
}

bool gpuBackendDrawFrame(GpuBackend *gpu, const Mat4 *viewProj)
{
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUTexture *swapchain = NULL;
	Uint32 width = 0;
	Uint32 height = 0;
	bool capturing;

	if (gpu == NULL || gpu->device == NULL || viewProj == NULL)
		return false;

	cmd = SDL_AcquireGPUCommandBuffer(gpu->device);
	if (cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: SDL_AcquireGPUCommandBuffer failed: %s",
			     SDL_GetError());
		return false;
	}
	if (!SDL_AcquireGPUSwapchainTexture(cmd, gpu->window, &swapchain, &width, &height)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "gpu_backend: swapchain acquire failed: %s", SDL_GetError());
		SDL_SubmitGPUCommandBuffer(cmd);
		return false;
	}

	gpu->frameViewProj = *viewProj;

	if (swapchain != NULL) {
		SDL_GPUColorTargetInfo target = { 0 };
		SDL_GPURenderPass *pass;

		target.texture = swapchain;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.clear_color = (SDL_FColor){ 0.10f, 0.10f, 0.14f, 1.0f };
		pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
		if (pass != NULL) {
			drawQuad(gpu, cmd, pass, width, height);
			SDL_EndGPURenderPass(pass);
		}
	}

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
	return true;
}
