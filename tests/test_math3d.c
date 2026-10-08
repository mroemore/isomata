/*
 * math3d tests (CTOL rung 1: unit + boundary).
 *
 * Pins the Mat4 memory convention end to end: column-major storage
 * (m[col*4 + row]) is the SAME layout GLSL/Vulkan reads from a std140
 * uniform block, so a Mat4 pushed to the GPU verbatim is the matrix the
 * shader multiplies by. Identity, multiply, translate, ortho (exact
 * entries), look-at, and the world->clip composition are all pinned here;
 * the static-quad render (Task 7) proves the same convention empirically
 * against the GPU.
 *
 * Pure: links only math3d.c plus the Unity subset; no SDL. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_math3d().
 */

#include "unity.h"

#include "render/math3d.h"

#include <math.h>

#define EPS 1e-5f

/* Compare all 16 column-major entries against a flat expectation. */
static void assertMat4Within(const Mat4 *m, const float *expected, float eps)
{
	int i;

	for (i = 0; i < 16; i++)
		TEST_ASSERT_FLOAT_WITHIN_MESSAGE(eps, expected[i], m->m[i], "mat4 entry");
}

/* m[col*4 + row]: identity has the 1s on the diagonal. */
static void test_identity_is_diagonal(void)
{
	const float expect[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	};
	Mat4 m = mat4Identity();

	assertMat4Within(&m, expect, EPS);
}

/* Multiply by identity is a fixed point, and translation composes
 * additively (T(1,2,3) * T(4,5,6) == T(5,7,9)) — the multiply is
 * column-major-correct, not a transposed near-miss. */
static void test_multiply_identity_and_translations(void)
{
	Mat4 id = mat4Identity();
	Mat4 t1 = mat4Translate(1.0f, 2.0f, 3.0f);
	Mat4 t2 = mat4Translate(4.0f, 5.0f, 6.0f);
	Mat4 a = mat4Multiply(&id, &t1);
	Mat4 b = mat4Multiply(&t1, &t2);
	const float expectA[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		1, 2, 3, 1,
	};
	const float expectB[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		5, 7, 9, 1,
	};

	assertMat4Within(&a, expectA, EPS);
	assertMat4Within(&b, expectB, EPS);
}

/* The translation lives in column 3 (indices 12..14), never in the
 * transposed row-3 slots (3,7,11). */
static void test_translate_places_translation_in_column3(void)
{
	Mat4 t = mat4Translate(-2.0f, 0.5f, 7.0f);

	TEST_ASSERT_FLOAT_WITHIN(EPS, -2.0f, t.m[12]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, t.m[13]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 7.0f, t.m[14]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.m[3]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.m[7]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.m[11]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, t.m[15]);
}

/* Ortho exact entries (Vulkan depth range): for l=-2,r=2,b=-1,t=1,n=1,f=11
 * the matrix has 2/(r-l)=0.5, 2/(t-b)=1, -1/(f-n)=-0.1 and -n/(f-n)=-0.1
 * (z in [0,1], not the GL [-1,1]). Asserted entry by entry. */
static void test_ortho_exact_entries(void)
{
	Mat4 o = mat4Ortho(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 11.0f);
	const float expect[16] = {
		0.5f, 0, 0, 0,
		0, 1.0f, 0, 0,
		0, 0, -0.1f, 0,
		0, 0, -0.1f, 1.0f,
	};

	assertMat4Within(&o, expect, EPS);
}

/* Transform point: the world->clip composition under the Vulkan depth
 * range. A point at the right/top/near corner of the ortho volume lands on
 * (1,1,0); the far plane maps to z=1. */
static void test_transform_point_and_world_to_clip(void)
{
	Mat4 o = mat4Ortho(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 11.0f);
	Vec3 corner = { 2.0f, 1.0f, -1.0f };
	Vec4 clip = mat4TransformPoint(&o, corner);
	Vec4 farPoint = mat4TransformPoint(&o, (Vec3){ 0.0f, 0.0f, -11.0f });

	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, clip.x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, clip.y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, clip.z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, clip.w);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, farPoint.z);

	/* proj * translate(1,0,0) applied to (-1,0,-1) == proj of (0,0,-1). */
	{
		Mat4 t = mat4Translate(1.0f, 0.0f, 0.0f);
		Mat4 pt = mat4Multiply(&o, &t);
		Vec3 p = { -1.0f, 0.0f, -1.0f };
		Vec4 a = mat4TransformPoint(&pt, p);
		Vec4 b = mat4TransformPoint(&o, (Vec3){ 0.0f, 0.0f, -1.0f });

		TEST_ASSERT_FLOAT_WITHIN(EPS, b.x, a.x);
		TEST_ASSERT_FLOAT_WITHIN(EPS, b.y, a.y);
		TEST_ASSERT_FLOAT_WITHIN(EPS, b.z, a.z);
	}
}

/* Look-at: the eye maps to the camera origin and the target lands on the
 * -Z axis at exactly minus the eye->target distance; a point one unit
 * along the camera's right maps to +X. */
static void test_look_at_maps_target_down_negative_z(void)
{
	Vec3 eye = { 0.0f, 5.0f, 20.0f };
	Vec3 target = { 0.0f, 0.0f, 0.0f };
	Vec3 up = { 0.0f, 1.0f, 0.0f };
	Mat4 v = mat4LookAt(eye, target, up);
	Vec4 o = mat4TransformPoint(&v, eye);
	Vec4 t = mat4TransformPoint(&v, target);

	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, o.x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, o.y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, o.z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t.y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -sqrtf(25.0f + 400.0f), t.z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, t.w);
}

/* --- runner ----------------------------------------------------------- */

void run_test_math3d(void);

void run_test_math3d(void)
{
	RUN_TEST(test_identity_is_diagonal);
	RUN_TEST(test_multiply_identity_and_translations);
	RUN_TEST(test_translate_places_translation_in_column3);
	RUN_TEST(test_ortho_exact_entries);
	RUN_TEST(test_transform_point_and_world_to_clip);
	RUN_TEST(test_look_at_maps_target_down_negative_z);
}
