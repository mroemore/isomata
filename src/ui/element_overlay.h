#ifndef ISOMATA_UI_ELEMENT_OVERLAY_H
#define ISOMATA_UI_ELEMENT_OVERLAY_H

/*
 * Overlay: a dim full-rect backdrop that contains children (pause/settings
 * screens). Headless (no SDL); see element.h for the tree.
 *
 * Invariants:
 * - Stateless (zero inline payload). draw() fills the whole rect with
 *   UI_COLOR_OVERLAY (0x00000099, pinned); children are drawn by the base
 *   walk afterwards, in order.
 * - Purely visual: it has no layout callback and consumes NO intents or
 *   pointer taps, so a child menu/button still receives input and
 *   UI_CANCEL keeps propagating to the owner. Modality (blocking input to
 *   elements behind the overlay) is the input layer's decision, not this
 *   element's.
 */

#include "ui/element.h"

Element *uiCreateOverlay(void);

#endif /* ISOMATA_UI_ELEMENT_OVERLAY_H */
