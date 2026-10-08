/*
 * Menu: selectable text rows with wrap-around navigation, activation and
 * tap-to-select. See element_menu.h for the invariant block (bounds,
 * selection rules, row tiling, UI_CANCEL pass-through).
 */

#include "ui/element_menu.h"

typedef struct MenuItem {
	char text[UI_TEXT_MAX];
	UiActionFn onActivate;
	void *ctx;
} MenuItem;

typedef struct MenuState {
	TextStyle style;
	int count;
	int selected;		/* -1 exactly when empty */
	MenuItem items[UI_MENU_MAX_ITEMS];
} MenuState;

/* Fire item i's callback when present. */
static void menuFire(const MenuState *st, int index)
{
	if (st->items[index].onActivate != NULL)
		st->items[index].onActivate(st->items[index].ctx);
}

static void menuDraw(Element *self, UiDrawCtx *ctx)
{
	const MenuState *st = uiElementPayload(self);
	int i;

	if (st->count == 0) {
		ctx->vt->fillRect(ctx, self->x, self->y, self->w, self->h,
				  UI_COLOR_BACKGROUND);
		return;
	}

	for (i = 0; i < st->count; i++) {
		/* Edge-consistent tiling: row i = [y + h*i/count,
		 * y + h*(i+1)/count); adjacent rows never gap. */
		int top = self->y + (int)(((long long)self->h * i) / st->count);
		uint32_t fill = i == st->selected ? UI_COLOR_SELECTION
						  : UI_COLOR_BACKGROUND;

		ctx->vt->fillRect(ctx, self->x, top, self->w,
				  self->y + (int)(((long long)self->h * (i + 1)) / st->count) - top,
				  fill);
		if (st->items[i].text[0] != '\0')
			ctx->vt->drawText(ctx, self->x, top, st->items[i].text,
					  &st->style, UI_COLOR_TEXT);
	}
}

static bool menuHandleIntent(Element *self, UiIntent intent)
{
	MenuState *st = uiElementPayload(self);

	switch (intent) {
	case UI_NAV_UP:
		if (st->count == 0)
			return false;
		st->selected = (st->selected - 1 + st->count) % st->count;
		return true;
	case UI_NAV_DOWN:
		if (st->count == 0)
			return false;
		st->selected = (st->selected + 1) % st->count;
		return true;
	case UI_ACTIVATE:
		if (st->count == 0)
			return false;
		menuFire(st, st->selected);
		return true;
	case UI_CANCEL:
	case UI_NAV_LEFT:
	case UI_NAV_RIGHT:
	default:
		/* Cancel must reach the owner; left/right are not ours. */
		return false;
	}
}

static bool menuHandlePointer(Element *self, int x, int y)
{
	MenuState *st = uiElementPayload(self);
	int row;

	if (st->count == 0 || self->w <= 0 || self->h <= 0)
		return false;
	if (x < self->x || x >= self->x + self->w)
		return false;
	if (y < self->y || y >= self->y + self->h)
		return false;

	row = (int)(((long long)(y - self->y) * st->count) / self->h);
	if (row < 0 || row >= st->count)
		return false;
	st->selected = row;
	menuFire(st, row);
	return true;
}

static const ElementVt menuVt = {
	.layout = NULL,
	.draw = menuDraw,
	.handleIntent = menuHandleIntent,
	.handlePointer = menuHandlePointer,
	.destroy = NULL,
	.size = sizeof(MenuState),
	.name = "menu",
};

Element *uiCreateMenu(const TextStyle *style)
{
	Element *menu = uiCreateElement(&menuVt);
	MenuState *st;

	if (menu == NULL)
		return NULL;
	st = uiElementPayload(menu);
	st->style = style != NULL ? *style : (TextStyle){ 0 };
	st->count = 0;
	st->selected = -1;
	return menu;
}

Element *uiMenuAddItem(Element *menu, const char *text, UiActionFn onActivate,
		       void *ctx)
{
	MenuState *st;
	MenuItem *item;

	if (menu == NULL)
		return NULL;
	st = uiElementPayload(menu);
	if (st->count >= UI_MENU_MAX_ITEMS)
		return NULL;

	item = &st->items[st->count];
	uiCopyText(item->text, sizeof(item->text), text);
	item->onActivate = onActivate;
	item->ctx = ctx;
	st->count++;
	if (st->selected < 0)
		st->selected = 0;	/* first item becomes selected */
	return menu;
}

int uiMenuSelected(const Element *menu)
{
	if (menu == NULL)
		return -1;
	return ((const MenuState *)uiElementPayload(menu))->selected;
}

bool uiMenuSetSelected(Element *menu, int index)
{
	MenuState *st;

	if (menu == NULL)
		return false;
	st = uiElementPayload(menu);
	if (index < 0 || index >= st->count)
		return false;
	st->selected = index;
	return true;
}

int uiMenuItemCount(const Element *menu)
{
	if (menu == NULL)
		return 0;
	return ((const MenuState *)uiElementPayload(menu))->count;
}
