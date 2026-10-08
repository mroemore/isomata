/*
 * Camera3D tests (CTOL rung 1: unit + boundary).
 *
 * Pins the isometric camera: the true-isometric default pitch, the
 * orthographic projection (exact entries at zoom 1 and zoom 2), 45-degree
 * step yaw tweening and mod-360 normalization, instant reset to the startup
 * state, zoom multiplicative steps with the [0.25, 4.0] clamp, and
 * screen-relative ground panning at yaw 0 and yaw 90 (the two axis-aligned
 * cases that fix the sign convention).
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

/* A step is 45 degrees; the macro is the single source of that angle. */
static void test_step_is_45_degrees(void)
{
	Camera3D c = freshCamera();

	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, CAMERA_STEP_DEG);
	cameraRotateStep(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, cameraYawDeg(&c));
}

/* A step is deferred (the tween starts at 0 and runs over
 * CAMERA_TURN_SECONDS); a full tween completes exactly at one step and the
 * exposed yaw is normalized mod 360 (eight steps = 0, nine steps = 45). */
static void test_step_completes_and_normalizes(void)
{
	Camera3D c = freshCamera();
	int i;

	cameraRotateStep(&c, 1);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));	/* deferred */
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, CAMERA_STEP_DEG * 0.5f,
				 cameraYawDeg(&c));
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, CAMERA_STEP_DEG, cameraYawDeg(&c));

	/* Seven more steps -> 45 + 7*45 = 360 -> 0. */
	for (i = 0; i < 7; i++) {
		cameraRotateStep(&c, 1);
		updateCamera3D(&c, CAMERA_TURN_SECONDS);
	}
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, cameraYawDeg(&c));

	/* A ninth step -> 45. */
	cameraRotateStep(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, CAMERA_STEP_DEG, cameraYawDeg(&c));

	/* Negative steps normalize into [0, 360). */
	cameraRotateStep(&c, -1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, cameraYawDeg(&c));

	/* A negative accumulated yaw wraps up into range (-45 -> 315). */
	{
		Camera3D d = freshCamera();

		cameraRotateStep(&d, -1);
		updateCamera3D(&d, CAMERA_TURN_SECONDS);
		TEST_ASSERT_FLOAT_WITHIN(1e-3f, 360.0f - CAMERA_STEP_DEG,
					 cameraYawDeg(&d));
	}

	/* An update that overshoots the duration clamps to the target. */
	{
		Camera3D e = freshCamera();

		cameraRotateStep(&e, 1);
		updateCamera3D(&e, CAMERA_TURN_SECONDS * 2.0f);
		TEST_ASSERT_FLOAT_WITHIN(1e-3f, CAMERA_STEP_DEG, cameraYawDeg(&e));
	}
}

/* A second step mid-tween composes onto the pending target: the final yaw
 * is 90, not 45. */
static void test_step_composes_during_tween(void)
{
	Camera3D c = freshCamera();

	cameraRotateStep(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);	/* at 22.5 */
	cameraRotateStep(&c, 1);			/* target 90 */
	updateCamera3D(&c, CAMERA_TURN_SECONDS);	/* finish */
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));

	/* An idle update is a no-op. */
	updateCamera3D(&c, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));

	/* A negative dt clamps to the tween start (never rewinds past it). */
	cameraRotateStep(&c, 1);		/* target 135 */
	updateCamera3D(&c, -5.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 135.0f, cameraYawDeg(&c));

	/* A zero-direction step records nothing. */
	cameraRotateStep(&c, 0);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 135.0f, cameraYawDeg(&c));
}

/* Reset is instant and restores the exact startup state (yaw 0, default
 * zoom, target origin, no tween pending). */
static void test_reset_restores_startup_state(void)
{
	Camera3D c = freshCamera();
	float x = 99.0f;
	float z = 99.0f;

	cameraRotateStep(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	cameraZoom(&c, 2.0f);
	cameraPan(&c, 5.0f, 6.0f);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, cameraYawDeg(&c));

	cameraReset(&c);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_ZOOM_DEFAULT, cameraZoomLevel(&c));
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);

	/* No tween is pending: an update does not move the yaw. */
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));
}

/* Reset mid-tween cancels the pending target (it does not land on it). */
static void test_reset_cancels_pending_tween(void)
{
	Camera3D c = freshCamera();

	cameraRotateStep(&c, 1);		/* target 45, not yet applied */
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	cameraReset(&c);
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 2.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));
}

/* cameraYawTargetDeg reports the yaw a step is heading to before the tween
 * runs, and the resting yaw once it has. */
