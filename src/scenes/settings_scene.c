/*
 * Settings scene (see settings_scene.h). A dim overlay with a small centred
 * pane. The Back item (or CMD_BACK) pops the scene; the frozen menu beneath
 * shows through the dim backdrop because the app draws the whole stack.
 */

#include "scenes/settings_scene.h"

#include "app.h"
#include "input/input.h"
#include "scenes/ui_bridge.h"
#include "ui/element_label.h"
#include "ui/element_menu.h"
#include "ui/element_overlay.h"
#include "ui/layout.h"
#include "ui/ui_font.h"
#include "ui/ui_scale.h"

#define SETTINGS_PANE_W 360
#define SETTINGS_PANE_H 220
#define SETTINGS_ROW_HEIGHT 34

typedef struct SettingsState {
	Element *root;		/* overlay */
	Element *pane;
} SettingsState;

static TextStyle settingsStyle(const App *app)
{
	UiFont *font = appFont(app);
	TextStyle style;

	style.font = font;
	style.measure = uiFontMeasure(font);
	style.pixelSize = APP_UI_FONT_PIXELS;
	return style;
}

static void settingsOnBack(void *ctx)
{
	App *app = ctx;

	popScene(appSceneStack(app));
}

static bool settings_init(void *self, App *app)
{
	SettingsState *st = scenePayload(self);
	TextStyle style = settingsStyle(app);
	Element *label;
	Element *menu;

	st->root = uiCreateOverlay();
	st->pane = uiCreatePane(UI_AXIS_VERTICAL, 24, 16);
	label = uiCreateLabel("SETTINGS (placeholder)", &style);
	menu = uiCreateMenu(&style);
	if (st->root == NULL || st->pane == NULL || label == NULL || menu == NULL ||
	    uiMenuAddItem(menu, "Back", settingsOnBack, app) == NULL) {
		uiDestroyElement(label);
		uiDestroyElement(menu);
		uiDestroyElement(st->pane);
		uiDestroyElement(st->root);
		st->root = NULL;
		st->pane = NULL;
		return false;
	}
	uiSetRect(label, 0, 0, 0, 40);
	uiSetRect(menu, 0, 0, 0, SETTINGS_ROW_HEIGHT);
	uiAppendChild(st->pane, label);
	uiAppendChild(st->pane, menu);
	uiAppendChild(st->root, st->pane);
	return true;
}

static void settings_update(void *self, App *app, float dt)
{
	SettingsState *st = scenePayload(self);
	const InputFrame *frame = appInputFrame(app);
	bool consumed;

	(void)dt;
	/* Gate the BACK fallback on the bridge: a frame like [SELECT, BACK]
	 * activates the Back item (queueing one pop) and must not also queue a
	 * second pop from the raw command. The menu never consumes UI_CANCEL,
	 * so a BACK-only frame falls through here and pops exactly once. */
	consumed = uiBridgeDispatch(st->root, frame);
	if (!consumed && uiBridgeFrameHasBack(frame))
		popScene(appSceneStack(app));
}

static void settings_draw(void *self, App *app)
{
	SettingsState *st = scenePayload(self);
	float scale = appUiScale(app);
	int vw = uiScalePhysicalToVirtual(appPixelWidth(app), scale);
	int vh = uiScalePhysicalToVirtual(appPixelHeight(app), scale);

	uiSetRect(st->root, 0, 0, vw, vh);
	uiSetRect(st->pane, (vw - SETTINGS_PANE_W) / 2, (vh - SETTINGS_PANE_H) / 2,
		  SETTINGS_PANE_W, SETTINGS_PANE_H);
	uiLayout(st->root);
	uiDraw(st->root, appUiDrawCtx(app));
}

static void settings_unload(void *self, App *app)
{
	SettingsState *st = scenePayload(self);

	(void)app;
	uiDestroyElement(st->root);
	st->root = NULL;
	st->pane = NULL;
}

static const SceneVt settingsVt = {
	settings_init, settings_update, settings_draw, settings_unload,
	sizeof(SettingsState), "settings",
};

Scene *settingsSceneCreate(void)
{
	return createScene(&settingsVt);
}
