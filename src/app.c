#include "app.h"

#include "platform/platform.h"
#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/gpu_backend.h"
#include "render/math3d.h"
#include "render/sprites.h"
#include "render/voxmap.h"
#include "ui/ui_font.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define APP_TITLE "Isomata"
#define APP_FRAME_DELAY_MS 15
#define APP_DRAWLIST_CAPACITY 4096
#define APP_SPRITE_COUNT 3

struct App {
	SDL_Window *window;
	int width;
	int height;
	bool running;
};

static bool g_sdlInitialized = false;

static Uint64 smokeDeadlineMs(void) {
	const char *env = SDL_getenv("ISO_SMOKE_MS");
	if (!env || *env == '\0') {
		return 0;
	}
	const char *end = NULL;
	const unsigned long long value = strtoull(env, (char **)&end, 10);
	if (end == env || value == 0) {
		return 0;
	}
	return value;
}

/* Read an integer environment variable (ISO_CAMERA_TURNS verification hook).
 * A missing or unparsable value yields the fallback. */
static int envInt(const char *name, int fallback) {
	const char *env = SDL_getenv(name);
	char *end = NULL;
	long value;

	if (!env || *env == '\0') {
		return fallback;
	}
	value = strtol(env, &end, 10);
	if (end == env) {
		return fallback;
	}
	return (int)value;
}

/* Smoke hook: when ISO_SMOKE_MS drives the loop, prove the SDL text tier
 * loads the bundled font and measures a string (no renderer, no GPU
 * device). One log line carries the result; a missing asset is logged,
 * not fatal, so the smoke test still exercises the rest of the loop. */
static void smokeProbeFont(void) {
	char path[512];
	const char *resolved = platformAssetPath("fonts/KiwiSoda.ttf", path, sizeof(path));

	if (!resolved) {
		SDL_Log("isomata smoke: font path unresolved");
		return;
	}
	UiFont *font = uiLoadFont(resolved, 32);
	if (!font) {
		SDL_Log("isomata smoke: font load failed: %s", SDL_GetError());
		return;
	}
	TextMeasure measure = uiFontMeasure(font);
	int w = 0;
	int h = 0;
	/* uiFontMeasure guarantees fn != NULL for a non-NULL font. */
	int rc = measure.fn(measure.ctx, "Isomata", &w, &h);
	SDL_Log("isomata smoke: font measure \"Isomata\" = %dx%d (height %d, rc %d)",
		w, h, uiFontHeight(font), rc);
	uiFreeFont(font);
}

/* Minimal temporary camera controls for the Task 8 verification pass: arrow
 * keys rotate a quarter turn, +/- zoom. Task 9 replaces this with the input
 * layer. */
static void appHandleEvents(App *app, Camera3D *camera) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT) {
			app->running = false;
		} else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			app->running = false;
		} else if (event.type == SDL_EVENT_KEY_DOWN) {
			switch (event.key.key) {
			case SDLK_LEFT:
				cameraRotateQuarterTurn(camera, -1);
				break;
			case SDLK_RIGHT:
				cameraRotateQuarterTurn(camera, 1);
				break;
			case SDLK_PLUS:
			case SDLK_EQUALS:
				cameraZoom(camera, 1.0f);
				break;
			case SDLK_MINUS:
				cameraZoom(camera, -1.0f);
				break;
			default:
				break;
			}
		}
	}
}

/* The demo's three billboards: two on the height-2 plateau and one on the
 * tower top, so their painter order is visible against the terrain. */
static void buildDemoSprites(SpriteEntity sprites[APP_SPRITE_COUNT]) {
	sprites[0] = (SpriteEntity){ 6.5f, 2.0f, 9.5f, 1.2f, 1.8f,
				     DRAW_TINT(255, 255, 255, 255) };
	sprites[1] = (SpriteEntity){ 11.5f, 2.0f, 10.5f, 1.2f, 1.8f,
				     DRAW_TINT(255, 255, 255, 255) };
	sprites[2] = (SpriteEntity){ 7.0f, 9.0f, 8.0f, 1.5f, 2.2f,
				     DRAW_TINT(255, 255, 255, 255) };
}

App *appCreate(const char *title, int width, int height) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
		return NULL;
	}
	g_sdlInitialized = true;

	SDL_Window *window = SDL_CreateWindow(
		title && *title ? title : APP_TITLE,
		width,
		height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!window) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
		SDL_Quit();
		g_sdlInitialized = false;
		return NULL;
	}

	App *app = malloc(sizeof(*app));
	if (!app) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		g_sdlInitialized = false;
		return NULL;
	}
	app->window = window;
	app->width = width;
	app->height = height;
	app->running = true;
	return app;
}

