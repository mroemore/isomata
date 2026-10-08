#ifndef ISOMATA_UI_SCALE_H
#define ISOMATA_UI_SCALE_H

/*
 * DPI scaling for the UI: density is converted into a clamped scale factor
 * and coordinates are moved between virtual (logical UI) pixels and
 * physical (device) pixels with consistent rounding.
 *
 * Invariants:
 * - uiScaleFromDensity clamps to [1.0, 3.0]. A density that is NaN, <= 0,
 *   or infinite yields the identity scale 1.0 (safe degradation when no
 *   trustworthy display information exists; infinite is never useful as a
 *   scale, hence not clamped to the top).
 * - uiScaleVirtualToPhysical / uiScalePhysicalToVirtual round with
 *   lroundf (half away from zero) after the multiply/divide. A scale that
 *   is NaN, <= 0, or infinite is treated as identity (documented here so
 *   tests pin it); callers never produce such a scale through
 *   uiScaleFromDensity, so this only ever guards direct misuse.
 * - uiScaleRect converts a virtual rect to physical pixels
 *   edge-consistently: x and y are scaled first, then the RIGHT/BOTTOM
 *   corners (x+w, y+h) are scaled and w/h derived as differences. Two
 *   virtual rects sharing an edge therefore share their physical edge
 *   too, no matter how the rounds fall — adjacent rects can never gap or
 *   overlap by rounding. The direction is virtual -> physical (multiply);
 *   physical -> virtual callers invert with the physical-to-virtual
 *   conversion on the corners the same way.
 * - Pure module: no SDL; all state lives in the callers' own values.
 */

#include <stdbool.h>

/* Density (OS UI scale, see platform.h) -> clamped scale factor. */
float uiScaleFromDensity(float density);

/* Virtual (logical UI) pixels -> physical pixels. */
int uiScaleVirtualToPhysical(int px, float scale);

/* Physical pixels -> virtual pixels. */
int uiScalePhysicalToVirtual(int px, float scale);

/* Scale a rect (in place) from virtual to physical pixels per the
 * edge-consistent rule above. Any NULL out pointer makes the call a
 * no-op. */
void uiScaleRect(int *x, int *y, int *w, int *h, float scale);

#endif /* ISOMATA_UI_SCALE_H */
