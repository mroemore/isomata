/*
 * Toast: hold-then-fade notification. See toast.h for the invariant block
 * (pinned constants, surplus-dt carry across phases, ignored bad dt,
 * alpha-scaled draw).
 */

#include "ui/toast.h"

#include <math.h>
#include <stdint.h>

typedef enum {
	TOAST_PHASE_HIDDEN = 0,
	TOAST_PHASE_SHOWING,
	TOAST_PHASE_FADING,
} ToastPhase;

typedef struct ToastState {
	char text[UI_TEXT_MAX];
	TextStyle style;
	ToastPhase phase;
	float elapsed;
} ToastState;

/* Multiply the alpha byte of an 0xRRGGBBAA color by `a` (clamped). */
static uint32_t scaleAlpha(uint32_t rgba, float a)
{
	uint32_t alpha = rgba & 0xFFu;
	uint32_t scaled;

	if (a <= 0.0f)
		scaled = 0;
	else if (a >= 1.0f)
		scaled = alpha;
	else
		scaled = (uint32_t)lroundf((float)alpha * a);
	if (scaled > 255u)
		scaled = 255u;
	return (rgba & 0xFFFFFF00u) | scaled;
}

static void toastDraw(Element *self, UiDrawCtx *ctx)
{
	const ToastState *st = uiElementPayload(self);
	float alpha = toastAlpha(self);
	uint32_t fill;

	if (alpha <= 0.0f)
		return;		/* hidden: nothing to draw */

	fill = scaleAlpha(UI_COLOR_BACKGROUND, alpha);
	ctx->vt->fillRect(ctx, self->x, self->y, self->w, self->h, fill);
	if (st->text[0] != '\0') {
		ctx->vt->drawText(ctx, self->x, self->y, st->text, &st->style,
				  scaleAlpha(UI_COLOR_TEXT, alpha));
	}
}

static const ElementVt toastVt = {
	.layout = NULL,
	.draw = toastDraw,
	.handleIntent = NULL,
	.handlePointer = NULL,
	.destroy = NULL,
	.size = sizeof(ToastState),
	.name = "toast",
};

Element *uiCreateToast(const TextStyle *style)
{
	Element *toast = uiCreateElement(&toastVt);
	ToastState *st;

	if (toast == NULL)
		return NULL;
	st = uiElementPayload(toast);
	st->text[0] = '\0';
	st->style = style != NULL ? *style : (TextStyle){ 0 };
	st->phase = TOAST_PHASE_HIDDEN;
	st->elapsed = 0.0f;
	return toast;
}

void toastShow(Element *toast, const char *text)
{
	ToastState *st;

	if (toast == NULL)
		return;
	st = uiElementPayload(toast);
	uiCopyText(st->text, sizeof(st->text), text);
	st->phase = TOAST_PHASE_SHOWING;
	st->elapsed = 0.0f;
}

void toastUpdate(Element *toast, float dt)
{
	ToastState *st;

	if (toast == NULL)
		return;
	if (!isfinite(dt) || dt <= 0.0f)
		return;		/* no rewind, no NaN poisoning */
	st = uiElementPayload(toast);
	if (st->phase == TOAST_PHASE_HIDDEN)
		return;

	st->elapsed += dt;

	/* Sequential (not else-if) checks so one large dt can cross both
	 * boundaries; the surplus carries from hold into fade. */
	if (st->phase == TOAST_PHASE_SHOWING && st->elapsed >= TOAST_HOLD_SECONDS) {
		st->elapsed -= TOAST_HOLD_SECONDS;
		st->phase = TOAST_PHASE_FADING;
	}
	if (st->phase == TOAST_PHASE_FADING && st->elapsed >= TOAST_FADE_SECONDS) {
		st->elapsed = 0.0f;
		st->phase = TOAST_PHASE_HIDDEN;
	}
}

float toastAlpha(const Element *toast)
{
	const ToastState *st;

	if (toast == NULL)
		return 0.0f;
	st = uiElementPayload(toast);
	if (st->phase == TOAST_PHASE_HIDDEN)
		return 0.0f;
	if (st->phase == TOAST_PHASE_SHOWING)
		return 1.0f;
	/* Fading. */
	{
		float alpha = 1.0f - st->elapsed / TOAST_FADE_SECONDS;

		if (alpha < 0.0f)
			alpha = 0.0f;
		if (alpha > 1.0f)
			alpha = 1.0f;
		return alpha;
	}
}

bool toastVisible(const Element *toast)
{
	if (toast == NULL)
		return false;
	return ((const ToastState *)uiElementPayload(toast))->phase !=
	       TOAST_PHASE_HIDDEN;
}
