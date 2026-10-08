#ifndef ISOMATA_UI_ELEMENT_BUTTON_H
#define ISOMATA_UI_ELEMENT_BUTTON_H

/*
 * Button element: a filled, text-centered, activatable rect. Headless (no
 * SDL); see element.h for the tree and the draw/text seams.
 *
 * Invariants:
 * - Text lives inline (UI_TEXT_MAX, truncated safely, NULL = empty) and
 *   the TextStyle is stored by value (NULL style = zeroed).
 * - handlePointer consumes a tap whose virtual coords fall inside the
 *   button's rect [x, x+w) x [y, y+h), invoking onActivate(ctx) exactly
 *   once when set; a tap outside, or on a zero-area rect, is not consumed
 *   and fires nothing.
 * - handleIntent consumes UI_ACTIVATE only, invoking onActivate(ctx) when
 *   set ("focused" here means the input layer routed the intent to this
 *   element; there is no hidden focus flag). Every other intent returns
 *   false, so navigation and cancel keep propagating.
 * - draw fills the whole rect with UI_COLOR_BUTTON, then draws the text
 *   centered using the TextStyle's measure seam: with a working measure,
 *   top-left = (x + (w - textW)/2, y + (h - textH)/2) (C integer
 *   division); when measure.fn is NULL or reports failure the text is
 *   drawn at the rect's top-left instead (zero-size fallback, documented).
 *   Empty text draws no text call at all.
 */

#include "ui/element.h"

typedef void (*UiActionFn)(void *ctx);

Element *uiCreateButton(const char *text, const TextStyle *style,
			UiActionFn onActivate, void *ctx);

#endif /* ISOMATA_UI_ELEMENT_BUTTON_H */
