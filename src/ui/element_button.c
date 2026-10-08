/*
 * Button: fill + centered text, tap/activate callback. See
 * element_button.h for the invariant block (hit-test consumption, the
 * UI_ACTIVATE-only intent rule, the measure fallback).
 */

#include "ui/element_button.h"

typedef struct ButtonState {
	char text[UI_TEXT_MAX];
	TextStyle style;
	UiActionFn onActivate;
	void *ctx;
} ButtonState;

/* Measure the text through the seam; returns false when there is no
 * usable measurement (fn absent or reporting failure). */
static bool measureText(const TextStyle *style, const char *text, int *w, int *h)
{
	if (style->measure.fn == NULL)
		return false;
	if (style->measure.fn(style->measure.ctx, text, w, h) != 0)
		return false;
	return true;
}

static void buttonFire(ButtonState *st)
{
	if (st->onActivate != NULL)
		st->onActivate(st->ctx);
}

static void buttonDraw(Element *self, UiDrawCtx *ctx)
{
	const ButtonState *st = uiElementPayload(self);

	ctx->vt->fillRect(ctx, self->x, self->y, self->w, self->h,
			  UI_COLOR_BUTTON);
	if (st->text[0] == '\0')
		return;

	int tx = self->x;
	int ty = self->y;
	int w = 0;
	int h = 0;

	if (measureText(&st->style, st->text, &w, &h)) {
		tx = self->x + (self->w - w) / 2;
		ty = self->y + (self->h - h) / 2;
	}
	ctx->vt->drawText(ctx, tx, ty, st->text, &st->style, UI_COLOR_TEXT);
}

static bool buttonHandleIntent(Element *self, UiIntent intent)
{
	ButtonState *st = uiElementPayload(self);

	/* Only activation is ours; "focused" is the routing decision the
	 * input layer already made by delivering the intent here. */
	if (intent != UI_ACTIVATE)
		return false;
	buttonFire(st);
	return true;
}

static bool buttonHandlePointer(Element *self, int x, int y)
{
	ButtonState *st = uiElementPayload(self);

	if (self->w <= 0 || self->h <= 0)
		return false;
	if (x < self->x || x >= self->x + self->w)
		return false;
	if (y < self->y || y >= self->y + self->h)
		return false;
	buttonFire(st);
	return true;
}

static const ElementVt buttonVt = {
	.layout = NULL,
	.draw = buttonDraw,
	.handleIntent = buttonHandleIntent,
	.handlePointer = buttonHandlePointer,
	.destroy = NULL,
	.size = sizeof(ButtonState),
	.name = "button",
};

Element *uiCreateButton(const char *text, const TextStyle *style,
			UiActionFn onActivate, void *ctx)
{
	Element *button = uiCreateElement(&buttonVt);
	ButtonState *st;

	if (button == NULL)
		return NULL;
	st = uiElementPayload(button);
	uiCopyText(st->text, sizeof(st->text), text);
	st->style = style != NULL ? *style : (TextStyle){ 0 };
	st->onActivate = onActivate;
	st->ctx = ctx;
	return button;
}
