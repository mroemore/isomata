/*
 * SDL_gpu UiDrawCtx (see ui_gpu.h). Owns a dedicated white 1x1 texture for
 * solid fills and a bounded FIFO cache of SDL3_ttf-rendered text textures,
 * and submits screen-space quads through the render backend's quad path.
 */

#include "ui/ui_gpu.h"

#include "platform/platform.h"
#include "render/gpu_backend.h"
#include "ui/ui_scale.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct UiTextEntry {
	bool used;
	char text[UI_TEXT_MAX];
	int pixelSize;		/* physical size this entry was rendered at */
	SDL_GPUTexture *texture;
	int w;
	int h;
} UiTextEntry;

struct UiGpu {
	UiDrawCtx ctx;		/* must be first: callbacks cast ctx back */
	GpuBackend *gpu;
	SDL_GPUDevice *device;
	TTF_Font *ttf;
	int basePixelSize;
	float scale;
	SDL_GPUTexture *white;
	SDL_GPUTexture *icons[UI_ICON_COUNT];	/* NULL = missing/unloadable */
	UiTextEntry cache[UI_GPU_TEXT_CACHE];
	int nextSlot;		/* FIFO eviction cursor */
};

/* The asset file backing each icon (white on transparent, 64x64). NULL for
 * UI_ICON_COUNT / out-of-range. */
static const char *uiGpuIconAssetName(UiIcon icon)
{
	switch (icon) {
	case UI_ICON_ROTATE_CCW:
		return "icons/rotate-2.png";
	case UI_ICON_ROTATE_CW:
		return "icons/rotate-clockwise-2.png";
	case UI_ICON_RESTORE:
		return "icons/restore.png";
	case UI_ICON_BULB:
		return "icons/bulb.png";
	case UI_ICON_BULB_OFF:
		return "icons/bulb-off.png";
	default:
		return NULL;
	}
}

/* Upload an RGBA32 pixel buffer as a nearest-sampled GPU texture. Uses the
 * open frame's command buffer when there is one (the copy pass runs before
 * the render pass), else a private command buffer. */
static SDL_GPUTexture *uiGpuUploadPixels(UiGpu *ui, int w, int h, int pitch,
					 const void *pixels)
{
	SDL_GPUTexture *tex;
	SDL_GPUTransferBuffer *transfer;
	SDL_GPUCommandBuffer *cmd;
	SDL_GPUCopyPass *copy;
	bool ownCmd = false;
	void *mapped;

	tex = SDL_CreateGPUTexture(ui->device,
		&(SDL_GPUTextureCreateInfo){
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = (Uint32)w,
			.height = (Uint32)h,
			.layer_count_or_depth = 1,
			.num_levels = 1,
			.sample_count = SDL_GPU_SAMPLECOUNT_1,
		});
	if (tex == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: texture creation failed: %s", SDL_GetError());
		return NULL;
	}
	transfer = SDL_CreateGPUTransferBuffer(ui->device,
		&(SDL_GPUTransferBufferCreateInfo){
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = (Uint32)(pitch * h),
		});
	if (transfer == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: texture transfer buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTexture(ui->device, tex);
		return NULL;
	}
	mapped = SDL_MapGPUTransferBuffer(ui->device, transfer, false);
	if (mapped == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: texture transfer map failed: %s", SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(ui->device, transfer);
		SDL_ReleaseGPUTexture(ui->device, tex);
		return NULL;
	}
	memcpy(mapped, pixels, (size_t)pitch * (size_t)h);
	SDL_UnmapGPUTransferBuffer(ui->device, transfer);

	cmd = gpuBackendFrameCommandBuffer(ui->gpu);
	if (cmd == NULL) {
		cmd = SDL_AcquireGPUCommandBuffer(ui->device);
		ownCmd = true;
	}
	if (cmd == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: texture upload command buffer failed: %s",
			     SDL_GetError());
		SDL_ReleaseGPUTransferBuffer(ui->device, transfer);
		SDL_ReleaseGPUTexture(ui->device, tex);
		return NULL;
	}
	copy = SDL_BeginGPUCopyPass(cmd);
	if (copy == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: texture upload copy pass failed: %s",
			     SDL_GetError());
		if (ownCmd)
			SDL_CancelGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(ui->device, transfer);
		SDL_ReleaseGPUTexture(ui->device, tex);
		return NULL;
	}
	SDL_UploadToGPUTexture(copy,
		&(SDL_GPUTextureTransferInfo){ .transfer_buffer = transfer, .offset = 0,
					       .pixels_per_row = (Uint32)w,
					       .rows_per_layer = (Uint32)h },
		&(SDL_GPUTextureRegion){ .texture = tex, .w = (Uint32)w,
					 .h = (Uint32)h, .d = 1 },
		false);
	SDL_EndGPUCopyPass(copy);
	if (ownCmd)
		SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(ui->device, transfer);
	return tex;
}

