/*
 * Pane: single-axis container layout. See layout.h for the invariant
 * block (content inset, gap, fixed/weighted split, the cumulative-floor
 * weighted rule that tiles the leftover main axis exactly, cross-axis
 * stretch, draw fill).
 */

#include "ui/layout.h"

typedef struct PaneState {
	UiAxis axis;
	int padding;
	int gap;
} PaneState;

static void paneDestroy(Element *self)
{
	(void)self;
	/* No owned buffers: the text/children live in the base tree. */
}

static void paneDraw(Element *self, UiDrawCtx *ctx)
{
	ctx->vt->fillRect(ctx, self->x, self->y, self->w, self->h,
			  UI_COLOR_BACKGROUND);
}

/* (w or h) along the pane's main axis; the other extent is the cross
 * axis. */
static int mainExtent(const PaneState *st, const Element *child)
{
	return st->axis == UI_AXIS_HORIZONTAL ? child->w : child->h;
}

static void paneLayout(Element *self)
{
	const PaneState *st = uiElementPayload(self);
	Element *child;
	int pad;
	int gap;
	int contentX;
	int contentY;
	int contentW;
	int contentH;
	int main;
	int cross;
	int origin;
	int n = 0;
	int gapTotal;
	int fixedTotal;
	int avail;
	int long long totalWeight = 0;
	int long long cumWeight = 0;
	int long long cumAlloc = 0;
	int cursor;

	/* Negative insets degrade to 0 (documented). */
	pad = st->padding > 0 ? st->padding : 0;
	gap = st->gap > 0 ? st->gap : 0;

	contentX = self->x + pad;
	contentY = self->y + pad;
	contentW = self->w - 2 * pad;
	contentH = self->h - 2 * pad;
	if (contentW < 0)
		contentW = 0;
	if (contentH < 0)
		contentH = 0;

	for (child = self->firstChild; child != NULL; child = child->nextSibling)
		n++;
	if (n == 0)
		return;

	main = st->axis == UI_AXIS_HORIZONTAL ? contentW : contentH;
	cross = st->axis == UI_AXIS_HORIZONTAL ? contentH : contentW;
	origin = st->axis == UI_AXIS_HORIZONTAL ? contentX : contentY;

	gapTotal = gap * (n - 1);
	fixedTotal = 0;
	for (child = self->firstChild; child != NULL; child = child->nextSibling) {
		if (uiWeight(child) > 0) {
			totalWeight += uiWeight(child);
		} else {
			int extent = mainExtent(st, child);

			if (extent > 0)
				fixedTotal += extent;
		}
	}

	/* Leftover main-axis space for the weighted children, floored at 0
	 * (an over-full pane collapses weighted children instead of
	 * producing negative extents). */
	avail = main - gapTotal - fixedTotal;
	if (avail < 0)
		avail = 0;

	/* Cumulative-floor allocation: weighted child i gets
	 * floor(avail * cumWeight_i / totalWeight) minus the previous
	 * cumulative floor. Because the last cumulative weight equals
	 * totalWeight, these differences tile `avail` exactly. */
	cursor = origin;
	child = self->firstChild;
	while (child != NULL) {
		int extent;

		if (child != self->firstChild)
			cursor += gap;

		if (uiWeight(child) > 0 && totalWeight > 0) {
			int long long target;

			cumWeight += uiWeight(child);
			target = ((int long long)avail * cumWeight) / totalWeight;
			extent = (int)(target - cumAlloc);
			cumAlloc = target;
		} else {
			extent = mainExtent(st, child);
			if (extent < 0)
				extent = 0;
		}

		if (st->axis == UI_AXIS_HORIZONTAL) {
			uiSetRect(child, cursor, contentY, extent, cross);
		} else {
			uiSetRect(child, contentX, cursor, cross, extent);
		}
		cursor += extent;
		child = child->nextSibling;
	}
}

static const ElementVt paneVt = {
	.layout = paneLayout,
	.draw = paneDraw,
	.handleIntent = NULL,
	.handlePointer = NULL,
	.destroy = paneDestroy,
	.size = sizeof(PaneState),
	.name = "pane",
};

Element *uiCreatePane(UiAxis axis, int padding, int gap)
{
	Element *pane = uiCreateElement(&paneVt);
	PaneState *st;

	if (pane == NULL)
		return NULL;
	st = uiElementPayload(pane);
	st->axis = axis;
	st->padding = padding;
	st->gap = gap;
	return pane;
}
