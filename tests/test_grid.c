/*
 * Grid-quad tests (CTOL rung 1: unit + boundary).
 *
 * Pins the infinite ground grid's pure sizing/anchoring contract:
 *  - the quad covers the full visible ground footprint at every zoom in
 *    [CAMERA_ZOOM_MIN, CAMERA_ZOOM_MAX] and every aspect up to GRID_MAX_ASPECT
 *    (halfExtent reaches the screen's ground circumradius), for any yaw;
 *  - the alpha fade is off-screen at every zoom and completes strictly inside
 *    the quad edge (fadeStart/fadeEnd >= circumradius, fadeEnd < halfExtent);
 *  - UV == world XZ, so a fixed world point maps to the same UV no matter how
 *    the camera is panned/zoomed (the pattern cannot slide);
 *  - NULL camera / bad aspect / NULL out are safe.
 *
 * Pure: links only grid.c (+ camera3d/math3d) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_grid().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/grid.h"

#include <math.h>

#define PI_F 3.14159265358979323846f
#define EPS 1e-4f

/* The screen's ground circumradius, computed independently of grid.c: the
 * visible footprint is half-width H*aspect and half-length H/sin(pitch). */
static float screenGroundRadius(float zoom, float aspect)
{
	float h = CAMERA_BASE_HALF_HEIGHT / zoom;
	float w = h * aspect;
	float sp = sinf(CAMERA_DEFAULT_PITCH_DEG * (PI_F / 180.0f));
	float depth = h / sp;

	return sqrtf(w * w + depth * depth);
}

/* Put the camera at a known yaw, zoom and target. */
static void configureCamera(Camera3D *c, float yawDeg, float zoom, float tx,
			    float tz)
{
	int steps;

	initCamera3D(c);
	for (steps = 0; steps < (int)(yawDeg / CAMERA_STEP_DEG); steps++) {
		cameraRotateStep(c, 1);
		updateCamera3D(c, CAMERA_TURN_SECONDS);
	}
	cameraZoom(c, logf(zoom) / logf(CAMERA_ZOOM_STEP));
	cameraPan(c, tx, tz);	/* yaw 0: pan(x,y) -> target (x, -y) */
}

/* The core sizing property across the whole zoom/aspect sweep. */
static void test_covers_screen_across_zoom_and_aspect(void)
{
	static const float zooms[] = { CAMERA_ZOOM_MIN, 0.5f, 1.0f, 2.0f,
				       CAMERA_ZOOM_MAX };
	static const float aspects[] = { 1.0f, 16.0f / 9.0f, 4.0f };
	static const float yaws[] = { 0.0f, 45.0f, 90.0f, 135.0f, 315.0f };
	size_t zi;
	size_t ai;
	size_t yi;

	for (zi = 0; zi < sizeof(zooms) / sizeof(zooms[0]); zi++) {
		for (ai = 0; ai < sizeof(aspects) / sizeof(aspects[0]); ai++) {
			for (yi = 0; yi < sizeof(yaws) / sizeof(yaws[0]); yi++) {
				Camera3D c;
				GridQuad q;
				float r;

				configureCamera(&c, yaws[yi], zooms[zi], 0.0f,
						0.0f);
				buildGridQuad(&c, aspects[ai], &q);
				r = screenGroundRadius(zooms[zi], aspects[ai]);

				/* The quad reaches the footprint's circumradius... */
				TEST_ASSERT_TRUE(q.halfExtent >= r - EPS);
				/* ...its fade lives beyond the visible footprint... */
				TEST_ASSERT_TRUE(q.fadeStart >= r - EPS);
				TEST_ASSERT_TRUE(q.fadeEnd >= r - EPS);
				/* ...and completes strictly inside the quad edge. */
				TEST_ASSERT_TRUE(q.fadeStart < q.fadeEnd);
				TEST_ASSERT_TRUE(q.fadeEnd < q.halfExtent);
			}
		}
	}
}

/* The quad is a world-axis-aligned square centred on the camera target, with
 * UVs equal to world XZ. */
