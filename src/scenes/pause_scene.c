/*
 * Pause scene (see pause_scene.h). A dim overlay with a centred pane and a
 * Resume / Quit to Menu menu. Resume (or CMD_BACK) pops to the level; Quit to
 * Menu replaces the stack with a fresh menu.
 */

#include "scenes/pause_scene.h"

#include "app.h"
#include "input/input.h"
#include "scenes/menu_scene.h"
#include "scenes/ui_bridge.h"
#include "ui/element_label.h"
#include "ui/element_menu.h"
#include "ui/element_overlay.h"
#include "ui/layout.h"
#include "ui/ui_font.h"
#include "ui/ui_scale.h"

#include <SDL3/SDL.h>

#define PAUSE_PANE_W 360
#define PAUSE_PANE_H 220
#define PAUSE_ROW_HEIGHT 34
#define PAUSE_ITEM_COUNT 2

typedef struct PauseState {
	Element *root;		/* overlay */
	Element *pane;
} PauseState;

static TextStyle pauseStyle(const App *app)
{
	UiFont *font = appFont(app);
	TextStyle style;

	style.font = font;
	style.measure = uiFontMeasure(font);
	style.pixelSize = APP_UI_FONT_PIXELS;
	return style;
}

static void pauseOnResume(void *ctx)
{
	popScene(appSceneStack((App *)ctx));
}

static void pauseOnQuitToMenu(void *ctx)
{
	App *app = ctx;
	Scene *menu = menuSceneCreate();

	if (menu == NULL)
		return;
	if (!replaceScene(appSceneStack(app), menu)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "pause_scene: scene stack rejected menu");
		destroyScene(menu);
	}
}

static bool pause_init(void *self, App *app)
{
	PauseState *st = scenePayload(self);
	TextStyle style = pauseStyle(app);
	Element *label;
	Element *menu;

	st->root = uiCreateOverlay();
	st->pane = uiCreatePane(UI_AXIS_VERTICAL, 24, 16);
	label = uiCreateLabel("PAUSED", &style);
	menu = uiCreateMenu(&style);
	if (st->root == NULL || st->pane == NULL || label == NULL || menu == NULL ||
	    uiMenuAddItem(menu, "Resume", pauseOnResume, app) == NULL ||
	    uiMenuAddItem(menu, "Quit to Menu", pauseOnQuitToMenu, app) == NULL) {
		uiDestroyElement(label);
		uiDestroyElement(menu);
		uiDestroyElement(st->pane);
		uiDestroyElement(st->root);
		st->root = NULL;
		st->pane = NULL;
		return false;
	}
	uiSetRect(label, 0, 0, 0, 40);
	uiSetRect(menu, 0, 0, 0, PAUSE_ROW_HEIGHT * PAUSE_ITEM_COUNT);
	uiAppendChild(st->pane, label);
	uiAppendChild(st->pane, menu);
	uiAppendChild(st->root, st->pane);
	return true;
}

static void pause_update(void *self, App *app, float dt)
{
	PauseState *st = scenePayload(self);
	const InputFrame *frame = appInputFrame(app);
	bool consumed;

	(void)dt;
	/* Gate the BACK fallback on the bridge: a frame like [SELECT, BACK]
	 * activates Resume (queueing one pop) and must not also queue a second
	 * pop from the raw command (which, after the menu's replaceScene, would
	 * pop an empty stack). A BACK-only frame falls through and pops once. */
	consumed = uiBridgeDispatch(st->root, frame);
	if (!consumed && uiBridgeFrameHasBack(frame))
		popScene(appSceneStack(app));
}

static void pause_draw(void *self, App *app)
{
	PauseState *st = scenePayload(self);
	float scale = appUiScale(app);
	int vw = uiScalePhysicalToVirtual(appPixelWidth(app), scale);
	int vh = uiScalePhysicalToVirtual(appPixelHeight(app), scale);

	uiSetRect(st->root, 0, 0, vw, vh);
	uiSetRect(st->pane, (vw - PAUSE_PANE_W) / 2, (vh - PAUSE_PANE_H) / 2,
		  PAUSE_PANE_W, PAUSE_PANE_H);
	uiLayout(st->root);
	uiDraw(st->root, appUiDrawCtx(app));
}

static void pause_unload(void *self, App *app)
{
	PauseState *st = scenePayload(self);

	(void)app;
	uiDestroyElement(st->root);
	st->root = NULL;
	st->pane = NULL;
}

static const SceneVt pauseVt = {
	pause_init, pause_update, pause_draw, pause_unload,
	sizeof(PauseState), "pause",
};

Scene *pauseSceneCreate(void)
{
	return createScene(&pauseVt);
}
