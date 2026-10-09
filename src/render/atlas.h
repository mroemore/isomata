#ifndef ISOMATA_RENDER_ATLAS_H
#define ISOMATA_RENDER_ATLAS_H

/*
 * Runtime texture atlas packing (pure). No SDL: the glue (gpu_backend.c)
 * loads the PNG pixels and uploads the assembled RGBA buffer; everything
 * about WHERE a source lands and WHICH UVs select it lives here so it is
 * headless-testable.
 *
 * Model: N square sources (16/32/64 texels) are placed on a uniform grid of
 * square cells. The cell is the smallest supported size (16/32/64) that fits
 * the largest source; a smaller source is anchored at its cell's top-left and
 * the rest of the cell is transparent padding. Sources are laid out in slot
 * order, row-major.
 *
 * UV rects are INSET by half a texel at each edge: with a nearest sampler a
 * rect that touched the exact region boundary could sample the neighbouring
 * cell (or the transparent padding) on its edge texels; the half-texel inset
 * maps the four quad corners onto the centre of the first/last source texel,
 * so every source texel is sampled exactly once and no neighbour bleeds in.
 *
 * The atlas is square-cell but not necessarily square: `columns` = ceil of
 * sqrt(count) keeps it close to square, `rows` = ceil(count / columns).
 */

#include <stdbool.h>
#include <stdint.h>

#define ATLAS_CELL_MIN 16
#define ATLAS_CELL_MAX 64
#define ATLAS_MAX_SLOTS 256

typedef struct AtlasRect {
	float u0;	/* left edge (smallest u) */
	float v0;	/* top edge (smallest v; v grows downward) */
	float u1;	/* right edge */
	float v1;	/* bottom edge */
} AtlasRect;

typedef struct AtlasLayout {
	int cellSize;	/* texels per cell (16/32/64) */
	int columns;
	int rows;
	int width;	/* atlas texels across */
	int height;	/* atlas texels down */
	int count;	/* slots packed */
} AtlasLayout;

/* Smallest supported cell size (16/32/64) that fits `maxSize`. Non-positive
 * sizes clamp to 16; sizes above 64 clamp to 64. */
int atlasCellSizeFor(int maxSize);

/* Grid layout for `count` square cells of `cellSize` texels. Returns false
 * (and leaves `out` untouched) on a NULL out, count outside [1,
 * ATLAS_MAX_SLOTS], or a cellSize outside [16, 64]. */
bool atlasComputeLayout(int count, int cellSize, AtlasLayout *out);

/* Pixel origin (top-left) of `slot` in the atlas. False on NULL layout / out
 * of range. */
bool atlasSlotOrigin(const AtlasLayout *layout, int slot, int *outX, int *outY);

/* UV rect (half-texel inset) of the `srcSize`-texel source placed at `slot`'s
 * origin. `srcSize` is clamped to [1, cellSize]. False on NULL / out of
 * range. */
bool atlasSlotRect(const AtlasLayout *layout, int slot, int srcSize,
		   AtlasRect *out);

/* Copy a 4-bytes-per-pixel `srcW`x`srcH` source (row stride `srcStride`
 * bytes) into `dst` at (dstX, dstY), clipped to BOTH the destination bounds
 * and a `cellSize`x`cellSize` cell anchored at (dstX, dstY), so a source wider
 * or taller than its cell cannot smear into the neighbouring cells. Returns
 * false on a NULL pointer, a non-positive size, or a stride smaller than
 * srcW * 4. */
bool atlasBlitPixels(uint8_t *dst, int dstW, int dstH, int dstX, int dstY,
		     const uint8_t *src, int srcW, int srcH, int srcStride,
		     int cellSize);

/* Fill a `size`x`size` region of `dst` at (x, y) with the generated fallback
 * pattern: an 8-texel magenta/black RGBA checker that reads as "missing
 * texture". Clipped to the destination bounds; false on NULL / non-positive
 * size. */
bool atlasFillFallbackCell(uint8_t *dst, int dstW, int dstH, int x, int y,
			   int size);

/* Fill a `size`x`size` region of `dst` at (x, y) with opaque white. The debug
 * view samples this cell so the light colour is not tinted by a material
 * texture. Clipped to the destination bounds; false on NULL / non-positive
 * size. */
bool atlasFillWhiteCell(uint8_t *dst, int dstW, int dstH, int x, int y,
			int size);

#endif /* ISOMATA_RENDER_ATLAS_H */