/* Load one icon PNG (resolved through platformAssetPath so Android APK
 * assets work) and upload it. Returns NULL and logs ONCE per create when
 * the file is missing/unreadable — the UI tolerates a missing icon (the
 * button simply skips the image); this never fails uiGpuCreate. */
static SDL_GPUTexture *uiGpuLoadIcon(UiGpu *ui, UiIcon icon)
{
	const char *name = uiGpuIconAssetName(icon);
	char path[512];
	SDL_Surface *raw;
	SDL_Surface *rgba;
	SDL_GPUTexture *tex;

	if (name == NULL || platformAssetPath(name, path, sizeof(path)) == NULL)
		return NULL;
	raw = SDL_LoadPNG(path);
	if (raw == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: icon '%s' failed to load: %s", path,
			     SDL_GetError());
		return NULL;
	}
	rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(raw);
	if (rgba == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: icon '%s' convert failed: %s", path,
			     SDL_GetError());
		return NULL;
	}
	tex = uiGpuUploadPixels(ui, rgba->w, rgba->h, (int)rgba->pitch,
				rgba->pixels);
	SDL_DestroySurface(rgba);
	return tex;
}

/* Find or build the text texture for (text, physical pixelSize). FIFO
 * eviction: when the cache is full the oldest slot's texture is released and
 * reused. Returns NULL on render/upload failure. */
static UiTextEntry *uiGpuTextTexture(UiGpu *ui, const char *text, int pixelSize)
{
	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *surface;
	SDL_Surface *rgba;
	SDL_GPUTexture *tex;
	UiTextEntry *entry;
	int i;

	for (i = 0; i < UI_GPU_TEXT_CACHE; i++) {
		UiTextEntry *e = &ui->cache[i];

		if (e->used && e->pixelSize == pixelSize &&
		    strcmp(e->text, text) == 0)
			return e;
	}

	if (!TTF_SetFontSize(ui->ttf, (float)pixelSize)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: TTF_SetFontSize(%d) failed: %s", pixelSize,
			     SDL_GetError());
		return NULL;
	}
	surface = TTF_RenderText_Blended(ui->ttf, text, strlen(text), white);
	if (surface == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: TTF_RenderText_Blended failed for '%s': %s",
			     text, SDL_GetError());
		return NULL;
	}
	rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(surface);
	if (rgba == NULL)
		return NULL;
	tex = uiGpuUploadPixels(ui, rgba->w, rgba->h, (int)rgba->pitch, rgba->pixels);
	entry = &ui->cache[ui->nextSlot];
	ui->nextSlot = (ui->nextSlot + 1) % UI_GPU_TEXT_CACHE;
	if (tex == NULL) {
		SDL_DestroySurface(rgba);
		return NULL;
	}
	if (entry->used && entry->texture != NULL)
		SDL_ReleaseGPUTexture(ui->device, entry->texture);
	entry->used = true;
	uiCopyText(entry->text, sizeof(entry->text), text);
	entry->pixelSize = pixelSize;
	entry->texture = tex;
	entry->w = rgba->w;
	entry->h = rgba->h;
	SDL_DestroySurface(rgba);
	return entry;
}

static void uiGpuFillRect(UiDrawCtx *ctx, int x, int y, int w, int h,
			  uint32_t rgba)
{
	static const float uv[4][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
	};
	UiGpu *ui = (UiGpu *)ctx;
	int px = x;
	int py = y;
	int pw = w;
	int ph = h;

	if (ui == NULL)
		return;
	uiScaleRect(&px, &py, &pw, &ph, ui->scale);
	gpuBackendDrawUiQuad(ui->gpu, ui->white, (float)px, (float)py,
			     (float)pw, (float)ph, uv, rgba);
}

