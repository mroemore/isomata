/*
 * Level scene (see level_scene.h). SDL-tier: loads the demo map, owns a
 * Camera3D + DrawList + billboards and a small UI root (the achievement
 * toast plus the ROT L / ROT R / RESET controls), and per frame builds/draws
 * the world list, then handles the world commands from the InputFrame.
 *
 * Event wiring:
 * - Every successful camera rotation step publishes EV_GAMEPLAY_CAMERA_TURNED
 *   (topic EV_TOPIC_GAMEPLAY) and an EV_AUDIO_PLAY request for the rotate
 *   sound.
 * - The scene subscribes to EV_TOPIC_ACHIEVEMENT; an EV_ACHIEVEMENT_UNLOCKED
 *   payload shows the toast with the achievement title. The subscription is
 *   removed in unload.
 *
 * The on-screen controls are children of the UI root; a tap routes through
 * uiBridgeDispatch to a button callback, which calls the same helpers as the
 * keyboard command handler (no duplicated rotate/reset logic).
 */

#include "scenes/level_scene.h"

#include "achievement.h"
#include "app.h"
#include "audio/audio.h"
#include "events.h"
#include "input/input.h"
#include "platform/platform.h"
#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/frame.h"
#include "render/gpu_backend.h"
#include "render/grid.h"
#include "render/math3d.h"
#include "render/sprites.h"
#include "render/voxmap.h"
#include "scenes/pause_scene.h"
#include "scenes/ui_bridge.h"
#include "ui/element.h"
#include "ui/element_button.h"
#include "ui/layout.h"
#include "ui/toast.h"
#include "ui/ui_font.h"

#include <SDL3/SDL.h>

#define LEVEL_DRAWLIST_CAPACITY 4096
#define LEVEL_SPRITE_COUNT 3

#define LEVEL_TOAST_W 300
#define LEVEL_TOAST_H 48
#define LEVEL_TOAST_TOP 20		/* virtual px from the top edge */

#define LEVEL_BUTTON_W 96
#define LEVEL_BUTTON_H 48		/* >= 44 virtual px, touch-friendly */
#define LEVEL_BUTTON_MARGIN 16		/* inset from the safe-area edges */
#define LEVEL_BUTTON_GAP 8		/* between the ROT L / ROT R pair */

typedef struct LevelState {
	Voxmap *map;
	Camera3D camera;
	DrawList list;
	SpriteEntity sprites[LEVEL_SPRITE_COUNT];
	size_t spriteCount;

	EventBus *bus;		/* borrowed from the App; may be NULL */
	int rotationSteps;	/* running count of applied 45-degree steps */
	Element *root;		/* UI root (a transparent container) */
	Element *toast;		/* achievement toast */
	Element *rotL;		/* rotate counter-clockwise */
	Element *rotR;		/* rotate clockwise */
	Element *reset;		/* camera reset */
} LevelState;

/* The demo's three billboards (Task 8): two on the plateau, one on the
 * tower top, so painter order is visible against the terrain. */
static void levelBuildSprites(LevelState *st)
{
	st->sprites[0] = (SpriteEntity){ 6.5f, 2.0f, 9.5f, 1.2f, 1.8f,
					 DRAW_TINT(255, 255, 255, 255) };
	st->sprites[1] = (SpriteEntity){ 11.5f, 2.0f, 10.5f, 1.2f, 1.8f,
					 DRAW_TINT(255, 255, 255, 255) };
	st->sprites[2] = (SpriteEntity){ 7.0f, 9.0f, 8.0f, 1.5f, 2.2f,
					 DRAW_TINT(255, 255, 255, 255) };
	st->spriteCount = LEVEL_SPRITE_COUNT;
}

static TextStyle levelStyle(const App *app)
{
	UiFont *font = appFont(app);
	TextStyle style;

	style.font = font;
	style.measure = uiFontMeasure(font);
	style.pixelSize = APP_UI_FONT_PIXELS;
	return style;
}

