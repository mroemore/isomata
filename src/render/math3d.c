/*
 * math3d (see math3d.h for the column-major memory convention). Plain
 * arithmetic; no branches, so every function is fully exercised by the
 * suite's straight-line calls.
 */

#include "render/math3d.h"

#include <math.h>

Mat4 mat4Identity(void)
{
	Mat4 m = { { 0 } };

	m.m[0] = 1.0f;
	m.m[5] = 1.0f;
	m.m[10] = 1.0f;
	m.m[15] = 1.0f;
	return m;
}

/* Column-major product: (A*B)[c*4+r] = sum_k A[k*4+r] * B[c*4+k]. */
Mat4 mat4Multiply(const Mat4 *a, const Mat4 *b)
{
	Mat4 out = { { 0 } };
	int c;
	int r;

	for (c = 0; c < 4; c++) {
		for (r = 0; r < 4; r++) {
			float sum = 0.0f;
			int k;

			for (k = 0; k < 4; k++)
				sum += a->m[k * 4 + r] * b->m[c * 4 + k];
			out.m[c * 4 + r] = sum;
		}
	}
	return out;
}

Mat4 mat4Translate(float x, float y, float z)
{
	Mat4 m = mat4Identity();

	m.m[12] = x;
	m.m[13] = y;
	m.m[14] = z;
	return m;
}

Mat4 mat4Ortho(float left, float right, float bottom, float top, float near,
	       float far)
{
	Mat4 m = { { 0 } };

	m.m[0] = 2.0f / (right - left);
	m.m[5] = 2.0f / (top - bottom);
	/* Vulkan depth range: near -> 0, far -> 1. */
	m.m[10] = -1.0f / (far - near);
	m.m[12] = -(right + left) / (right - left);
	m.m[13] = -(top + bottom) / (top - bottom);
	m.m[14] = -near / (far - near);
	m.m[15] = 1.0f;
	return m;
}

Mat4 mat4LookAt(Vec3 eye, Vec3 target, Vec3 up)
{
	/* Forward points from eye to target; right and true-up complete the
	 * basis. The view matrix's rows are that basis (columns in our
	 * column-major storage), with the translation derived from the eye. */
	Vec3 f = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
	float fLen = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
	Vec3 s;
	float sLen;
	Vec3 u;
	Mat4 m = { { 0 } };

	f.x /= fLen;
	f.y /= fLen;
	f.z /= fLen;

	/* right = normalize(forward x up) */
	s.x = f.y * up.z - f.z * up.y;
	s.y = f.z * up.x - f.x * up.z;
	s.z = f.x * up.y - f.y * up.x;
	sLen = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z);
	s.x /= sLen;
	s.y /= sLen;
	s.z /= sLen;

	/* true up = right x forward */
	u.x = s.y * f.z - s.z * f.y;
	u.y = s.z * f.x - s.x * f.z;
	u.z = s.x * f.y - s.y * f.x;

	m.m[0] = s.x;
	m.m[4] = s.y;
	m.m[8] = s.z;
	m.m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
	m.m[1] = u.x;
	m.m[5] = u.y;
	m.m[9] = u.z;
	m.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
	m.m[2] = -f.x;
	m.m[6] = -f.y;
	m.m[10] = -f.z;
	m.m[14] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
	m.m[15] = 1.0f;
	return m;
}

Vec4 mat4TransformPoint(const Mat4 *m, Vec3 p)
{
	Vec4 out;

	out.x = m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12];
	out.y = m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13];
	out.z = m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14];
	out.w = m->m[3] * p.x + m->m[7] * p.y + m->m[11] * p.z + m->m[15];
	return out;
}
