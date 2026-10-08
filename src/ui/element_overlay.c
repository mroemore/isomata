/*
 * Overlay: a stateless dim backdrop. See element_overlay.h for the
 * invariant block (pinned fill color, purely visual, consumes nothing).
 */

#include "ui/element_overlay.h"

static void overlayDraw(Element *self, UiDrawCtx *ctx)
{
	ctx->vt->fillRect(ctx, self->x, self->y, self->w, self->h,
			  UI_COLOR_OVERLAY);
}

static const ElementVt overlayVt = {
	.layout = NULL,
	.draw = overlayDraw,
	.handleIntent = NULL,
	.handlePointer = NULL,
	.destroy = NULL,
	.size = 0,		/* stateless */
	.name = "overlay",
};

Element *uiCreateOverlay(void)
{
	return uiCreateElement(&overlayVt);
}
