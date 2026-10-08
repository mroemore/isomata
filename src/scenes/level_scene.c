/*
 * Level scene (see level_scene.h). SDL-tier: loads the demo map, owns a
 * Camera3D + DrawList + billboards and the achievement toast (a small UI
 * root), and per frame builds/draws the world list, then handles the world
 * commands from the InputFrame.
 *
 * Task 10 event wiring:
 * - Every successful camera quarter turn publishes EV_GAMEPLAY_CAMERA_TURNED
 *   (topic EV_TOPIC_GAMEPLAY) and an EV_AUDIO_PLAY request for the rotate
 *   sound.
 * - The scene subscribes to EV_TOPIC_ACHIEVEMENT; an EV_ACHIEVEMENT_UNLOCKED
 *   payload shows the toast with the achievement title. The subscription is
 *   removed in unload.
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
#include "render/math3d.h"
#include "render/sprites.h"
#include "render/voxmap.h"
#include "scenes/pause_scene.h"
#include "scenes/ui_bridge.h"
#include "ui/element.h"
#include "ui/layout.h"
#include "ui/toast.h"
#include "ui/ui_font.h"

#include <SDL3/SDL.h>

#define LEVEL_DRAWLIST_CAPACITY 4096
#define LEVEL_SPRITE_COUNT 3
#define LEVEL_PAN_PER_PIXEL 0.05f	/* world units per virtual drag pixel */

#define LEVEL_TOAST_W 300
#define LEVEL_TOAST_H 48
#define LEVEL_TOAST_TOP 20		/* virtual px from the top edge */

typedef struct LevelState {
	Voxmap *map;
	Camera3D camera;
	DrawList list;
	SpriteEntity sprites[LEVEL_SPRITE_COUNT];
	size_t spriteCount;

	EventBus *bus;		/* borrowed from the App; may be NULL */
	int quarterTurns;	/* running count of applied quarter turns */
	Element *root;		/* UI root (a transparent container for the toast) */
	Element *toast;
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
}

static bool level_init(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	TextStyle style = levelStyle(app);
	char mapPath[512];

	initCamera3D(&st->camera);
	/* Centre the 16x16 map: at yaw 0, pan x moves +X and pan y moves -Z. */
	cameraPan(&st->camera, 8.0f, -8.0f);
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

	/* Toast UI. ui/ has no non-drawing container, so a zero-padding pane
	 * (sized to the toast each frame in levelDrawUi, and not drawn at all
	 * while the toast is hidden) is the root the toast hangs off. */
	st->root = uiCreatePane(UI_AXIS_VERTICAL, 0, 0);
	st->toast = uiCreateToast(&style);
	if (st->root == NULL || st->toast == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: toast UI allocation failed");
		uiDestroyElement(st->toast);
		uiDestroyElement(st->root);
		st->root = NULL;
		st->toast = NULL;
	} else {
		uiAppendChild(st->root, st->toast);
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

static void levelPublishTurn(LevelState *st, int direction)
{
	GameplayCameraTurn turn;
	AudioPlayRequest play = { SOUND_ROTATE };

	st->quarterTurns++;
	turn.direction = direction;
	turn.quarter = st->quarterTurns;
	/* NULL bus is tolerated by publishEvent (returns false). */
	publishEvent(st->bus, EV_TOPIC_GAMEPLAY, EV_GAMEPLAY_CAMERA_TURNED,
		     &turn, sizeof(turn));
	publishEvent(st->bus, EV_TOPIC_AUDIO, EV_AUDIO_PLAY, &play,
		     sizeof(play));
}

static void levelHandleCommand(LevelState *st, App *app, Command cmd)
{
	switch (cmd) {
	case CMD_ROTATE_CW:
		cameraRotateQuarterTurn(&st->camera, 1);
		levelPublishTurn(st, 1);
		break;
	case CMD_ROTATE_CCW:
		cameraRotateQuarterTurn(&st->camera, -1);
		levelPublishTurn(st, -1);
		break;
	case CMD_ZOOM_IN:
		cameraZoom(&st->camera, 1.0f);
		break;
	case CMD_ZOOM_OUT:
		cameraZoom(&st->camera, -1.0f);
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

	(void)uiBridgeDispatch(st->root, frame);

	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++)
		levelHandleCommand(st, app, frame->commands[i]);

	/* Content follows the pointer: drag right/down moves the view with it. */
	if (frame->panDx != 0 || frame->panDy != 0)
		cameraPan(&st->camera, -(float)frame->panDx * LEVEL_PAN_PER_PIXEL,
			  -(float)frame->panDy * LEVEL_PAN_PER_PIXEL);

	updateCamera3D(&st->camera, dt);
	toastUpdate(st->toast, dt);
}

/* Draw the toast UI on top of the world, centred near the top edge. Nothing
 * is drawn while the toast is hidden (so the root pane never paints a
 * persistent backdrop). */
static void levelDrawUi(LevelState *st, App *app)
{
	UiDrawCtx *ctx = appUiDrawCtx(app);
	int sx;
	int sy;
	int sw;
	int sh;
	int x;
	int y;

	if (st->root == NULL || !toastVisible(st->toast))
		return;
	/* Keep the toast inside the window's safe area so it clears notches /
	 * status bars (on a desktop display the safe area is the full window). */
	appSafeArea(app, &sx, &sy, &sw, &sh);
	x = sx + (sw - LEVEL_TOAST_W) / 2;
	y = sy + LEVEL_TOAST_TOP;
	uiSetRect(st->root, x, y, LEVEL_TOAST_W, LEVEL_TOAST_H);
	uiSetRect(st->toast, x, y, LEVEL_TOAST_W, LEVEL_TOAST_H);
	uiLayout(st->root);
	uiDraw(st->root, ctx);
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
		Mat4 view = cameraView(&st->camera);
		Mat4 viewProj = mat4Multiply(&projection, &view);

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
	uiDestroyElement(st->root);
	st->root = NULL;
	st->toast = NULL;
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
