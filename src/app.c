#include "app.h"

#include "achievement.h"
#include "audio/audio.h"
#include "events.h"
#include "input/input.h"
#include "input/input_sdl.h"
#include "platform/platform.h"
#include "render/gpu_backend.h"
#include "scenes/menu_scene.h"
#include "scene.h"
#include "ui/ui_font.h"
#include "ui/ui_gpu.h"
#include "ui/ui_scale.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define APP_TITLE "Isomata"
#define APP_FRAME_DELAY_MS 15
#define APP_SCENE_STACK_CAPACITY 8
/* Bounded event queue: comfortably above one frame's publish volume (a
 * handful of gameplay/audio events), matching events.h's bounded-queue
 * contract. */
#define APP_EVENT_CAPACITY 64

struct App {
	SDL_Window *window;
	int width;
	int height;
	int pixelW;
	int pixelH;
	/* Safe-area inset of the window in virtual pixels, refreshed each frame
	 * (see appSafeArea). Equal to the full window on a desktop display. */
	int safeX;
	int safeY;
	int safeW;
	int safeH;
	bool running;

	float uiScale;
	UiFont *font;
	GpuBackend *gpu;
	UiGpu *uiGpu;
	UiDrawCtx *uiCtx;
	SceneStack *stack;
	Input *input;
	InputFrame frame;