/* EV_TOPIC_ACHIEVEMENT subscriber: show the unlock title in the toast. */
static void levelOnAchievement(void *ctx, const Event *event)
{
	LevelState *st = ctx;
	const AchievementUnlocked *unlock;

	if (event->topic != EV_TOPIC_ACHIEVEMENT ||
	    event->type != EV_ACHIEVEMENT_UNLOCKED)
		return;
	if (event->payload == NULL ||
	    event->payloadSize < sizeof(AchievementUnlocked))
		return;
	unlock = event->payload;
	toastShow(st->toast, unlock->title);
	SDL_Log("isomata: achievement unlocked: %s",
		unlock->title != NULL ? unlock->title : "(untitled)");
}

static void levelPublishTurn(LevelState *st, int direction)
{
	GameplayCameraTurn turn;
	AudioPlayRequest play = { SOUND_ROTATE };

	st->rotationSteps++;
	turn.direction = direction;
	turn.step = st->rotationSteps;
	/* NULL bus is tolerated by publishEvent (returns false). */
	publishEvent(st->bus, EV_TOPIC_GAMEPLAY, EV_GAMEPLAY_CAMERA_TURNED,
		     &turn, sizeof(turn));
	publishEvent(st->bus, EV_TOPIC_AUDIO, EV_AUDIO_PLAY, &play,
		     sizeof(play));
}

/* Restore the exact startup camera state. Shared by level_init and the
 * CMD_RESET handler (and the RESET button). */
static void levelResetCamera(LevelState *st)
{
	initCamera3D(&st->camera);
	/* Centre the 16x16 map: at yaw 0, pan x moves +X and pan y moves -Z. */
	cameraPan(&st->camera, 8.0f, -8.0f);
	SDL_Log("isomata: level camera reset (yaw %.1f, zoom %.2f)",
		cameraYawDeg(&st->camera), cameraZoomLevel(&st->camera));
}

/* Apply one 45-degree rotate step and publish it. The single path shared by
 * the command handler and the on-screen ROT buttons. */
static void levelRotate(LevelState *st, int direction)
{
	cameraRotateStep(&st->camera, direction);
	SDL_Log("isomata: level rotate %+d -> yaw %.1f", direction,
		cameraYawTargetDeg(&st->camera));
	levelPublishTurn(st, direction);
}

/* Button callbacks: no logic of their own, just the shared helpers. */
static void levelOnRotateCcw(void *ctx)
{
	levelRotate(ctx, -1);
}

static void levelOnRotateCw(void *ctx)
{
	levelRotate(ctx, 1);
}

static void levelOnReset(void *ctx)
{
	levelResetCamera(ctx);
}

static bool level_init(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	TextStyle style = levelStyle(app);
	char mapPath[512];

	levelResetCamera(st);
	initDrawList(&st->list, LEVEL_DRAWLIST_CAPACITY);
	levelBuildSprites(st);

	/* Load the map through SDL I/O so Android APK assets resolve (the pure
	 * parser's file reader cannot see them); parseVoxmapText copies the
	 * cells it needs, so the SDL_LoadFile buffer is freed immediately. */
	if (platformAssetPath("maps/demo.txt", mapPath, sizeof(mapPath)) != NULL) {
		size_t mapSize = 0;
		void *mapText = SDL_LoadFile(mapPath, &mapSize);

		if (mapText != NULL) {
			st->map = parseVoxmapText(mapText, mapSize);
			SDL_free(mapText);
		}
	}
	if (st->map == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: demo map unavailable");

	/* UI root: a zero-padding pane is the layout root the toast and the
	 * controls hang off. The pane itself is never drawn (its draw() paints
	 * an opaque fill); each child is drawn individually (see levelDrawUi). */
	st->root = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	st->toast = uiCreateToast(&style);
	st->rotL = uiCreateButton("ROT L", &style, levelOnRotateCcw, st);
	st->rotR = uiCreateButton("ROT R", &style, levelOnRotateCw, st);
	st->reset = uiCreateButton("RESET", &style, levelOnReset, st);
	if (st->root == NULL || st->toast == NULL || st->rotL == NULL ||
	    st->rotR == NULL || st->reset == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: level UI allocation failed");
		/* Nothing has been appended yet, so each element is freed once
		 * (uiDestroyElement is NULL-safe). */
		uiDestroyElement(st->toast);
		uiDestroyElement(st->rotL);
		uiDestroyElement(st->rotR);
		uiDestroyElement(st->reset);
		uiDestroyElement(st->root);
		st->root = NULL;
		st->toast = NULL;
		st->rotL = NULL;
		st->rotR = NULL;
		st->reset = NULL;
	} else {
		uiAppendChild(st->root, st->toast);
		uiAppendChild(st->root, st->rotL);
		uiAppendChild(st->root, st->rotR);
		uiAppendChild(st->root, st->reset);
	}

	/* Subscribe to achievement unlocks (self-unsubscribes in unload). */
	st->bus = appEventBus(app);
	if (st->bus != NULL) {
		if (!subscribeEvent(st->bus, EV_TOPIC_ACHIEVEMENT,
				    levelOnAchievement, st))
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "level_scene: achievement subscribe failed");
	} else {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: no event bus (toast/audio disabled)");
	}
	return true;
}