static void test_corners_centred_and_uv_is_world_xz(void)
{
	Camera3D c;
	GridQuad q;
	float tx = 99.0f;
	float tz = 99.0f;
	int i;

	configureCamera(&c, 0.0f, 1.0f, 8.0f, 8.0f);
	buildGridQuad(&c, 16.0f / 9.0f, &q);
	cameraTarget(&c, &tx, &tz);

	TEST_ASSERT_FLOAT_WITHIN(EPS, tx, q.centerX);
	TEST_ASSERT_FLOAT_WITHIN(EPS, tz, q.centerZ);

	/* Corners are the centre +- halfExtent on XZ, y = 0 (BL, BR, TR, TL). */
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerX - q.halfExtent, q.corners[0][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerZ - q.halfExtent, q.corners[0][2]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerX + q.halfExtent, q.corners[1][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerZ - q.halfExtent, q.corners[1][2]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerX + q.halfExtent, q.corners[2][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerZ + q.halfExtent, q.corners[2][2]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerX - q.halfExtent, q.corners[3][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q.centerZ + q.halfExtent, q.corners[3][2]);

	for (i = 0; i < 4; i++) {
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.corners[i][1]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, q.corners[i][0], q.uv[i][0]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, q.corners[i][2], q.uv[i][1]);
	}
}

/* A fixed world point must map to the same UV under any camera state: the
 * UVs are world coordinates, so the pattern is world-anchored and cannot
 * slide when the camera pans or zooms. */
static void test_uv_is_camera_independent_no_slide(void)
{
	Camera3D a;
	Camera3D b;
	GridQuad qa;
	GridQuad qb;
	/* A world point inside both quads. */
	float px = 4.0f;
	float pz = 6.0f;

	configureCamera(&a, 0.0f, 1.0f, 8.0f, 8.0f);
	configureCamera(&b, 90.0f, 2.0f, 2.0f, 3.0f);
	buildGridQuad(&a, 16.0f / 9.0f, &qa);
	buildGridQuad(&b, 1.0f, &qb);

	/* Bilinear UV interpolation at the world point, both quads. Both must
	 * give (px, pz) because UV == world XZ. */
	{
		float ta = (px - (qa.centerX - qa.halfExtent)) /
			   (2.0f * qa.halfExtent);
		float tz = (pz - (qa.centerZ - qa.halfExtent)) /
			   (2.0f * qa.halfExtent);
		float ux = qa.uv[0][0] * (1 - ta) * (1 - tz) +
			   qa.uv[1][0] * ta * (1 - tz) +
			   qa.uv[2][0] * ta * tz +
			   qa.uv[3][0] * (1 - ta) * tz;
		float uz = qa.uv[0][1] * (1 - ta) * (1 - tz) +
			   qa.uv[1][1] * ta * (1 - tz) +
			   qa.uv[2][1] * ta * tz +
			   qa.uv[3][1] * (1 - ta) * tz;

		TEST_ASSERT_TRUE(ta > 0.0f && ta < 1.0f);
		TEST_ASSERT_TRUE(tz > 0.0f && tz < 1.0f);
		TEST_ASSERT_FLOAT_WITHIN(EPS, px, ux);
		TEST_ASSERT_FLOAT_WITHIN(EPS, pz, uz);
	}
	{
		float ta = (px - (qb.centerX - qb.halfExtent)) /
			   (2.0f * qb.halfExtent);
		float tz = (pz - (qb.centerZ - qb.halfExtent)) /
			   (2.0f * qb.halfExtent);
		float ux = qb.uv[0][0] * (1 - ta) * (1 - tz) +
			   qb.uv[1][0] * ta * (1 - tz) +
			   qb.uv[2][0] * ta * tz +
			   qb.uv[3][0] * (1 - ta) * tz;
		float uz = qb.uv[0][1] * (1 - ta) * (1 - tz) +
			   qb.uv[1][1] * ta * (1 - tz) +
			   qb.uv[2][1] * ta * tz +
			   qb.uv[3][1] * (1 - ta) * tz;

		TEST_ASSERT_TRUE(ta > 0.0f && ta < 1.0f);
		TEST_ASSERT_TRUE(tz > 0.0f && tz < 1.0f);
		TEST_ASSERT_FLOAT_WITHIN(EPS, px, ux);
		TEST_ASSERT_FLOAT_WITHIN(EPS, pz, uz);
	}
}

/* The quad follows the camera target (so it is always under the view). */
static void test_centre_follows_target(void)
{
	Camera3D c;
	GridQuad q;

	configureCamera(&c, 0.0f, 1.0f, 12.0f, -5.0f);
	buildGridQuad(&c, 1.0f, &q);
	/* pan(x,y) at yaw 0 -> target (12, 5). */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 12.0f, q.centerX);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, q.centerZ);
}

/* NULL camera, NULL out and bad aspects are all safe. */
static void test_null_and_bad_aspect_safe(void)
{
	GridQuad q;
	Camera3D c;
	GridQuad q0;
	GridQuad qn;

	buildGridQuad(NULL, 1.0f, &q);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.centerX);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.centerZ);
	TEST_ASSERT_TRUE(q.fadeEnd < q.halfExtent);
	TEST_ASSERT_TRUE(q.halfExtent > 0.0f);

	buildGridQuad(NULL, 1.0f, NULL);	/* no crash */
	configureCamera(&c, 0.0f, 1.0f, 0.0f, 0.0f);
	buildGridQuad(&c, 0.0f, NULL);		/* no crash */

	/* aspect 0 and NaN behave as aspect 1. */
	buildGridQuad(&c, 0.0f, &q0);
	buildGridQuad(&c, (float)NAN, &qn);
	TEST_ASSERT_FLOAT_WITHIN(EPS, q0.halfExtent, qn.halfExtent);
	{
		GridQuad q1;

		buildGridQuad(&c, 1.0f, &q1);
		TEST_ASSERT_FLOAT_WITHIN(EPS, q1.halfExtent, q0.halfExtent);
	}
}

void run_test_grid(void);

void run_test_grid(void)
{
	RUN_TEST(test_covers_screen_across_zoom_and_aspect);
	RUN_TEST(test_corners_centred_and_uv_is_world_xz);
	RUN_TEST(test_uv_is_camera_independent_no_slide);
	RUN_TEST(test_centre_follows_target);
	RUN_TEST(test_null_and_bad_aspect_safe);
}
