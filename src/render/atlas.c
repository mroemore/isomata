/*
 * Atlas packing math (see atlas.h for the contract). Pure: no SDL, no
 * allocation. All arithmetic is integer except the UV rects.
 */

#include "render/atlas.h"

#include <stddef.h>

int atlasCellSizeFor(int maxSize)
{
	if (maxSize <= ATLAS_CELL_MIN)
		return ATLAS_CELL_MIN;
	if (maxSize <= 32)
		return 32;
	return ATLAS_CELL_MAX;
}

bool atlasComputeLayout(int count, int cellSize, AtlasLayout *out)
{
	int columns;

	if (out == NULL || count < 1 || count > ATLAS_MAX_SLOTS)
		return false;
	if (cellSize < ATLAS_CELL_MIN || cellSize > ATLAS_CELL_MAX)
		return false;
	/* Smallest columns whose square reaches `count`, so the atlas stays as
	 * square as the slot count allows. */
	columns = 1;
	while (columns * columns < count)
		columns++;
	out->cellSize = cellSize;
	out->columns = columns;
	out->rows = (count + columns - 1) / columns;
	out->width = columns * cellSize;
	out->height = out->rows * cellSize;
	out->count = count;
	return true;
}

bool atlasSlotOrigin(const AtlasLayout *layout, int slot, int *outX, int *outY)
{
	if (layout == NULL || slot < 0 || slot >= layout->count)
		return false;
	if (outX != NULL)
		*outX = (slot % layout->columns) * layout->cellSize;
	if (outY != NULL)
		*outY = (slot / layout->columns) * layout->cellSize;
	return true;
}

bool atlasSlotRect(const AtlasLayout *layout, int slot, int srcSize,
		   AtlasRect *out)
{
	int x;
	int y;
	int size;
	float w;
	float h;

	if (out == NULL || !atlasSlotOrigin(layout, slot, &x, &y))
		return false;
	size = srcSize;
	if (size < 1)
		size = 1;
	if (size > layout->cellSize)
		size = layout->cellSize;
	w = (float)layout->width;
	h = (float)layout->height;
	out->u0 = ((float)x + 0.5f) / w;
	out->v0 = ((float)y + 0.5f) / h;
	out->u1 = ((float)x + (float)size - 0.5f) / w;
	out->v1 = ((float)y + (float)size - 0.5f) / h;
	return true;
}

bool atlasBlitPixels(uint8_t *dst, int dstW, int dstH, int dstX, int dstY,
		     const uint8_t *src, int srcW, int srcH, int srcStride,
		     int cellSize)
{
	int row;

	if (dst == NULL || src == NULL || dstW <= 0 || dstH <= 0 || srcW <= 0 ||
	    srcH <= 0 || srcStride < srcW * 4 || cellSize <= 0)
		return false;
	/* Never copy past the cell: a non-square (or oversized) source must not
	 * smear into the next slot. */
	if (srcW > cellSize)
		srcW = cellSize;
	if (srcH > cellSize)
		srcH = cellSize;
	for (row = 0; row < srcH; row++) {
		int dy = dstY + row;
		int copyW = srcW;
		int col;

		if (dy < 0 || dy >= dstH)
			continue;
		for (col = 0; col < copyW; col++) {
			int dx = dstX + col;
			const uint8_t *s;
			uint8_t *d;

			if (dx < 0 || dx >= dstW)
				continue;
			s = src + (size_t)row * (size_t)srcStride +
			    (size_t)col * 4;
			d = dst + ((size_t)dy * (size_t)dstW + (size_t)dx) * 4;
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			d[3] = s[3];
		}
	}
	return true;
}

bool atlasFillWhiteCell(uint8_t *dst, int dstW, int dstH, int x, int y,
			int size)
{
	int row;

	if (dst == NULL || dstW <= 0 || dstH <= 0 || size <= 0)
		return false;
	for (row = 0; row < size; row++) {
		int dy = y + row;
		int col;

		if (dy < 0 || dy >= dstH)
			continue;
		for (col = 0; col < size; col++) {
			int dx = x + col;
			uint8_t *d;

			if (dx < 0 || dx >= dstW)
				continue;
			d = dst + ((size_t)dy * (size_t)dstW + (size_t)dx) * 4;
			d[0] = 255;
			d[1] = 255;
			d[2] = 255;
			d[3] = 255;
		}
	}
	return true;
}

bool atlasFillFallbackCell(uint8_t *dst, int dstW, int dstH, int x, int y,
			   int size)
{
	int row;

	if (dst == NULL || dstW <= 0 || dstH <= 0 || size <= 0)
		return false;
	for (row = 0; row < size; row++) {
		int dy = y + row;
		int col;

		if (dy < 0 || dy >= dstH)
			continue;
		for (col = 0; col < size; col++) {
			int dx = x + col;
			uint8_t *d;
			bool magenta;

			if (dx < 0 || dx >= dstW)
				continue;
			magenta = (((col / 8) + (row / 8)) & 1) == 0;
			d = dst + ((size_t)dy * (size_t)dstW + (size_t)dx) * 4;
			if (magenta) {
				d[0] = 255;
				d[1] = 0;
				d[2] = 255;
				d[3] = 255;
			} else {
				d[0] = 0;
				d[1] = 0;
				d[2] = 0;
				d[3] = 255;
			}
		}
	}
	return true;
}
