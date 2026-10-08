/*
 * Camera3D (see camera3d.h for the model and conventions). Pure: math3d +
 * libm only.
 */

#include "render/camera3d.h"

#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265358979323846f

static float degToRad(float deg)
{
	return deg * (PI_F / 180.0f);
}

/* Wrap an unbounded yaw into [0, 360). */
static float normalizeYaw(float deg)
{
	float y = fmodf(deg, 360.0f);

	if (y < 0.0f)
		y += 360.0f;
	return y;
}

void initCamera3D(Camera3D *camera)
{
	if (camera == NULL)
		return;
	camera->yawDeg = 0.0f;
	camera->yawFromDeg = 0.0f;
	camera->yawTargetDeg = 0.0f;
	camera->tweenElapsed = 0.0f;
	camera->tweenDuration = 0.0f;
	camera->zoom = CAMERA_ZOOM_DEFAULT;
	camera->targetX = 0.0f;
	camera->targetZ = 0.0f;
}

void cameraRotateQuarterTurn(Camera3D *camera, int direction)
{
	if (camera == NULL || direction == 0)
		return;
	/* Accumulate onto the pending target so a second call mid-tween
	 * composes, then re-anchor the interpolation from where the yaw
	 * currently is. */
	camera->yawTargetDeg += direction > 0 ? 90.0f : -90.0f;
	camera->yawFromDeg = camera->yawDeg;
	camera->tweenElapsed = 0.0f;
	camera->tweenDuration = CAMERA_TURN_SECONDS;
}

void updateCamera3D(Camera3D *camera, float dt)
{
	float t;

	if (camera == NULL || camera->tweenDuration <= 0.0f)
		return;
	camera->tweenElapsed += dt;
	if (camera->tweenElapsed < 0.0f)
		camera->tweenElapsed = 0.0f;	/* a negative dt never rewinds */
	t = camera->tweenElapsed / camera->tweenDuration;
	if (t > 1.0f)
		t = 1.0f;
	camera->yawDeg = camera->yawFromDeg +
			 (camera->yawTargetDeg - camera->yawFromDeg) * t;
	if (t >= 1.0f) {
		camera->yawDeg = camera->yawTargetDeg;
		camera->tweenElapsed = 0.0f;
		camera->tweenDuration = 0.0f;
	}
}

void cameraZoom(Camera3D *camera, float amount)
{
	float zoom;

	if (camera == NULL)
		return;
	zoom = camera->zoom * powf(CAMERA_ZOOM_STEP, amount);
	if (zoom < CAMERA_ZOOM_MIN)
		zoom = CAMERA_ZOOM_MIN;
	if (zoom > CAMERA_ZOOM_MAX)
		zoom = CAMERA_ZOOM_MAX;
	camera->zoom = zoom;
}

void cameraPan(Camera3D *camera, float x, float y)
{
	float yawRad;
	float c;
	float s;

	if (camera == NULL)
		return;
	yawRad = degToRad(camera->yawDeg);
	c = cosf(yawRad);
	s = sinf(yawRad);
	/* screen-right = (c, 0, -s); screen-forward = (-s, 0, -c) */
	camera->targetX += x * c - y * s;
	camera->targetZ += -x * s - y * c;
}

Mat4 cameraProjection(const Camera3D *camera, float aspect)
{
	float halfHeight;
	float halfWidth;

	if (camera == NULL)
		return mat4Identity();
	if (!(aspect > 0.0f))
		aspect = 1.0f;	/* guard <= 0 and NaN */
	halfHeight = CAMERA_BASE_HALF_HEIGHT / camera->zoom;
	halfWidth = halfHeight * aspect;
	return mat4Ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
			 CAMERA_NEAR, CAMERA_FAR);
}

Mat4 cameraView(const Camera3D *camera)
{
	float yawRad;
	float pitchRad;
	float cp;
	float sp;
	Vec3 eye;
	Vec3 target = { 0.0f, 0.0f, 0.0f };
	Vec3 up = { 0.0f, 1.0f, 0.0f };

	if (camera == NULL)
		return mat4Identity();
	yawRad = degToRad(camera->yawDeg);
	pitchRad = degToRad(CAMERA_DEFAULT_PITCH_DEG);
	cp = cosf(pitchRad);
	sp = sinf(pitchRad);
	eye.x = camera->targetX + sinf(yawRad) * cp * CAMERA_DISTANCE;
	eye.y = sp * CAMERA_DISTANCE;
	eye.z = camera->targetZ + cosf(yawRad) * cp * CAMERA_DISTANCE;
	target.x = camera->targetX;
	target.z = camera->targetZ;
	return mat4LookAt(eye, target, up);
}

float cameraYawDeg(const Camera3D *camera)
{
	if (camera == NULL)
		return 0.0f;
	return normalizeYaw(camera->yawDeg);
}

float cameraZoomLevel(const Camera3D *camera)
{
	if (camera == NULL)
		return 0.0f;
	return camera->zoom;
}

float cameraPitchDeg(const Camera3D *camera)
{
	if (camera == NULL)
		return 0.0f;
	return CAMERA_DEFAULT_PITCH_DEG;
}

void cameraTarget(const Camera3D *camera, float *x, float *z)
{
	if (camera == NULL)
		return;
	if (x != NULL)
		*x = camera->targetX;
	if (z != NULL)
		*z = camera->targetZ;
}