static void levelHandleCommand(LevelState *st, App *app, Command cmd)
{
	switch (cmd) {
	case CMD_ROTATE_CW:
		levelRotate(st, 1);
		break;
	case CMD_ROTATE_CCW:
		levelRotate(st, -1);
		break;
	case CMD_ZOOM_IN:
		cameraZoom(&st->camera, 1.0f);
		SDL_Log("isomata: level zoom %.2f", cameraZoomLevel(&st->camera));
		break;
	case CMD_ZOOM_OUT:
		cameraZoom(&st->camera, -1.0f);
		SDL_Log("isomata: level zoom %.2f", cameraZoomLevel(&st->camera));
		break;
	case CMD_RESET:
		levelResetCamera(st);
		break;
	case CMD_BACK:
		{
			Scene *pause = pauseSceneCreate();

			if (pause != NULL && !pushScene(appSceneStack(app), pause)) {
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					     "level_scene: scene stack rejected pause");
				destroyScene(pause);
			}
		}
		break;
	default:
		break;
	}
}

static void level_update(void *self, App *app, float dt)
{
	LevelState *st = scenePayload(self);
	const InputFrame *frame = appInputFrame(app);
	int i;

	/* Pointer-only: the buttons are tap controls with no keyboard focus
	 * model (Q/E/R own rotation/reset), so ACTIVATE must not fire a
	 * button. See uiBridgeDispatchPointer. */
	(void)uiBridgeDispatchPointer(st->root, frame);

	/* Opt-in pointer/tap detail (ISO_LOG=debug). Discrete events only, so
	 * a drag does not spam one line per frame. */
	if (frame->tap)
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: level tap at %d,%d", frame->tapX,
			     frame->tapY);
	if (frame->zoomSteps != 0)
		SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
			     "isomata: level pinch zoom steps %d",
			     frame->zoomSteps);

	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++)
		levelHandleCommand(st, app, frame->commands[i]);

	/* The camera follows the drag: dragging right moves the view right and
	 * dragging down moves the view toward the viewer (cameraPanByDrag). */
	if (frame->panDx != 0 || frame->panDy != 0)
		cameraPanByDrag(&st->camera, frame->panDx, frame->panDy);

	updateCamera3D(&st->camera, dt);
	toastUpdate(st->toast, dt);
}

/* Draw the toast and the three controls on top of the world. The pane root
 * is a layout container only and is NOT drawn: its draw() paints a constant
 * opaque UI_COLOR_BACKGROUND fill, which would sit behind the toast's
 * alpha-scaled fill and make the box snap from full opacity to gone at
 * HIDDEN instead of fading. Each child is drawn individually, so the toast
 * still self-hides via its alpha while the buttons always draw. Child rects
 * are assigned directly (no uiLayout): the pane's single-axis layout would
 * stack all four children instead of placing the corner controls. */
