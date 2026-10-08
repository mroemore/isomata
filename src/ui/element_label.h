#ifndef ISOMATA_UI_ELEMENT_LABEL_H
#define ISOMATA_UI_ELEMENT_LABEL_H

/*
 * Static text element. Headless (no SDL); the TextStyle carries the
 * measurement/rendering seam (see element.h).
 *
 * Invariants:
 * - The text lives inline in a UI_TEXT_MAX (128, NUL included) byte
 *   buffer: longer input is truncated on a byte boundary, always
 *   NUL-terminated. NULL text is the empty string.
 * - A label has no children and no layout (layout() is NULL). Its draw()
 *   calls ctx->drawText at the label's top-left with UI_COLOR_TEXT; the
 *   style is passed through untouched (a style with measure.fn == NULL is
 *   legal — the render context decides what to do).
 * - uiLabelText never returns NULL for a non-NULL label; it returns NULL
 *   for a NULL element. The returned pointer is owned by the label and is
 *   valid until the next uiLabelSetText or the label's destruction.
 */

#include "ui/element.h"

Element *uiCreateLabel(const char *text, const TextStyle *style);

/* Replaces the text (bounded copy; NULL = empty). */
void uiLabelSetText(Element *label, const char *text);

/* Borrowed, never NULL for a live label. */
const char *uiLabelText(const Element *label);

#endif /* ISOMATA_UI_ELEMENT_LABEL_H */
