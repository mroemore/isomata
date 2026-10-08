/*
 * Headless sanity suite (CTOL rung 1: unit + boundary).
 *
 * Pure C only: no SDL3, no SDL_gpu, no window. It links nothing besides
 * libm + the Unity subset, and exists to prove the headless harness itself
 * works end to end (runner, fixtures, assertions, libm link). Engine modules
 * land here as plain Unity test files in later tasks.
 */

#include "unity.h"

#include <math.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* Every assertion family the subset offers, on the passing path. */
static void test_unity_assertions_pass(void)
{
	TEST_ASSERT_TRUE(2 > 1);
	TEST_ASSERT_FALSE(2 < 1);
	TEST_ASSERT_NULL(NULL);
	TEST_ASSERT_NOT_NULL("non-empty string");
	TEST_ASSERT_EQUAL_INT(-42, -40 - 2);
	TEST_ASSERT_EQUAL_INT8(7, 7);
	TEST_ASSERT_EQUAL_INT32(100000, 100000);
	TEST_ASSERT_EQUAL_UINT(3u, 3u);
	TEST_ASSERT_EQUAL_UINT64(9999999999ULL, 9999999999ULL);
	TEST_ASSERT_EQUAL_PTR((const void *)"x", (const void *)"x");
	TEST_ASSERT_EQUAL_STRING("isomata", "isom" "ata");
	TEST_ASSERT_EQUAL_STRING_LEN("isomata", "isomxyz", 4);
	TEST_ASSERT_EQUAL_MEMORY("ace", "ace", 3);
	TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.5f, 0.55f);
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.5, 1.5);
}

/* Exercises the libm link of the headless executable (ctol_deps). */
static void test_math_links_libm(void)
{
	TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.41421356237309504880, sqrt(2.0));
	TEST_ASSERT_EQUAL_INT(3, 1 + 2);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_unity_assertions_pass);
	RUN_TEST(test_math_links_libm);
	return UNITY_END();
}
