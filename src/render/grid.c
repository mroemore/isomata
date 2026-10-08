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

/* The screen's ground circumradius for a zoom and aspect: the visible ground
 * footprint is half-width H*aspect and half-length H/sin(pitch), where
 * H = CAMERA_BASE_HALF_HEIGHT / zoom. */
static float gridRadius(float zoom, float aspect)
{
	float halfHeight = CAMERA_BASE_HALF_HEIGHT / zoom;
	float halfWidth = halfHeight * aspect;
	float sp = sinf(CAMERA_DEFAULT_PITCH_DEG * (PI_F / 180.0f));
	float depthHalf = halfHeight / sp;

	return sqrtf(halfWidth * halfWidth + depthHalf * depthHalf);
}

/* Normalize the zoom/aspect inputs (shared guards). */
static void gridInputs(const Camera3D *camera, float *aspect, float *zoom)
{
	if (camera != NULL)
		*zoom = cameraZoomLevel(camera);
	if (!(*zoom > 0.0f))
		*zoom = CAMERA_ZOOM_DEFAULT;
	if (!(*aspect > 0.0f))
		*aspect = 1.0f;		/* guard <= 0 and NaN */
}

void buildGridQuad(const Camera3D *camera, float aspect, GridQuad *out)
{
	float zoom = CAMERA_ZOOM_DEFAULT;
	float radius;
	float halfExtent;
	float tx = 0.0f;
	float tz = 0.0f;
	int i;

	if (out == NULL)
		return;

	gridInputs(camera, &aspect, &zoom);

	radius = gridRadius(zoom, aspect);
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

Mat4 gridProjection(const Camera3D *camera, float aspect)
{
	float zoom = CAMERA_ZOOM_DEFAULT;
	float halfHeight;
	float halfWidth;
	float halfExtent;
	float depth;
	float cp;

	if (camera == NULL)
		return cameraProjection(NULL, aspect);	/* identity */

	gridInputs(camera, &aspect, &zoom);
	halfHeight = CAMERA_BASE_HALF_HEIGHT / zoom;
	halfWidth = halfHeight * aspect;
	halfExtent = gridRadius(zoom, aspect) * GRID_EXTENT_SCALE;

	/* The quad's most distant corner sits at |cz| = CAMERA_DISTANCE +
	 * cos(pitch)*halfExtent*sqrt(2) along the view axis; a symmetric depth
	 * range of that radius keeps every ground vertex in [0, 1]. */
	cp = cosf(CAMERA_DEFAULT_PITCH_DEG * (PI_F / 180.0f));
	depth = CAMERA_DISTANCE + cp * halfExtent * 1.41421356f + 1.0f;

	return mat4Ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
			 -depth, depth);
}
