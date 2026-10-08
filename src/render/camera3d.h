#ifndef ISOMATA_RENDER_CAMERA3D_H
#define ISOMATA_RENDER_CAMERA3D_H

/*
 * Isometric 3D camera for the render tier. Pure: no SDL, and it depends
 * only on math3d.h.
 *
 * Model:
 * - The camera orbits a ground-plane look-at point (targetX, targetZ, 0)
 *   at a fixed true-isometric pitch of CAMERA_DEFAULT_PITCH_DEG, from
 *   CAMERA_DISTANCE away. `yaw` rotates that orbit about +Y; a step is
 *   CAMERA_STEP_DEG (45 degrees), tweened over CAMERA_TURN_SECONDS and
 *   composing while one is in flight. The public yaw is normalized to
 *   [0, 360) and rests at 45-degree multiples.
 * - cameraProjection is orthographic: the visible half-height is
 *   CAMERA_BASE_HALF_HEIGHT / zoom, the half-width follows the aspect.
 *   Larger zoom = more magnified (a smaller view volume).
 * - cameraZoom is multiplicative: `amount` is in wheel steps, each step
 *   multiplying the zoom by CAMERA_ZOOM_STEP, clamped to
 *   [CAMERA_ZOOM_MIN, CAMERA_ZOOM_MAX].
 * - cameraPan takes screen-relative ground-plane deltas: `x` moves the
 *   target along the camera's screen-right axis, `y` along the camera's
 *   forward axis (the ground projection of the view direction, i.e. into
 *   the screen). At yaw 0 right is +X and forward is -Z; at yaw 90 right
 *   is -Z and forward is -X (both pinned by tests).
 *
 * Struct fields are private to camera3d.c; callers use initCamera3D and
 * the accessors. The struct is defined here only so callers can hold one
 * by value (like scene.h's Scene).
 */

#include "render/math3d.h"

#define CAMERA_DEFAULT_PITCH_DEG 35.264f	/* true isometric: atan(1/sqrt 2) */
#define CAMERA_STEP_DEG 45.0f			/* one rotate press */
#define CAMERA_TURN_SECONDS 0.25f		/* one 45-degree step tween */
#define CAMERA_ZOOM_MIN 0.25f
#define CAMERA_ZOOM_MAX 4.0f
#define CAMERA_ZOOM_DEFAULT 1.0f
#define CAMERA_ZOOM_STEP 1.25f			/* per wheel step, multiplicative */
#define CAMERA_DISTANCE 20.0f			/* eye offset from the target */
#define CAMERA_BASE_HALF_HEIGHT 10.0f		/* world half-height at zoom 1 */
#define CAMERA_NEAR 1.0f
#define CAMERA_FAR 100.0f

typedef struct Camera3D {
	/* Private state. */
	float yawDeg;		/* current yaw (unbounded during a tween) */
	float yawFromDeg;	/* tween start yaw */
	float yawTargetDeg;	/* tween target yaw (composes on repeat) */
	float tweenElapsed;	/* seconds into the tween */
	float tweenDuration;	/* 0 = idle */
	float zoom;
	float targetX;
	float targetZ;
} Camera3D;

/* Initialize to yaw 0, zoom CAMERA_ZOOM_DEFAULT, target at the origin (the
 * startup state cameraReset restores). NULL is a no-op. */
void initCamera3D(Camera3D *camera);

/* Record a rotate step of CAMERA_STEP_DEG: direction > 0 is +45 degrees,
 * < 0 is -45, 0 is a no-op. Calls during a tween compose onto the pending
 * target. */
void cameraRotateStep(Camera3D *camera, int direction);

/* Instantly restore the startup state: yaw 0, zoom CAMERA_ZOOM_DEFAULT,
 * target at the origin, no tween pending. No tween is started (unlike
 * cameraRotateStep); NULL is a no-op. */
void cameraReset(Camera3D *camera);

/* Advance the yaw tween by dt seconds. Idle when no tween is pending. */
void updateCamera3D(Camera3D *camera, float dt);

/* Multiply the zoom by CAMERA_ZOOM_STEP^amount, clamped. */
void cameraZoom(Camera3D *camera, float amount);

/* Move the look-at target by screen-relative ground deltas (see the model
 * note above). */
void cameraPan(Camera3D *camera, float x, float y);

/* Orthographic view-projection source matrices. A NULL camera yields the
 * identity (so a degenerate camera never produces NaNs). */
Mat4 cameraProjection(const Camera3D *camera, float aspect);
Mat4 cameraView(const Camera3D *camera);

/* Accessors for tests and later tasks. cameraYawDeg is normalized to
 * [0, 360); cameraZoomLevel returns the current zoom; cameraPitchDeg returns
 * the fixed CAMERA_DEFAULT_PITCH_DEG (0 for a NULL camera); cameraTarget
 * writes the ground-plane look-at point through the out pointers (NULL skips
 * one). cameraYawTargetDeg is the yaw the camera is heading to (the pending
 * tween target while one is in flight, otherwise the current yaw), also
 * normalized to [0, 360) — 0 for a NULL camera. */
float cameraYawDeg(const Camera3D *camera);
float cameraYawTargetDeg(const Camera3D *camera);
float cameraZoomLevel(const Camera3D *camera);
float cameraPitchDeg(const Camera3D *camera);
void cameraTarget(const Camera3D *camera, float *x, float *z);

#endif /* ISOMATA_RENDER_CAMERA3D_H */
