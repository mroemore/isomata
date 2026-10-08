/*
 * Camera3D tests (CTOL rung 1: unit + boundary).
 *
 * Pins the isometric camera: the true-isometric default pitch, the
 * orthographic projection (exact entries at zoom 1 and zoom 2), quarter-
 * turn yaw tweening and mod-360 normalization, zoom multiplicative steps
 * with the [0.25, 4.0] clamp, and screen-relative ground panning at yaw 0
 * and yaw 90 (the two axis-aligned cases that fix the sign convention).
 *
 * Pure: links only camera3d.c/math3d.c plus the Unity subset; no SDL.
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_camera().
 */

#include "unity.h"

#include "render/camera3d.h"

#include <math.h>

#define EPS 1e-4f

static Camera3D freshCamera(void)
{
	Camera3D c;

	initCamera3D(&c);
	return c;
}

/* Defaults: yaw 0, zoom 1, target at the origin, and the pitch macro is the
 * true-isometric angle atan(1/sqrt(2)) in degrees (35.264...). The pitch is
 * pinned three ways so changing CAMERA_DEFAULT_PITCH_DEG fails the suite:
 * the macro vs the literal, the macro vs atan(1/sqrt 2), and an observable
 * camera-state entry derived from it. */
static void test_defaults_are_true_isometric(void)
{
	Camera3D c = freshCamera();
	float x = 99.0f;
	float z = 99.0f;
	Mat4 view;

	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, cameraZoomLevel(&c));
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);

	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 35.264f, CAMERA_DEFAULT_PITCH_DEG);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f,
				 atanf(1.0f / sqrtf(2.0f)) *
				 (180.0f / 3.14159265358979323846f),
				 CAMERA_DEFAULT_PITCH_DEG);
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_DEFAULT_PITCH_DEG,
				 cameraPitchDeg(&c));

	/* The default view's m[6] = -f.y = sin(pitch) and m[5] = u.y =
	 * cos(pitch). Hardcoded sin/cos(35.264 deg) so a macro change to any
	 * other pitch fails here even though cameraPitchDeg would merely echo
	 * the changed constant. */
	view = cameraView(&c);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.57735f, view.m[6]);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.81650f, view.m[5]);
}

/* A quarter turn is deferred (the tween starts at 0 and runs over
 * CAMERA_TURN_SECONDS); a full tween completes exactly at 90 and the
 * exposed yaw is normalized mod 360 (four turns = 0, five turns = 90). */
static void test_quarter_turn_completes_and_normalizes(void)
{
	Camera3D c = freshCamera();

	cameraRotateQuarterTurn(&c, 1);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));	/* deferred */
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, cameraYawDeg(&c));
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));

	/* Three more turns -> 360 -> 0. */
	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, cameraYawDeg(&c));

	/* A fifth turn -> 450 -> 90. */
	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));

	/* Negative turns normalize into [0, 360). */
	cameraRotateQuarterTurn(&c, -1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, cameraYawDeg(&c));

	/* A negative accumulated yaw wraps up into range (-90 -> 270). */
	{
		Camera3D d = freshCamera();

		cameraRotateQuarterTurn(&d, -1);
		updateCamera3D(&d, CAMERA_TURN_SECONDS);
		TEST_ASSERT_FLOAT_WITHIN(1e-3f, 270.0f, cameraYawDeg(&d));
	}

	/* An update that overshoots the duration clamps to the target. */
	{
		Camera3D e = freshCamera();

		cameraRotateQuarterTurn(&e, 1);
		updateCamera3D(&e, CAMERA_TURN_SECONDS * 2.0f);
		TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&e));
	}
}

/* A second quarter-turn mid-tween composes onto the pending target: the
 * final yaw is 180, not 90. */
static void test_quarter_turn_composes_during_tween(void)
{
	Camera3D c = freshCamera();

	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);	/* at 45 */
	cameraRotateQuarterTurn(&c, 1);			/* target 180 */
	updateCamera3D(&c, CAMERA_TURN_SECONDS);	/* finish */
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, cameraYawDeg(&c));

	/* An idle update is a no-op. */
	updateCamera3D(&c, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, cameraYawDeg(&c));

	/* A negative dt clamps to the tween start (never rewinds past it). */
	cameraRotateQuarterTurn(&c, 1);		/* target 270 */
	updateCamera3D(&c, -5.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, cameraYawDeg(&c));
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 270.0f, cameraYawDeg(&c));

	/* A zero-direction turn records nothing. */
	cameraRotateQuarterTurn(&c, 0);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 270.0f, cameraYawDeg(&c));
}

/* cameraTarget fills each out pointer independently. */
static void test_target_getters_partial(void)
{
	Camera3D c = freshCamera();
	float x = 0.0f;
	float z = 0.0f;

	cameraPan(&c, 3.0f, 4.0f);		/* yaw 0: target (3, -4) */
	cameraTarget(&c, &x, NULL);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, x);
	cameraTarget(&c, NULL, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -4.0f, z);
}

/* Zoom is multiplicative (each +1 step multiplies by CAMERA_ZOOM_STEP) and
 * clamped to [0.25, 4.0] in both directions. */
static void test_zoom_multiplicative_and_clamped(void)
{
	Camera3D c = freshCamera();

	cameraZoom(&c, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, CAMERA_ZOOM_STEP, cameraZoomLevel(&c));
	cameraZoom(&c, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, CAMERA_ZOOM_STEP * CAMERA_ZOOM_STEP,
				 cameraZoomLevel(&c));

	cameraZoom(&c, 100.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_ZOOM_MAX, cameraZoomLevel(&c));
	cameraZoom(&c, -100.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_ZOOM_MIN, cameraZoomLevel(&c));
}

