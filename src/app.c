#include "app.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define APP_TITLE "Isomata"
#define APP_FRAME_DELAY_MS 15

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

static void appHandleEvents(App *app) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_QUIT) {
			app->running = false;
		} else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			app->running = false;
		}
	}
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
	while (app->running) {
		if (deadline != 0 && SDL_GetTicks() - startMs >= deadline) {
			app->running = false;
			break;
		}
		appHandleEvents(app);
		SDL_Delay(APP_FRAME_DELAY_MS);
	}
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