static void test_yaw_target_accessor(void)
{
	Camera3D c = freshCamera();

	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawTargetDeg(&c));
	cameraRotateStep(&c, 1);
	/* Deferred: the current yaw is still 0, the target is 45. */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(&c));
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, cameraYawTargetDeg(&c));
	/* A second step mid-tween composes the target to 90. */
	updateCamera3D(&c, CAMERA_TURN_SECONDS * 0.5f);
	cameraRotateStep(&c, 1);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawTargetDeg(&c));
	/* Completed tween: target == current yaw. */
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawTargetDeg(&c));
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, cameraYawDeg(&c));
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

	cameraRotateStep(&c, 1);
	updateCamera3D(&c, CAMERA_TURN_SECONDS);
	cameraRotateStep(&c, 1);
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

/* cameraPanByDrag: the camera follows the drag, the INVERSE of the raw
 * cameraPan sign. At yaw 0 dragging right (dx > 0) moves the view right
 * (target +X) and dragging down (dy > 0) moves the view toward the viewer
 * (target +Z). The raw cameraPan(x, y) moves the target (+x, -y) at yaw 0,
 * so the helper composes the drag sign on top:
 * cameraPanByDrag(dx, dy) == cameraPan(dx * K, -dy * K). */
static void test_pan_by_drag_follows_pointer_at_yaw_zero(void)
{
	Camera3D c = freshCamera();
	float x = 0.0f;
	float z = 0.0f;

	/* The mapping constant is pinned (reduced from the old 0.05). */
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.04f, CAMERA_PAN_PER_PIXEL);

	cameraPanByDrag(&c, 1, 0);	/* drag right -> view right (+X) */
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_PAN_PER_PIXEL, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);

	cameraPanByDrag(&c, -1, 0);	/* drag left: undoes it */
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);

	cameraPanByDrag(&c, 0, 1);	/* drag down -> toward viewer (+Z) */
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, CAMERA_PAN_PER_PIXEL, z);

	cameraPanByDrag(&c, 0, -1);	/* drag up: undoes it */
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);
}

/* Magnitude: the target delta is exactly the virtual delta times the pinned
 * per-pixel constant (no rounding, no clamp). */
static void test_pan_by_drag_magnitude(void)
{
	Camera3D c = freshCamera();
	float x = 0.0f;
	float z = 0.0f;

	cameraPanByDrag(&c, 10, 5);
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f * CAMERA_PAN_PER_PIXEL, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f * CAMERA_PAN_PER_PIXEL, z);
}

/* The helper is exactly the raw pan with the drag sign composed on top
 * (pins the yaw-0 axis convention the level's centring comment relies on). */
static void test_pan_by_drag_composes_raw_pan(void)
{
	Camera3D drag = freshCamera();
	Camera3D raw = freshCamera();
	float dx = 0.0f;
	float dz = 0.0f;
	float rx = 0.0f;
	float rz = 0.0f;

	cameraPanByDrag(&drag, 7, -3);
	cameraPan(&raw, 7.0f * CAMERA_PAN_PER_PIXEL,
		  -(-3.0f) * CAMERA_PAN_PER_PIXEL);
	cameraTarget(&drag, &dx, &dz);
	cameraTarget(&raw, &rx, &rz);
	TEST_ASSERT_FLOAT_WITHIN(EPS, rx, dx);
	TEST_ASSERT_FLOAT_WITHIN(EPS, rz, dz);
}

/* NULL camera and zero deltas are no-ops. */
static void test_pan_by_drag_noop_cases(void)
{
	Camera3D c = freshCamera();
	float x = 9.0f;
	float z = 9.0f;

	cameraPanByDrag(NULL, 5, 5);
	cameraPanByDrag(&c, 0, 0);
	cameraTarget(&c, &x, &z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, z);
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
	cameraRotateStep(NULL, 1);
	cameraReset(NULL);
	updateCamera3D(NULL, 1.0f);
	cameraZoom(NULL, 1.0f);
	cameraPan(NULL, 1.0f, 1.0f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawDeg(NULL));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cameraYawTargetDeg(NULL));
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
	RUN_TEST(test_step_is_45_degrees);
	RUN_TEST(test_step_completes_and_normalizes);
	RUN_TEST(test_step_composes_during_tween);
	RUN_TEST(test_reset_restores_startup_state);
	RUN_TEST(test_reset_cancels_pending_tween);
	RUN_TEST(test_yaw_target_accessor);
	RUN_TEST(test_target_getters_partial);
	RUN_TEST(test_zoom_multiplicative_and_clamped);
	RUN_TEST(test_pan_at_yaw_zero);
	RUN_TEST(test_pan_at_yaw_ninety);
	RUN_TEST(test_pan_by_drag_follows_pointer_at_yaw_zero);
	RUN_TEST(test_pan_by_drag_magnitude);
	RUN_TEST(test_pan_by_drag_composes_raw_pan);
	RUN_TEST(test_pan_by_drag_noop_cases);
	RUN_TEST(test_projection_ortho_entries);
	RUN_TEST(test_projection_zoom_and_aspect_guard);
	RUN_TEST(test_view_places_target_on_negative_z_axis);
	RUN_TEST(test_world_to_clip_composition);
	RUN_TEST(test_null_arguments_are_safe);
}
