#ifndef ISOMATA_RENDER_GRID_H
#define ISOMATA_RENDER_GRID_H

/*
 * Infinite ground grid (pure: camera3d + math3d + libm only, no SDL).
 *
 * buildGridQuad produces the single ground quad (y = 0) the GPU tier draws
 * beneath the voxel map so the ground reads as an infinite grid:
 *
 * - The quad is a world-axis-aligned square centred on the camera's ground
 *   target. Its half-extent is the screen's ground circumradius times
 *   GRID_EXTENT_SCALE, so it covers the full visible ground footprint at
 *   every zoom in [CAMERA_ZOOM_MIN, CAMERA_ZOOM_MAX] and every aspect up to
 *   GRID_MAX_ASPECT. The footprint is the intersection of the view rectangle
 *   with the ground plane: half-width H*aspect and half-length H/sin(pitch),
 *   where H = CAMERA_BASE_HALF_HEIGHT / zoom (the visible half-height).
 *   Its circumradius is therefore sqrt((H*aspect)^2 + (H/sin pitch)^2); a
 *   square whose inscribed circle reaches that radius contains the rotated
 *   footprint for any yaw, so the quad is never yaw-dependent.
 *
 * - The UVs ARE the world XZ of the matching corner, so grid lines are
 *   anchored to world coordinates: panning and zooming the camera move the
 *   quad but never slide the pattern.
 *
 * - fadeStart/fadeEnd are radial (world XZ) alpha-fade radii scaled from the
 *   same H, so the fade band sits beyond the visible footprint at every zoom
 *   (GRID_FADE_START_SCALE >= 1, GRID_FADE_END_SCALE >= 1) and the fade
 *   completes strictly inside the quad edge (GRID_FADE_END_SCALE <
 *   GRID_EXTENT_SCALE), so the quad edge is never visible as a hard cut.
 *
 * A NULL camera or a non-positive/NaN aspect falls back to safe defaults
 * (yaw 0, zoom CAMERA_ZOOM_DEFAULT, target origin; aspect 1); a NULL `out`
 * is a no-op.
 */

#include "render/camera3d.h"

/* Aspect sweep bound for the coverage property (the quad is sized from the
 * screen diagonal, so larger aspects still get a covered quad; this only
 * bounds the test sweep). */
#define GRID_MAX_ASPECT 4.0f

/* Quad half-extent as a multiple of the screen's ground circumradius. */
#define GRID_EXTENT_SCALE 1.6f
/* Alpha fade radii as multiples of the same circumradius. The fade starts
 * beyond the visible footprint (>= 1) and ends strictly inside the quad
 * edge (GRID_FADE_END_SCALE < GRID_EXTENT_SCALE). */
#define GRID_FADE_START_SCALE 1.05f
#define GRID_FADE_END_SCALE 1.35f

typedef struct GridQuad {
	float corners[4][3];	/* world space, y = 0: BL, BR, TR, TL */
	float uv[4][2];		/* = the matching corner's world (x, z) */
	float centerX;		/* quad centre (world XZ), on the camera target */
	float centerZ;
	float halfExtent;	/* |corner - centre| (quad half-size) */
	float fadeStart;	/* full alpha inside this world-XZ radius */
	float fadeEnd;		/* alpha reaches 0 here; < halfExtent */
} GridQuad;

/* Build the grid quad for `camera` at `aspect` (width / height). See the
 * header note for the sizing and anchoring contract. */
void buildGridQuad(const Camera3D *camera, float aspect, GridQuad *out);

/* The orthographic projection to draw the grid quad with: the same x/y
 * mapping as cameraProjection(camera, aspect), but a symmetric depth range
 * that encloses the whole ground quad. At low zoom the camera's near plane
 * (CAMERA_NEAR) otherwise cuts a hard horizontal edge across the near ground
 * (the ground quad reaches toward the camera past it), which breaks the
 * "infinite grid" read; the grid pipeline has no depth test, so the wider
 * range is free. Multiply this by cameraView(camera) for the grid's
 * view-projection. A NULL camera returns the identity (as cameraProjection
 * does). */
Mat4 gridProjection(const Camera3D *camera, float aspect);

#endif /* ISOMATA_RENDER_GRID_H */