	EventBus *bus;
	AchievementSystem *achievements;
	Audio *audio;
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
	UiFont *font = uiLoadFont(resolved, APP_UI_FONT_PIXELS);
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

/* Smoke hook: report the safe-area inset the scenes will lay their UI root
 * in. Exercises the appSafeArea accessor under the dummy-driver smoke run
 * (the real draw path needs a GPU); a no-inset desktop window reports the
 * full client rect. */
static void smokeProbeSafeArea(App *app) {
	int x;
	int y;
	int w;
	int h;

	/* Exercise the accessor's defensive arms (NULL app / NULL out params)
	 * so the dummy-driver smoke run covers them, not only the GPU draw
	 * path; the last call leaves the real values in x/y/w/h for the log. */
	appSafeArea(NULL, &x, &y, &w, &h);
	appSafeArea(app, NULL, NULL, NULL, NULL);
	appSafeArea(app, &x, &y, &w, &h);
	SDL_Log("isomata smoke: safe area %d,%d %dx%d", x, y, w, h);
}

/* Smoke hook: push a synthetic mouse press/motion/release through the SDL
 * event queue so the dummy-driver run drives the mouse branch of
 * inputHandleSdlEvent (including the window->pixel conversion). The window is
 * real (appCreate); under the dummy driver its pixel density is 1.0, so the
 * conversion is identity here. The density != 1 path is reasoned about in the
 * final-fix report and exercised on a real HiDPI display. */
static void smokeProbeInput(App *app) {
	SDL_WindowID id = SDL_GetWindowID(app->window);
	SDL_Event ev;

	if (id == 0)
		return;
	SDL_zero(ev);
	ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	ev.button.windowID = id;
	ev.button.which = 1;		/* a real mouse, not SDL_TOUCH_MOUSEID */
	ev.button.button = SDL_BUTTON_LEFT;
	ev.button.x = 10.0f;
	ev.button.y = 20.0f;
	SDL_PushEvent(&ev);

	SDL_zero(ev);
	ev.type = SDL_EVENT_MOUSE_MOTION;
	ev.motion.windowID = id;
	ev.motion.which = 1;
	ev.motion.state = SDL_BUTTON_LMASK;
	ev.motion.x = 40.0f;
	ev.motion.y = 20.0f;
	SDL_PushEvent(&ev);

	SDL_zero(ev);
	ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
	ev.button.windowID = id;
	ev.button.which = 1;
	ev.button.button = SDL_BUTTON_LEFT;
	ev.button.x = 40.0f;
	ev.button.y = 20.0f;
	SDL_PushEvent(&ev);
}

/* Refresh the window metrics the scenes read: pixel size and the safe-area
 * inset. The physical safe rect from the platform seam is converted to
 * virtual pixels edge-consistently (corners scaled, extents derived) so the
 * UI root shares edges with the renderer's scale. */
static void appRefreshMetrics(App *app) {
	SDL_Rect safe = { 0, 0, 0, 0 };
	int right;
	int bottom;

	SDL_GetWindowSizeInPixels(app->window, &app->pixelW, &app->pixelH);
	platformSafeArea(app->window, &safe.x, &safe.y, &safe.w, &safe.h);
	right = uiScalePhysicalToVirtual(safe.x + safe.w, app->uiScale);
	bottom = uiScalePhysicalToVirtual(safe.y + safe.h, app->uiScale);
	app->safeX = uiScalePhysicalToVirtual(safe.x, app->uiScale);
	app->safeY = uiScalePhysicalToVirtual(safe.y, app->uiScale);
	app->safeW = right - app->safeX;
	app->safeH = bottom - app->safeY;
}

/* Release the runtime (scenes, input, UI, GPU, font). Idempotent and safe on
 * a partially-initialized App; the window/SDL lifecycle stays with
 * appDestroy. uiGpu is released before the backend it borrows the device
 * from. */
static void appReleaseRuntime(App *app) {
	if (app->stack != NULL) {
		destroySceneStack(app->stack);
		app->stack = NULL;
	}
	/* Scenes are gone (their unload unsubscribed); audio and the
	 * achievement system unsubscribe themselves, then the bus is freed. */
	if (app->audio != NULL) {
		audioDestroy(app->audio);
		app->audio = NULL;
	}
	if (app->achievements != NULL) {
		destroyAchievementSystem(app->achievements);
		app->achievements = NULL;
	}
	if (app->bus != NULL) {
		destroyEventBus(app->bus);
		app->bus = NULL;
	}
	if (app->input != NULL) {
		destroyInput(app->input);
		app->input = NULL;
	}
	if (app->uiGpu != NULL) {
		uiGpuDestroy(app->uiGpu);
		app->uiGpu = NULL;
		app->uiCtx = NULL;
	}
	if (app->gpu != NULL) {
		gpuBackendDestroy(app->gpu);
		app->gpu = NULL;
	}
	if (app->font != NULL) {
		uiFreeFont(app->font);
		app->font = NULL;
	}
}

App *appCreate(const char *title, int width, int height) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
		return NULL;
	}
	g_sdlInitialized = true;

	/* ISO_LOG=debug raises the app log category to DEBUG so the opt-in
	 * per-event detail (audio plays, pointer/tap) is emitted. SDL's
	 * default priority is INFO, so DEBUG lines are silent without it. */
	{
		const char *logLevel = SDL_getenv("ISO_LOG");

		if (logLevel != NULL && SDL_strcmp(logLevel, "debug") == 0)
			SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION,
					   SDL_LOG_PRIORITY_DEBUG);
	}

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

	App *app = calloc(1, sizeof(*app));
	if (!app) {
		SDL_DestroyWindow(window);
		SDL_Quit();
		g_sdlInitialized = false;
		return NULL;
	}
	app->window = window;
	app->width = width;
	app->height = height;
	app->pixelW = width;
	app->pixelH = height;
	app->safeX = 0;
	app->safeY = 0;
	app->safeW = width;
	app->safeH = height;
	app->running = true;
	app->uiScale = 1.0f;
	return app;
}

/* Build the runtime: font, GPU backend, UI draw context, scene stack (menu
 * on top) and input. Returns false when a non-smoke run cannot render. */
