#ifndef ISOMATA_ENTITIES_EASING_H
#define ISOMATA_ENTITIES_EASING_H

/*
 * Pure easing (tween) curve library. No SDL, no allocation, no state.
 *
 * An easing function maps a normalised progress t to a normalised output in
 * the same [0, 1] range. `easingApply` clamps t to [0, 1] first, so
 * out-of-range or non-finite progress can never produce an out-of-range
 * parameter (a NaN t becomes 0). Every curve pins f(0) = 0 and f(1) = 1:
 *   EASE_LINEAR   f(t) = t
 *   EASE_IN       f(t) = t^2
 *   EASE_OUT      f(t) = 1 - (1 - t)^2
 *   EASE_IN_OUT   f(t) = 3t^2 - 2t^3   (smoothstep)
 * These are the interpolation functions the tile-movement layer uses between
 * one tile centre and the next.
 */

typedef enum EasingFn {
	EASE_LINEAR = 0,
	EASE_IN,
	EASE_OUT,
	EASE_IN_OUT,
	EASE_FN_COUNT
} EasingFn;

/* Apply easing `fn` to `t`, clamping t to [0, 1] first. An unknown enum value
 * falls back to linear (never crashes). Pure and deterministic. */
float easingApply(EasingFn fn, float t);

#endif /* ISOMATA_ENTITIES_EASING_H */
