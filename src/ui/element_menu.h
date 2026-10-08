#ifndef ISOMATA_UI_ELEMENT_MENU_H
#define ISOMATA_UI_ELEMENT_MENU_H

/*
 * Vertical menu of selectable, activatable text rows. Headless (no SDL);
 * see element.h for the tree and the draw/text seams, element_button.h for
 * UiActionFn.
 *
 * Invariants:
 * - Items live inline: UI_MENU_MAX_ITEMS (16) rows, each text capped at
 *   UI_TEXT_MAX. Adding beyond the cap fails and adds nothing; text is
 *   truncated safely and NULL text is empty.
 * - Selection is an index in [0, count-1], or -1 exactly when the menu is
 *   empty. Adding the first item selects it (index 0). uiMenuSetSelected
 *   accepts only an in-range index (false otherwise, selection unchanged).
 * - Navigation wraps: UI_NAV_UP moves to the previous item (0 -> count-1),
 *   UI_NAV_DOWN to the next (count-1 -> 0). Both are consumed while the
 *   menu is non-empty and NOT consumed while it is empty.
 * - UI_ACTIVATE fires the selected item's callback (ctx passed through)
 *   and is consumed when the menu is non-empty; it is not consumed when
 *   empty. UI_CANCEL is NEVER consumed (it must reach the owner so a pause
 *   menu can close). UI_NAV_LEFT/RIGHT are not consumed either.
 * - A tap inside the menu's rect selects the row under the tap and fires
 *   it, consuming the tap. Rows tile the rect edge-consistently:
 *   row i spans y + h*i/count .. y + h*(i+1)/count (integer division), so
 *   adjacent rows never gap or overlap. A tap outside the rect, on a
 *   zero-area rect, or on an empty menu is not consumed.
 * - draw fills each row (UI_COLOR_SELECTION for the selected row,
 *   UI_COLOR_BACKGROUND otherwise) then draws its text at the row's
 *   top-left in UI_COLOR_TEXT. An empty menu fills its whole rect with
 *   UI_COLOR_BACKGROUND and draws no text.
 */

#include "ui/element.h"
#include "ui/element_button.h"

#define UI_MENU_MAX_ITEMS 16

/* A menu with no items (selected -1). NULL style = zeroed. */
Element *uiCreateMenu(const TextStyle *style);

/* Returns the menu on success, NULL when menu is NULL or already full. */
Element *uiMenuAddItem(Element *menu, const char *text, UiActionFn onActivate,
		       void *ctx);

/* Selected index, or -1 when menu is NULL or empty. */
int uiMenuSelected(const Element *menu);

/* True when the index was applied (in range); false leaves it unchanged. */
bool uiMenuSetSelected(Element *menu, int index);

/* Number of items, 0 when menu is NULL. */
int uiMenuItemCount(const Element *menu);

#endif /* ISOMATA_UI_ELEMENT_MENU_H */
