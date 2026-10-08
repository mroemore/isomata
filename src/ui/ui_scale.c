/*
 * UI scale: density -> clamped scale, and pixel conversion between the
 * virtual (logical UI) raster and the physical (device) raster. See
 * ui_scale.h for the invariant block: the [1.0, 3.0] clamp with invalid
 * densities collapsed to 1.0, lroundf rounding (ties away from zero), the
 * identity rule for invalid scales, and the edge-consistent rect rule
 * (x/y scaled, w/h derived from the scaled corners).
 */

#include "ui/ui_scale.h"

#include <math.h>
#include <stddef.h>

/* The scale guard shared by every conversion: a scale that is NaN, <= 0
 * or infinite is treated as identity (documented in ui_scale.h). */
static bool scaleInvalid(float scale)
{
	return !isfinite(scale) || scale <= 0.0f;
}

float uiScaleFromDensity(float density)
{
	if (scaleInvalid(density))
		return 1.0f;	/* NaN, <= 0, ±inf: unusable -> identity */
	if (density < 1.0f)
		return 1.0f;
	if (density > 3.0f)
		return 3.0f;
	return density;
}

int uiScaleVirtualToPhysical(int px, float scale)
{
	if (scaleInvalid(scale))
		return px;	/* identity */
	return lroundf((float)px * scale);
}

int uiScalePhysicalToVirtual(int px, float scale)
{
	if (scaleInvalid(scale))
		return px;	/* identity */
	return lroundf((float)px / scale);
}

void uiScaleRect(int *x, int *y, int *w, int *h, float scale)
{
	int nx;
	int ny;
	int right;
	int bottom;

	if (x == NULL || y == NULL || w == NULL || h == NULL)
		return;
	if (scaleInvalid(scale))
		return;		/* identity: rect untouched */

	/* Corners first, extents derived: the shared-edge invariant. */
	nx = uiScaleVirtualToPhysical(*x, scale);
	ny = uiScaleVirtualToPhysical(*y, scale);
	right = uiScaleVirtualToPhysical(*x + *w, scale);
	bottom = uiScaleVirtualToPhysical(*y + *h, scale);
	*x = nx;
	*y = ny;
	*w = right - nx;
	*h = bottom - ny;
}