bool appRun(App *app) {
	if (!app) {
		return false;
	}
	const Uint64 startMs = SDL_GetTicks();
	const Uint64 deadline = smokeDeadlineMs();
	const bool smoke = deadline != 0;
	if (smoke) {
		smokeProbeFont();
	}

	/* Task 8: build the demo voxmap and sprites, then each frame fill, sort
	 * and draw the list. The smoke path runs under SDL_VIDEODRIVER=dummy,
	 * where claiming a swapchain fails by design, so a GPU-init failure is
	 * tolerated there and still exits 0; a normal run fails hard. */
	char shaderDir[512];
	char texturePath[512];
	char mapPath[512];
	const char *screenshot = SDL_getenv("ISO_SCREENSHOT");
	Camera3D camera;
	GpuBackend *gpu = NULL;
	Voxmap *map = NULL;
	DrawList list;
	SpriteEntity sprites[APP_SPRITE_COUNT];
	int turns;
	int i;
	Uint64 lastMs = startMs;

	initCamera3D(&camera);
	/* Centre the 16x16 map: at yaw 0, pan x moves +X and pan y moves -Z. */
	cameraPan(&camera, 8.0f, -8.0f);
	/* Verification hook: scripted initial yaw in quarter turns. */
	turns = envInt("ISO_CAMERA_TURNS", 0);
	for (i = 0; i < turns; i++) {
		cameraRotateQuarterTurn(&camera, 1);
		updateCamera3D(&camera, CAMERA_TURN_SECONDS);
	}
	for (i = 0; i > turns; i--) {
		cameraRotateQuarterTurn(&camera, -1);
		updateCamera3D(&camera, CAMERA_TURN_SECONDS);
	}

	initDrawList(&list, APP_DRAWLIST_CAPACITY);
	buildDemoSprites(sprites);

	if (platformAssetPath("maps/demo.txt", mapPath, sizeof(mapPath)) != NULL)
		map = loadVoxmap(mapPath);
	if (map == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: demo map unavailable");

	if (platformAssetPath("shaders", shaderDir, sizeof(shaderDir)) != NULL &&
	    platformAssetPath("textures/placeholder.png", texturePath,
			      sizeof(texturePath)) != NULL) {
		gpu = gpuBackendCreate(app->window, shaderDir, texturePath,
				       screenshot);
	} else {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: render asset paths unresolved");
	}
	if (gpu == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: GPU backend unavailable%s",
			     smoke ? " (tolerated in smoke mode)" : "");
		if (!smoke) {
			destroyDrawList(&list);
			destroyVoxmap(map);
			return false;
		}
	} else {
		int pixelW = 0;
		int pixelH = 0;

		SDL_GetWindowSizeInPixels(app->window, &pixelW, &pixelH);
		SDL_Log("isomata: window %dx%d pixels (aspect %.3f)", pixelW,
			pixelH, pixelH > 0 ? (float)pixelW / (float)pixelH : 1.0f);
	}

	while (app->running) {
		const Uint64 now = SDL_GetTicks();
		const float dt = (float)(now - lastMs) / 1000.0f;
		lastMs = now;

		if (deadline != 0 && now - startMs >= deadline) {
			app->running = false;
			break;
		}
		appHandleEvents(app, &camera);
		updateCamera3D(&camera, dt);
		if (gpu != NULL) {
			int pixelW = 0;
			int pixelH = 0;
			SDL_GetWindowSizeInPixels(app->window, &pixelW, &pixelH);
			const float aspect = pixelH > 0 ? (float)pixelW / (float)pixelH : 1.0f;
			const Mat4 projection = cameraProjection(&camera, aspect);
			const Mat4 view = cameraView(&camera);
			const Mat4 viewProj = mat4Multiply(&projection, &view);

			clearDrawList(&list);
			if (map != NULL)
				voxmapEmitFaces(map, &list, &camera,
						DRAW_TINT(255, 255, 255, 255));
			for (i = 0; i < APP_SPRITE_COUNT; i++)
				appendSprite(&list, &sprites[i], &camera);
			sortDrawList(&list, &camera);
			gpuBackendDrawList(gpu, &viewProj, &list);
		}
		SDL_Delay(APP_FRAME_DELAY_MS);
	}
	destroyDrawList(&list);
	destroyVoxmap(map);
	gpuBackendDestroy(gpu);
	return true;
}

void appDestroy(App *app) {
	if (!app) {
		return;
	}
	if (app->window) {
		SDL_DestroyWindow(app->window);
	}
	free(app);
	if (g_sdlInitialized) {
		SDL_Quit();
		g_sdlInitialized = false;
	}
}
