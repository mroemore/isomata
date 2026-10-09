/* isomata/src/app.h — the application shell.
 *
 * Invariants (dotzy-style header contract):
 *   - App owns the SDL window and drives the frame loop; it is the only
 *     owner of SDL lifecycle init/quit for the process.
 *   - appCreate() must run before appRun()/appDestroy(); no call is
 *     re-entrant and each App is intended one per process.
 *   - appRun() returns when SDL_EVENT_QUIT, a window close event, or the
 *     ISO_SMOKE_MS deadline (smoke-test hook used by tests/) is reached.
 *   - All file-scope state lives behind g_ prefixed names in app.c; the
 *     App type is opaque to callers.
 *   - No Android conditionals here: platform differences go through
 *     src/platform/platform.h only.
 *   - Task 9: the App owns the SceneStack, the SDL_gpu backend and the
 *     UiDrawCtx, and publishes the current InputFrame + uiScale to scenes
 *     through the accessors below. Scenes are SDL-tier (engine_sources) and
 *     may include this header; the pure engine (scene.c) only forward-
 *     declares App.
 *   - Task 10: the App also owns the EventBus (capacity 64), the
 *     AchievementSystem and the Audio player. It dispatches the bus once per
 *     frame after updateSceneStack and before drawing, so an event published
 *     during a scene update is delivered the same frame, and one published
 *     from inside a callback is delivered on the NEXT frame (snapshot
 *     semantics, events.h). audio.c init is tolerant, so a box with no audio
 *     device yields a NULL Audio and a working app.
 */
#ifndef ISOMATA_APP_H
#define ISOMATA_APP_H

#include <stdbool.h>

typedef struct App App;

/* Opaque forward declarations (all named struct tags), so this header pulls
 * in no SDL/pure headers. Scene code includes the concrete headers itself. */
typedef struct InputFrame InputFrame;
typedef struct UiDrawCtx UiDrawCtx;
typedef struct SceneStack SceneStack;
typedef struct GpuBackend GpuBackend;
typedef struct UiFont UiFont;
typedef struct EventBus EventBus;
typedef struct Audio Audio;

/* Design pixel size for UI text (label/menu/button styles). */
#define APP_UI_FONT_PIXELS 30

/* Initialize SDL3 and create the window. Returns NULL on failure
 * (SDL_GetError() carries the reason; SDL_Quit state is cleaned up). */
App *appCreate(const char *title, int width, int height);

/* Run the event loop until clean shutdown is requested. Returns true when
 * the loop exited cleanly. */
bool appRun(App *app);

/* Destroy the window and release SDL. Call once, then discard the pointer. */
void appDestroy(App *app);

/* --- scene-facing accessors -------------------------------------------------
 * Valid during scene init/update/draw/unload. NULL app or an unset field
 * yields NULL / 0 / 1.0 as appropriate. */

/* The frame's command/tap/pan/zoom output (input.h). */
const InputFrame *appInputFrame(const App *app);

/* The UI's physical-pixel scale (ui_scale.h). 1.0 before the loop starts. */
float appUiScale(const App *app);

/* The abstract UI draw context (NULL when no GPU backend is up, e.g. the
 * dummy-driver smoke run). Scenes pass this to uiDraw. */
UiDrawCtx *appUiDrawCtx(App *app);

/* The scene stack scenes push/pop/replace on. */
SceneStack *appSceneStack(App *app);

/* The SDL_gpu backend (NULL when unavailable); the level scene draws its
 * world list through it. */
GpuBackend *appGpuBackend(App *app);

/* The measurement font (ui_font.h); NULL when no font could be loaded. */
UiFont *appFont(const App *app);

/* The engine event bus (events.h) owned by the App. Scenes publish gameplay
 * / UI / audio events and subscribe to topics on it. NULL only if the bus
 * could not be allocated (a degraded but non-fatal state). */
EventBus *appEventBus(App *app);

/* The audio player (audio.h); NULL when no audio device could be opened
 * (audio.c init is tolerant). Scenes pass it to audioPlay or publish
 * EV_AUDIO_PLAY; a NULL player is a no-op. */
Audio *appAudio(App *app);

/* Window pixel size, refreshed each frame. */
int appPixelWidth(const App *app);
int appPixelHeight(const App *app);

/* Safe-area inset of the window in VIRTUAL (logical UI) pixels, refreshed
 * each frame (SDL_GetWindowSafeArea via the platform seam). Scenes lay their
 * UI root in this rect so content clears notches/status/navigation bars;
 * on a desktop display it equals (0, 0, virtualWidth, virtualHeight). Writes
 * 0s before the loop starts or for a NULL app. Any out pointer may be NULL. */
void appSafeArea(const App *app, int *outX, int *outY, int *outW, int *outH);

/* Ask the loop to stop after the current frame (clean exit, code 0). */
void appRequestQuit(App *app);

#endif /* ISOMATA_APP_H */