/* Pan is screen-relative on the ground plane. At yaw 0 screen-right is +X
 * and screen-forward is -Z, so pan(2,3) moves the target to (2,-3). */
static void test_pan_at_yaw_zero(void)
{
	Camera3D c = freshCamera();
	float x = 0.0f;
	float z = 0.0f;

	cameraPan(&c, 2.0f, 3.0f);
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -3.0f, z);
}

/* At yaw 90 screen-right is -Z and screen-forward is -X: pan(1,0) moves
 * z by -1, pan(0,1) moves x by -1. */
static void test_pan_at_yaw_ninety(void)
{
	Camera3D c = freshCamera();
	float x = 0.0f;
	float z = 0.0f;

	cameraRotateQuarterTurn(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));

	cameraPan(&c, 1.0f, 0.0f);
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, z);

	cameraPan(&c, 0.0f, 1.0f);
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, z);
}

/* Projection is orthographic with half-height BASE/zoom and half-width
 * scaled by aspect: at zoom 1, aspect 2 -> ortho(-20,20,-10,10,1,100). */
static void test_projection_ortho_entries(void)
{
	Camera3D c = freshCamera();
	Mat4 p = cameraProjection(&c, 2.0f);

	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f / 40.0f, p.m[0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f / 20.0f, p.m[5]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f / 99.0f, p.m[10]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f / 99.0f, p.m[14]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, p.m[15]);
}

/* Zoom magnifies: at zoom 2 the visible half-height halves, so the y scale
 * doubles. A non-positive aspect falls back to 1.0. */
static void test_projection_zoom_and_aspect_guard(void)
{
	Camera3D c = freshCamera();
	Mat4 p;
	Mat4 q;

	cameraZoom(&c, 100.0f);			/* clamp to 4.0 */
	p = cameraProjection(&c, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f / (2.0f * (CAMERA_BASE_HALF_HEIGHT / 4.0f)),
				 p.m[5]);

	/* aspect 0 (and NaN) -> treated as 1.0, so halfW == halfH and the
	 * x scale equals the y scale. */
	q = cameraProjection(&c, 0.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, p.m[5], q.m[0]);
	{
		Mat4 r = cameraProjection(&c, (float)NAN);
		TEST_ASSERT_FLOAT_WITHIN(EPS, q.m[0], r.m[0]);
	}
}

/* View: the default camera looks down -Z from distance CAMERA_DISTANCE, so
 * the target maps to (0,0,-CAMERA_DISTANCE) and world +X is screen-right. */
static void test_view_places_target_on_negative_z_axis(void)
{
	Camera3D c = freshCamera();
	Mat4 v = cameraView(&c);
	Vec4 t = mat4TransformPoint(&v, (Vec3){ 0.0f, 0.0f, 0.0f });
	Vec4 right = mat4TransformPoint(&v, (Vec3){ 1.0f, 0.0f, 0.0f });

	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.y);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, -CAMERA_DISTANCE, t.z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, right.x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, right.y);
}

/* World->clip through the camera: the target projects to the center of
 * NDC, inside the [-1,1] cube. */
static void test_world_to_clip_composition(void)
{
	Camera3D c = freshCamera();
	Mat4 p = cameraProjection(&c, 16.0f / 9.0f);
	Mat4 v = cameraView(&c);
	Mat4 m = mat4Multiply(&p, &v);
	Vec4 clip = mat4TransformPoint(&m, (Vec3){ 0.0f, 0.0f, 0.0f });

	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, clip.x);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, clip.y);
	TEST_ASSERT_TRUE(clip.z >= 0.0f && clip.z <= 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, clip.w);
}

/* Every endpoint tolerates NULL. */
static void test_null_arguments_are_safe(void)
{
	Mat4 id = mat4Identity();
	Mat4 p;
	Mat4 v;

	initCamera3D(NULL);
	cameraRotateQuarterTurn(NULL, 1);
	updateCamera3D(NULL, 1.0f);
	cameraZoom(NULL, 1.0f);
	cameraPan(NULL, 1.0f, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(NULL));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraZoomLevel(NULL));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraPitchDeg(NULL));
	cameraTarget(NULL, NULL, NULL);

	p = cameraProjection(NULL, 1.0f);
	v = cameraView(NULL);
	TEST_ASSERT_EQUAL_MEMORY(&id, &p, sizeof(Mat4));
	TEST_ASSERT_EQUAL_MEMORY(&id, &v, sizeof(Mat4));
}

/* --- runner ----------------------------------------------------------- */

void run_test_camera(void);

void run_test_camera(void)
{
	RUN_TEST(test_defaults_are_true_isometric);
	RUN_TEST(test_quarter_turn_completes_and_normalizes);
	RUN_TEST(test_quarter_turn_composes_during_tween);
	RUN_TEST(test_target_getters_partial);
	RUN_TEST(test_zoom_multiplicative_and_clamped);
	RUN_TEST(test_pan_at_yaw_zero);
	RUN_TEST(test_pan_at_yaw_ninety);
	RUN_TEST(test_projection_ortho_entries);
	RUN_TEST(test_projection_zoom_and_aspect_guard);
	RUN_TEST(test_view_places_target_on_negative_z_axis);
	RUN_TEST(test_world_to_clip_composition);
	RUN_TEST(test_null_arguments_are_safe);
}
