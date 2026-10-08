#ifndef ISOMATA_UI_FONT_H
#define ISOMATA_UI_FONT_H

/*
 * SDL tier: a real font loaded through SDL3_ttf, plus a TextMeasure seam
 * (see element.h) built from its metrics. This is the ONLY file under
 * src/ui/ that includes SDL headers; every other ui module and the whole
 * headless suite stay SDL-free.
 *
 * Invariants:
 * - uiLoadFont opens `path` at `pixelSize` points. NULL/empty path or
 *   pixelSize <= 0 returns NULL. TTF_Init/TTF_Quit are reference-counted
 *   and paired one-for-one across uiLoadFont/uiFreeFont, so multiple
 *   fonts coexist and the library tears down when the last one is freed.
 * - uiFontMeasure returns a TextMeasure whose fn returns 0 on success and
 *   writes the pixel size of `text` (empty/NULL text measures 0x0), or
 *   nonzero on failure — matching the seam's 0-is-success convention. A
 *   NULL font yields a measure with fn == NULL (no measurement).
 * - uiFontHeight returns the font's line height, 0 for NULL.
 * - Measurement uses TTF_GetStringSize only: it needs no GPU device,
 *   window or renderer, so it is safe in the headless smoke path.
 */

#include "ui/element.h"

/* NULL on failure (bad args, TTF_Init failure, open failure). */
UiFont *uiLoadFont(const char *path, int pixelSize);

/* Closes the font and releases one TTF reference. NULL-safe. */
void uiFreeFont(UiFont *font);

/* Line height in pixels; 0 for NULL. */
int uiFontHeight(const UiFont *font);

/* Measurement seam for this font; fn == NULL when font is NULL. */
TextMeasure uiFontMeasure(UiFont *font);

#endif /* ISOMATA_UI_FONT_H */