static void uiGpuDrawText(UiDrawCtx *ctx, int x, int y, const char *text,
			  const struct TextStyle *style, uint32_t rgba)
{
	static const float uv[4][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
	};
	UiGpu *ui = (UiGpu *)ctx;
	UiTextEntry *entry;
	int pixelSize;
	int phys;
	int px;
	int py;

	if (ui == NULL || text == NULL || text[0] == '\0')
		return;
	pixelSize = (style != NULL && style->pixelSize > 0)
			    ? style->pixelSize
			    : ui->basePixelSize;
	phys = (int)lroundf((float)pixelSize * ui->scale);
	if (phys < 1)
		phys = 1;
	entry = uiGpuTextTexture(ui, text, phys);
	if (entry == NULL)
		return;
	px = uiScaleVirtualToPhysical(x, ui->scale);
	py = uiScaleVirtualToPhysical(y, ui->scale);
	gpuBackendDrawUiQuad(ui->gpu, entry->texture, (float)px, (float)py,
			     (float)entry->w, (float)entry->h, uv, rgba);
}

static void uiGpuDrawImage(UiDrawCtx *ctx, int x, int y, int w, int h,
			   UiIcon icon, uint32_t rgba)
{
	static const float uv[4][2] = {
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
	};
	UiGpu *ui = (UiGpu *)ctx;
	int px = x;
	int py = y;
	int pw = w;
	int ph = h;

	if (ui == NULL || (int)icon < 0 || icon >= UI_ICON_COUNT)
		return;
	/* Missing icon: already logged once at create; skip (blank button). */
	if (ui->icons[icon] == NULL)
		return;
	uiScaleRect(&px, &py, &pw, &ph, ui->scale);
	gpuBackendDrawUiQuad(ui->gpu, ui->icons[icon], (float)px, (float)py,
			     (float)pw, (float)ph, uv, rgba);
}

static const UiDrawCtxVt uiGpuVt = {
	uiGpuFillRect,
	uiGpuDrawText,
	uiGpuDrawImage,
};

UiGpu *uiGpuCreate(GpuBackend *gpu, const char *fontPath, int fontPixelSize)
{
	Uint8 whitePixel[4] = { 255, 255, 255, 255 };
	UiGpu *ui;
	int i;

	if (gpu == NULL || fontPath == NULL || fontPath[0] == '\0' ||
	    fontPixelSize <= 0)
		return NULL;
	ui = calloc(1, sizeof(*ui));
	if (ui == NULL)
		return NULL;
	ui->gpu = gpu;
	ui->device = gpuBackendDevice(gpu);
	ui->basePixelSize = fontPixelSize;
	ui->scale = 1.0f;
	ui->ctx.vt = &uiGpuVt;

	if (!TTF_Init()) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: TTF_Init failed: %s", SDL_GetError());
		free(ui);
		return NULL;
	}
	ui->ttf = TTF_OpenFont(fontPath, (float)fontPixelSize);
	if (ui->ttf == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "ui_gpu: TTF_OpenFont('%s') failed: %s", fontPath,
			     SDL_GetError());
		TTF_Quit();
		free(ui);
		return NULL;
	}
	ui->white = uiGpuUploadPixels(ui, 1, 1, 4, whitePixel);
	if (ui->white == NULL) {
		uiGpuDestroy(ui);
		return NULL;
	}
	/* Best-effort icon load: a missing PNG leaves that slot NULL (logged
	 * once above) and the button just skips the image. */
	for (i = 0; i < UI_ICON_COUNT; i++)
		ui->icons[i] = uiGpuLoadIcon(ui, (UiIcon)i);
	return ui;
}

void uiGpuDestroy(UiGpu *ui)
{
	int i;

	if (ui == NULL)
		return;
	for (i = 0; i < UI_GPU_TEXT_CACHE; i++) {
		if (ui->cache[i].used && ui->cache[i].texture != NULL)
			SDL_ReleaseGPUTexture(ui->device, ui->cache[i].texture);
	}
	for (i = 0; i < UI_ICON_COUNT; i++) {
		if (ui->icons[i] != NULL)
			SDL_ReleaseGPUTexture(ui->device, ui->icons[i]);
	}
	if (ui->white != NULL)
		SDL_ReleaseGPUTexture(ui->device, ui->white);
	if (ui->ttf != NULL)
		TTF_CloseFont(ui->ttf);
	TTF_Quit();
	free(ui);
}

UiDrawCtx *uiGpuDrawCtx(UiGpu *ui)
{
	if (ui == NULL)
		return NULL;
	return &ui->ctx;
}

void uiGpuSetScale(UiGpu *ui, float scale)
{
	if (ui == NULL || !isfinite(scale) || scale <= 0.0f)
		return;
	ui->scale = scale;
}