static bool appSetupRuntime(App *app, bool smoke) {
	char shaderDir[512];
	char texturesDir[512];
	char fontPath[512];
	const char *screenshot = SDL_getenv("ISO_SCREENSHOT");

	/* app->uiScale is resolved in appRun before this runs (the startup log
	 * reports it); the runtime just consumes it. */

	fontPath[0] = '\0';
	if (platformAssetPath("fonts/KiwiSoda.ttf", fontPath, sizeof(fontPath)) != NULL)
		app->font = uiLoadFont(fontPath, APP_UI_FONT_PIXELS);
	if (app->font == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "isomata: UI font unavailable");

	if (platformAssetPath("shaders", shaderDir, sizeof(shaderDir)) != NULL &&
	    platformAssetPath("textures", texturesDir,
			      sizeof(texturesDir)) != NULL) {
		app->gpu = gpuBackendCreate(app->window, shaderDir, texturesDir,
					    screenshot);
	} else {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: render asset paths unresolved");
	}
	if (app->gpu == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: GPU backend unavailable%s",
			     smoke ? " (tolerated in smoke mode)" : "");
		if (!smoke)
			return false;
	} else {
		appRefreshMetrics(app);
		SDL_Log("isomata: window %dx%d pixels (aspect %.3f)", app->pixelW,
			app->pixelH,
			app->pixelH > 0 ? (float)app->pixelW / (float)app->pixelH : 1.0f);
		if (fontPath[0] != '\0')
			app->uiGpu = uiGpuCreate(app->gpu, fontPath, APP_UI_FONT_PIXELS);
		if (app->uiGpu == NULL)
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: UI draw context unavailable (UI will not render)");
		else
			uiGpuSetScale(app->uiGpu, app->uiScale);
		app->uiCtx = uiGpuDrawCtx(app->uiGpu);
	}

	app->stack = createSceneStack(APP_SCENE_STACK_CAPACITY);
	app->input = createInput();
	if (app->stack == NULL || app->input == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: scene stack / input allocation failed");
		return false;
	}

	/* Event bus + achievement system + audio (Task 10). A failed bus or
	 * achievement system is logged and tolerated (events go nowhere); a
	 * NULL Audio is the tolerant no-device case (audio.h). */
	app->bus = createEventBus(APP_EVENT_CAPACITY);
	if (app->bus == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: event bus allocation failed (events disabled)");
	} else {
		app->achievements = createAchievementSystem(app->bus);
		if (app->achievements == NULL)
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "isomata: achievement system unavailable");
	}
	app->audio = audioCreate();
	audioSubscribe(app->audio, app->bus);

	Scene *menu = menuSceneCreate();
	if (menu == NULL || !pushScene(app->stack, menu)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: could not install the startup menu scene");
		destroyScene(menu);
		return false;
	}
	return true;
}

