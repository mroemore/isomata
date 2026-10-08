/*
 * Start menu scene (see menu_scene.h). Owns one UI tree: a vertical pane
 * holding the title label, the three-item menu and two hint labels (split so
 * each fits a narrow phone viewport). Menu item callbacks receive the App as
 * their ctx and mutate the scene stack (all mutations are deferred by the
 * stack, so this scene survives its own callback).
 */

#include "scenes/menu_scene.h"

#include "app.h"
#include "audio/audio.h"
#include "events.h"
#include "input/input.h"
#include "scenes/level_scene.h"
#include "scenes/settings_scene.h"
#include "scenes/ui_bridge.h"
#include "ui/element_label.h"
#include "ui/element_menu.h"
#include "ui/layout.h"
#include "ui/ui_font.h"

#include <SDL3/SDL.h>

#define MENU_ROW_HEIGHT 34
#define MENU_ITEM_COUNT 3

typedef struct MenuState {
	Element *root;
} MenuState;

static TextStyle menuStyle(const App *app)
{
	UiFont *font = appFont(app);
	TextStyle style;

	style.font = font;
	style.measure = uiFontMeasure(font);
	style.pixelSize = APP_UI_FONT_PIXELS;
	return style;
}

/* Scene-stack mutations from a menu callback: the stack records the op and
 * applies it next frame, so a rejected op must release the scene we made. */
static void menuPushOrDrop(Scene *scene, bool pushed)
{
	if (!pushed) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "menu_scene: scene stack rejected %s",
			     sceneName(scene));
		destroyScene(scene);
	}
}

/* Every menu activation plays the menu blip through the event bus. */
static void menuPlayBlip(App *app)
{
	AudioPlayRequest play = { SOUND_MENU };

	publishEvent(appEventBus(app), EV_TOPIC_AUDIO, EV_AUDIO_PLAY, &play,
		     sizeof(play));
}

static void menuOnStart(void *ctx)
{
	App *app = ctx;
	Scene *level = levelSceneCreate();

	menuPlayBlip(app);
	if (level == NULL)
		return;
	menuPushOrDrop(level, replaceScene(appSceneStack(app), level));
}

static void menuOnSettings(void *ctx)
{
	App *app = ctx;
	Scene *settings = settingsSceneCreate();

	menuPlayBlip(app);
	if (settings == NULL)
		return;
	menuPushOrDrop(settings, pushScene(appSceneStack(app), settings));
}

static void menuOnQuit(void *ctx)
{
	menuPlayBlip((App *)ctx);
	appRequestQuit((App *)ctx);
}

static bool menu_init(void *self, App *app)
{
	MenuState *st = scenePayload(self);
	TextStyle style = menuStyle(app);
	Element *title;
	Element *menu;
	Element *hint1;
	Element *hint2;

	st->root = uiCreatePane(UI_AXIS_VERTICAL, 80, 18);
	if (st->root == NULL)
		return false;
	title = uiCreateLabel("ISOMATA", &style);
	menu = uiCreateMenu(&style);
	/* The hint is split into two short labels so each fits the narrowest
	 * virtual viewport. The pane insets its children by 80 virtual px, so
	 * at the 320x640 / uiScale 1.30 default config only ~166 virtual px
	 * are usable: labels must stay under ~11 characters. */
	hint1 = uiCreateLabel("Up/Down", &style);
	hint2 = uiCreateLabel("Enter", &style);
	if (title == NULL || menu == NULL || hint1 == NULL || hint2 == NULL ||
	    uiMenuAddItem(menu, "Start", menuOnStart, app) == NULL ||
	    uiMenuAddItem(menu, "Settings", menuOnSettings, app) == NULL ||
	    uiMenuAddItem(menu, "Quit", menuOnQuit, app) == NULL) {
		uiDestroyElement(title);
		uiDestroyElement(menu);
		uiDestroyElement(hint1);
		uiDestroyElement(hint2);
		uiDestroyElement(st->root);
		st->root = NULL;
		return false;
	}
	uiSetRect(title, 0, 0, 0, 56);
	uiSetRect(menu, 0, 0, 0, MENU_ROW_HEIGHT * MENU_ITEM_COUNT);
	uiSetRect(hint1, 0, 0, 0, 30);
	uiSetRect(hint2, 0, 0, 0, 30);
	uiAppendChild(st->root, title);
	uiAppendChild(st->root, menu);
	uiAppendChild(st->root, hint1);
	uiAppendChild(st->root, hint2);
	return true;
}

static void menu_update(void *self, App *app, float dt)
{
	MenuState *st = scenePayload(self);

	(void)dt;
	/* The menu consumes nav/select; cancel passes through (ignored here). */
	uiBridgeDispatch(st->root, appInputFrame(app));
}

static void menu_draw(void *self, App *app)
{
	MenuState *st = scenePayload(self);
	UiDrawCtx *ctx = appUiDrawCtx(app);
	int x;
	int y;
	int w;
	int h;

	/* Lay the menu inside the window's safe area (equal to the full window
	 * on a desktop display), so it clears notches / status / navigation
	 * bars on Android. */
	appSafeArea(app, &x, &y, &w, &h);
	uiSetRect(st->root, x, y, w, h);
	uiLayout(st->root);
	uiDraw(st->root, ctx);
}

static void menu_unload(void *self, App *app)
{
	MenuState *st = scenePayload(self);

	(void)app;
	uiDestroyElement(st->root);
	st->root = NULL;
}

static const SceneVt menuVt = {
	menu_init, menu_update, menu_draw, menu_unload,
	sizeof(MenuState), "menu",
};

Scene *menuSceneCreate(void)
{
	return createScene(&menuVt);
}
