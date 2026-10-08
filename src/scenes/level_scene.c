/*
 * Level scene (see level_scene.h). SDL-tier: loads the demo map, owns a
 * Camera3D + DrawList + billboards, and per frame builds and draws the world
 * list, then handles the world commands from the InputFrame.
 */

#include "scenes/level_scene.h"

#include "app.h"
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

#include <SDL3/SDL.h>

#define LEVEL_DRAWLIST_CAPACITY 4096
#define LEVEL_SPRITE_COUNT 3
#define LEVEL_PAN_PER_PIXEL 0.05f	/* world units per virtual drag pixel */

typedef struct LevelState {
	Voxmap *map;
	Camera3D camera;
	DrawList list;
	SpriteEntity sprites[LEVEL_SPRITE_COUNT];
	size_t spriteCount;
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

static bool level_init(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	char mapPath[512];

	(void)app;
	initCamera3D(&st->camera);
	/* Centre the 16x16 map: at yaw 0, pan x moves +X and pan y moves -Z. */
	cameraPan(&st->camera, 8.0f, -8.0f);
	initDrawList(&st->list, LEVEL_DRAWLIST_CAPACITY);
	levelBuildSprites(st);

	if (platformAssetPath("maps/demo.txt", mapPath, sizeof(mapPath)) != NULL)
		st->map = loadVoxmap(mapPath);
	if (st->map == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "level_scene: demo map unavailable");
	return true;
}

static void levelHandleCommand(LevelState *st, App *app, Command cmd)
{
	switch (cmd) {
	case CMD_ROTATE_CW:
		cameraRotateQuarterTurn(&st->camera, 1);
		break;
	case CMD_ROTATE_CCW:
		cameraRotateQuarterTurn(&st->camera, -1);
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

	/* No UI yet: always false, but keeps the routing order uniform. */
	uiBridgeDispatch(NULL, frame);

	for (i = 0; i < frame->commandCount && i < INPUT_MAX_COMMANDS; i++)
		levelHandleCommand(st, app, frame->commands[i]);

	/* Content follows the pointer: drag right/down moves the view with it. */
	if (frame->panDx != 0 || frame->panDy != 0)
		cameraPan(&st->camera, -(float)frame->panDx * LEVEL_PAN_PER_PIXEL,
			  -(float)frame->panDy * LEVEL_PAN_PER_PIXEL);

	updateCamera3D(&st->camera, dt);
}

static void level_draw(void *self, App *app)
{
	LevelState *st = scenePayload(self);
	GpuBackend *gpu = appGpuBackend(app);
	int pw = appPixelWidth(app);
	int ph = appPixelHeight(app);
	float aspect;
	Mat4 projection;
	Mat4 view;
	Mat4 viewProj;

	if (gpu == NULL)
		return;

	aspect = ph > 0 ? (float)pw / (float)ph : 1.0f;
	projection = cameraProjection(&st->camera, aspect);
	view = cameraView(&st->camera);
	viewProj = mat4Multiply(&projection, &view);

	buildFrameDrawList(st->map, st->sprites, st->spriteCount, &st->camera,
			   &st->list);
	gpuBackendDrawList(gpu, &viewProj, &st->list);
}

static void level_unload(void *self, App *app)
{
	LevelState *st = scenePayload(self);

	(void)app;
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
