/*
 * Pure easing curves (see entities/easing.h).
 *
 * `easingApply` clamps t to [0, 1] with a NaN-safe comparison: a NaN or a
 * negative t collapses to 0, a t above 1 to 1, then the curve is evaluated.
 * Every curve is a polynomial in t, so the result is deterministic across
 * runs and platforms (no libm, no branchiness beyond the enum switch).
 */

#include "entities/easing.h"

float easingApply(EasingFn fn, float t)
{
	/* NaN-safe clamp: !(t > 0) is true for NaN and negatives. */
	if (!(t > 0.0f))
		t = 0.0f;
	else if (t > 1.0f)
		t = 1.0f;

	switch (fn) {
	case EASE_IN:
		return t * t;
	case EASE_OUT: {
		float u = 1.0f - t;

		return 1.0f - u * u;
	}
	case EASE_IN_OUT:
		/* smoothstep 3t^2 - 2t^3, factored to avoid a cube. */
		return t * t * (3.0f - 2.0f * t);
	case EASE_LINEAR:
	default:
		return t;
	}
}
