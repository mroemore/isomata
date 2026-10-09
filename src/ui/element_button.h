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
 * - draw fills the whole rect with UI_COLOR_BUTTON, then draws EITHER an
 *   icon (icon mode) OR the text (text mode), never both:
 *     * text mode draws the text centered using the TextStyle's measure
 *       seam: with a working measure, top-left = (x + (w - textW)/2,
 *       y + (h - textH)/2) (C integer division); when measure.fn is NULL
 *       or reports failure the text is drawn at the rect's top-left
 *       instead (zero-size fallback, documented). Empty text draws no text
 *       call at all. This is byte-identical to the pre-icon behavior.
 *     * icon mode (uiButtonSetIcon) draws NO text; the icon is the largest
 *       SQUARE that fits inside the rect minus UI_BUTTON_ICON_INSET on
 *       each side, centered, tinted UI_COLOR_TEXT. All shipped icons are
 *       square (64x64), so the square is the aspect-preserving fit. A ctx
 *       whose drawImage is NULL, or a zero-area rect, skips the image but
 *       still fills.
 * - uiButtonSetIcon(button, UI_ICON_COUNT) — or any out-of-range value —
 *   clears icon mode and restores text mode.
 */

#include "ui/element.h"

/* Icon inset from the button rect (virtual px per side): the drawn icon is
 * the largest square that fits inside rect - 2*inset, centered. */
#define UI_BUTTON_ICON_INSET 6

typedef void (*UiActionFn)(void *ctx);

Element *uiCreateButton(const char *text, const TextStyle *style,
			UiActionFn onActivate, void *ctx);

/* Icon mode. NULL button is ignored; an out-of-range icon (including
 * UI_ICON_COUNT, the "no icon" sentinel) clears back to text mode. */
void uiButtonSetIcon(Element *button, UiIcon icon);

/* The button's icon, or UI_ICON_COUNT when it is in text mode (also for a
 * NULL button). */
UiIcon uiButtonIcon(const Element *button);

#endif /* ISOMATA_UI_ELEMENT_BUTTON_H */