bool appRun(App *app) {
	if (!app) {
		return false;
	}
	const Uint64 startMs = SDL_GetTicks();
	const Uint64 deadline = smokeDeadlineMs();
	const bool smoke = deadline != 0;
	Uint64 lastMs = startMs;

	if (smoke) {
		smokeProbeFont();
		smokeProbeSafeArea(app);
	}

	/* Startup INFO line: SDL version, video driver, the window's physical
	 * pixel size and its logical (point) size, the display density and the
	 * resulting UI scale. Logged for every run, GPU or not. The UI scale is
	 * resolved here (density clamped to [1, 3], then the platform touch
	 * boost composed on top) so the runtime below and the log agree. */
	{
		int sdlVersion = SDL_GetVersion();
		const char *driver = SDL_GetCurrentVideoDriver();
		int windowW = 0;
		int windowH = 0;
		int pointsW = 0;
		int pointsH = 0;
		float density = platformDisplayDensity(app->window);

		app->uiScale = uiScaleFromDensity(density) * platformUiScaleBoost();
		SDL_GetWindowSizeInPixels(app->window, &windowW, &windowH);
		SDL_GetWindowSize(app->window, &pointsW, &pointsH);
		SDL_Log("isomata: SDL %d.%d.%d, video driver %s, window %dx%d pixels (%dx%d points, density %.2f), UI scale %.2f",
			SDL_VERSIONNUM_MAJOR(sdlVersion),
			SDL_VERSIONNUM_MINOR(sdlVersion),
			SDL_VERSIONNUM_MICRO(sdlVersion),
			driver != NULL ? driver : "?", windowW, windowH,
			pointsW, pointsH, density, app->uiScale);
	}

	if (!appSetupRuntime(app, smoke)) {
		appReleaseRuntime(app);
		return false;
	}

	if (smoke)
		smokeProbeInput(app);

	while (app->running) {
		const Uint64 now = SDL_GetTicks();
		const float dt = (float)(now - lastMs) / 1000.0f;
		lastMs = now;

		if (deadline != 0 && now - startMs >= deadline) {
			app->running = false;
			break;
		}

		inputBeginFrame(app->input, app->uiScale);
		{
			SDL_Event event;

			while (SDL_PollEvent(&event)) {
				if (event.type == SDL_EVENT_QUIT ||
				    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
					app->running = false;
				else
					inputHandleSdlEvent(app->input, &event);
			}
		}
		inputEndFrame(app->input, &app->frame);

		appRefreshMetrics(app);

		{
			/* Log each scene change once, with the new scene's name.
			 * The stack applies deferred mutations at the start of
			 * updateSceneStack, so this compares the applied top
			 * before and after. */
			Scene *before = activeScene(app->stack);

			updateSceneStack(app->stack, app, dt);
			{
				Scene *after = activeScene(app->stack);

				if (after != before) {
					const char *name = after != NULL
								   ? sceneName(after)
								   : NULL;

					SDL_Log("isomata: scene -> %s",
						name != NULL ? name : "(none)");
				}
			}
		}

		/* Deliver this frame's queued events. An event published during
		 * a scene update is delivered here (same frame); one published
		 * from inside a callback is delivered next frame (snapshot
		 * semantics, events.h), so the achievement toast/audio land one
		 * frame after the 4th turn. NULL bus is a harmless no-op. */
		dispatchEvents(app->bus);

		if (app->gpu != NULL && gpuBackendBeginFrame(app->gpu)) {
			drawSceneStackAll(app->stack, app);
			gpuBackendEndFrame(app->gpu);
		}
		SDL_Delay(APP_FRAME_DELAY_MS);
	}

	appReleaseRuntime(app);
	return true;
}

void appDestroy(App *app) {
	if (!app) {
		return;
	}
	appReleaseRuntime(app);
	if (app->window) {
		SDL_DestroyWindow(app->window);
	}
	free(app);
	if (g_sdlInitialized) {
		SDL_Quit();
		g_sdlInitialized = false;
	}
}

/* --- scene-facing accessors ------------------------------------------------- */

const InputFrame *appInputFrame(const App *app) {
	return app != NULL ? &app->frame : NULL;
}

float appUiScale(const App *app) {
	return app != NULL ? app->uiScale : 1.0f;
}

UiDrawCtx *appUiDrawCtx(App *app) {
	return app != NULL ? app->uiCtx : NULL;
}

SceneStack *appSceneStack(App *app) {
	return app != NULL ? app->stack : NULL;
}

GpuBackend *appGpuBackend(App *app) {
	return app != NULL ? app->gpu : NULL;
}

UiFont *appFont(const App *app) {
	return app != NULL ? app->font : NULL;
}

EventBus *appEventBus(App *app) {
	return app != NULL ? app->bus : NULL;
}

Audio *appAudio(App *app) {
	return app != NULL ? app->audio : NULL;
}

int appPixelWidth(const App *app) {
	return app != NULL ? app->pixelW : 0;
}

int appPixelHeight(const App *app) {
	return app != NULL ? app->pixelH : 0;
}

void appSafeArea(const App *app, int *outX, int *outY, int *outW, int *outH) {
	if (app == NULL) {
		if (outX) {
			*outX = 0;
		}
		if (outY) {
			*outY = 0;
		}
		if (outW) {
			*outW = 0;
		}
		if (outH) {
			*outH = 0;
		}
		return;
	}
	if (outX) {
		*outX = app->safeX;
	}
	if (outY) {
		*outY = app->safeY;
	}
	if (outW) {
		*outW = app->safeW;
	}
	if (outH) {
		*outH = app->safeH;
	}
}

void appRequestQuit(App *app) {
	if (app != NULL)
		app->running = false;
}
