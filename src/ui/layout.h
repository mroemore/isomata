#ifndef ISOMATA_UI_LAYOUT_H
#define ISOMATA_UI_LAYOUT_H

/*
 * Pane: a container element that lays its CHILDREN out along one axis.
 * Headless (no SDL); see element.h for the tree, the text seam and the
 * palette.
 *
 * Invariants:
 * - The pane insets its content by `padding` on all four sides (contentW =
 *   w - 2*padding, contentH = h - 2*padding, floored at 0). Adjacent
 *   children are separated by `gap`; there is no gap before the first or
 *   after the last child. Negative padding and negative gap are treated as
 *   0 (documented; tests pin it).
 * - Child order along the main axis is tree order. A child with weight <= 0
 *   is FIXED: it keeps its current main-axis extent (h on a vertical pane,
 *   w on a horizontal one), clamped at 0. A child with weight > 0 is
 *   WEIGHTED and shares what is left of the main axis after padding, gaps
 *   and fixed children.
 * - Weighted distribution: with `avail` the leftover main-axis space
 *   (floored at 0), weighted child i's size is the difference between the
 *   floor of `avail * cumulativeWeight / totalWeight` at i and at i-1. The
 *   successive floors tile `avail` EXACTLY (the last cumulative weight is
 *   totalWeight), so rounding never leaves a tail gap and never overflows
 *   the pane; the fractional remainder of each ratio lands on the first
 *   weighted child that crosses the next integer. All weighted children of
 *   an over-full pane (avail 0) collapse to size 0 while fixed children
 *   keep their requested extent (the pane may then overflow — documented,
 *   the layout never grows a child down).
 * - The cross axis is not distributed: every child spans the full content
 *   extent on the cross axis.
 * - The pane's draw() fills its whole rect with UI_COLOR_BACKGROUND;
 *   children are drawn by the base walk afterwards, in order.
 */

#include "ui/element.h"

typedef enum {
	UI_AXIS_VERTICAL = 0,
	UI_AXIS_HORIZONTAL,
	UI_AXIS_COUNT,
} UiAxis;

/* NULL on allocation failure; axis is not validated (values outside the
 * enum behave like VERTICAL for the != HORIZONTAL test). */
Element *uiCreatePane(UiAxis axis, int padding, int gap);

#endif /* ISOMATA_UI_LAYOUT_H */
