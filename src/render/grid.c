/*
 * Infinite ground grid (see grid.h for the sizing/anchoring contract).
 *
 * Pure: camera3d + math3d accessors + libm only. The quad is a world-axis-
 * aligned square, so it needs no yaw and covers the rotated ground footprint
 * for every camera yaw.
 */

#include "render/grid.h"

#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265358979323846f

void buildGridQuad(const Camera3D *camera, float aspect, GridQuad *out)
{
	float zoom = CAMERA_ZOOM_DEFAULT;
	float halfHeight;
	float halfWidth;
	float depthHalf;
	float radius;
	float halfExtent;
	float tx = 0.0f;
	float tz = 0.0f;
	float sp;
	int i;

	if (out == NULL)
		return;

	if (camera != NULL)
		zoom = cameraZoomLevel(camera);
	if (!(zoom > 0.0f))
		zoom = CAMERA_ZOOM_DEFAULT;
	if (!(aspect > 0.0f))
		aspect = 1.0f;		/* guard <= 0 and NaN */

	/* Visible ground footprint: half-width H*aspect, half-length
	 * H/sin(pitch) (the orthographic half-height projects onto the ground
	 * foreshortened by sin(pitch)). A square whose inscribed circle reaches
	 * the footprint's circumradius contains it for any yaw. */
	halfHeight = CAMERA_BASE_HALF_HEIGHT / zoom;
	halfWidth = halfHeight * aspect;
	sp = sinf(CAMERA_DEFAULT_PITCH_DEG * (PI_F / 180.0f));
	depthHalf = halfHeight / sp;
	radius = sqrtf(halfWidth * halfWidth + depthHalf * depthHalf);
	halfExtent = radius * GRID_EXTENT_SCALE;

	cameraTarget(camera, &tx, &tz);	/* NULL camera leaves the defaults */

	out->centerX = tx;
	out->centerZ = tz;
	out->halfExtent = halfExtent;
	out->fadeStart = radius * GRID_FADE_START_SCALE;
	out->fadeEnd = radius * GRID_FADE_END_SCALE;

	/* Canonical corner order BL, BR, TR, TL (XZ ground plane). */
	out->corners[0][0] = tx - halfExtent;
	out->corners[0][1] = 0.0f;
	out->corners[0][2] = tz - halfExtent;
	out->corners[1][0] = tx + halfExtent;
	out->corners[1][1] = 0.0f;
	out->corners[1][2] = tz - halfExtent;
	out->corners[2][0] = tx + halfExtent;
	out->corners[2][1] = 0.0f;
	out->corners[2][2] = tz + halfExtent;
	out->corners[3][0] = tx - halfExtent;
	out->corners[3][1] = 0.0f;
	out->corners[3][2] = tz + halfExtent;

	/* UVs are the world XZ of the matching corner: world-anchored lines. */
	for (i = 0; i < 4; i++) {
		out->uv[i][0] = out->corners[i][0];
		out->uv[i][1] = out->corners[i][2];
	}
}
