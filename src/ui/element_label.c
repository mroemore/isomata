/*
 * Label: inline bounded text drawn at the element's top-left. See
 * element_label.h for the invariant block (UI_TEXT_MAX cap, NULL = empty,
 * draw passthrough of the TextStyle).
 */

#include "ui/element_label.h"

typedef struct LabelState {
	char text[UI_TEXT_MAX];
	TextStyle style;
} LabelState;

static void labelDraw(Element *self, UiDrawCtx *ctx)
{
	const LabelState *st = uiElementPayload(self);

	if (st->text[0] == '\0')
		return;		/* nothing to draw for empty text */
	ctx->vt->drawText(ctx, self->x, self->y, st->text, &st->style,
			  UI_COLOR_TEXT);
}

static const ElementVt labelVt = {
	.layout = NULL,
	.draw = labelDraw,
	.handleIntent = NULL,
	.handlePointer = NULL,
	.destroy = NULL,
	.size = sizeof(LabelState),
	.name = "label",
};

Element *uiCreateLabel(const char *text, const TextStyle *style)
{
	Element *label = uiCreateElement(&labelVt);
	LabelState *st;

	if (label == NULL)
		return NULL;
	st = uiElementPayload(label);
	uiCopyText(st->text, sizeof(st->text), text);
	st->style = style != NULL ? *style : (TextStyle){ 0 };
	return label;
}

void uiLabelSetText(Element *label, const char *text)
{
	LabelState *st;

	if (label == NULL)
		return;
	st = uiElementPayload(label);
	uiCopyText(st->text, sizeof(st->text), text);
}

const char *uiLabelText(const Element *label)
{
	if (label == NULL)
		return NULL;
	return ((const LabelState *)uiElementPayload(label))->text;
}