static void levelDrawUi(LevelState *st, App *app)
{
	UiDrawCtx *ctx = appUiDrawCtx(app);
	int sx;
	int sy;
	int sw;
	int sh;
	int rowY;
	int resetX;
	int rotRight;

	if (st->root == NULL)
		return;
	/* Keep the UI inside the window's safe area so it clears notches /
	 * status bars (on a desktop display the safe area is the full window). */
	appSafeArea(app, &sx, &sy, &sw, &sh);

	uiSetRect(st->toast, sx + (sw - LEVEL_TOAST_W) / 2, sy + LEVEL_TOAST_TOP,
		  LEVEL_TOAST_W, LEVEL_TOAST_H);

	rowY = sy + sh - LEVEL_BUTTON_MARGIN - LEVEL_BUTTON_H;
	uiSetRect(st->rotL, sx + LEVEL_BUTTON_MARGIN, rowY, LEVEL_BUTTON_W,
		  LEVEL_BUTTON_H);
	uiSetRect(st->rotR,
		  sx + LEVEL_BUTTON_MARGIN + LEVEL_BUTTON_W + LEVEL_BUTTON_GAP,
		  rowY, LEVEL_BUTTON_W, LEVEL_BUTTON_H);
	/* RESET hugs the safe-area right edge, but at narrow virtual widths
	 * it is pushed to just right of ROT R (rotR.right + gap) so the two
	 * buttons never overlap: resetX = max(safeRight - width, rotR.right +
	 * gap). */
	resetX = sx + sw - LEVEL_BUTTON_MARGIN - LEVEL_BUTTON_W;
	rotRight = sx + LEVEL_BUTTON_MARGIN + LEVEL_BUTTON_W +
		   LEVEL_BUTTON_GAP + LEVEL_BUTTON_W;
	if (resetX < rotRight + LEVEL_BUTTON_GAP)
		resetX = rotRight + LEVEL_BUTTON_GAP;
	uiSetRect(st->reset, resetX, rowY, LEVEL_BUTTON_W, LEVEL_BUTTON_H);

	uiDraw(st->rotL, ctx);
	uiDraw(st->rotR, ctx);
	uiDraw(st->reset, ctx);
	uiDraw(st->toast, ctx);
}

static void level_draw(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	GpuBackend *gpu = appGpuBackend(app);

	if (gpu != NULL) {
		int pw = appPixelWidth(app);
		int ph = appPixelHeight(app);
		float aspect = ph > 0 ? (float)pw / (float)ph : 1.0f;
		Mat4 projection = cameraProjection(&st->camera, aspect);
		Mat4 gridProj = gridProjection(&st->camera, aspect);
		Mat4 view = cameraView(&st->camera);
		Mat4 viewProj = mat4Multiply(&projection, &view);
		Mat4 gridViewProj = mat4Multiply(&gridProj, &view);
		GridQuad grid;

		/* Infinite ground grid beneath the map: drawn first so the
		 * voxel faces (painter-ordered after it) occlude it, and it
		 * shows only on void tiles and beyond the map. It uses a
		 * depth range that encloses the whole quad so the camera's
		 * near plane never cuts a visible edge across the near ground
		 * at low zoom. */
		buildGridQuad(&st->camera, aspect, &grid);
		gpuBackendDrawGrid(gpu, &gridViewProj, &grid);

		buildFrameDrawList(st->map, st->sprites, st->spriteCount,
				   &st->camera, &st->list);
		gpuBackendDrawList(gpu, &viewProj, &st->list);
	}
	levelDrawUi(st, app);
}

static void level_unload(void *self, App *app)
{
	LevelState *st = scenePayload(self);

	(void)app;
	if (st->bus != NULL)
		unsubscribeEvent(st->bus, EV_TOPIC_ACHIEVEMENT,
				 levelOnAchievement, st);
	/* uiDestroyElement frees the whole subtree, so the buttons are freed
	 * here exactly once (never separately); the pointers are just cleared. */
	uiDestroyElement(st->root);
	st->root = NULL;
	st->toast = NULL;
	st->rotL = NULL;
	st->rotR = NULL;
	st->reset = NULL;
	destroyDrawList(&st->list);
	destroyVoxmap(st->map);
	st->map = NULL;
}

static const SceneVt levelVt = {
	level_init, level_update, level_draw, level_unload,
	sizeof(LevelState), "level",
};

Scene *levelSceneCreate(void)
{
	return createScene(&levelVt);
}
