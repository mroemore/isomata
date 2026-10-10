/*
 * A* pathfinding tests (CTOL rung 1: unit + boundary).
 *
 * Exact small-map cases: straight line, wall detour, doorway, enclosed goal,
 * start == goal; start/goal void or out of bounds; the output and node caps
 * (TOO_LONG); determinism across two runs; and a validity helper that every
 * successful path is a chain of legal walkable steps ending on the goal.
 *
 * Pure: links ai/pathfind.c + entities.c + voxmap.c + the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_pathfind().
 */

#include "unity.h"

#include "ai/pathfind.h"
#include "entities/entities.h"
#include "render/voxmap.h"

#include <string.h>

static Voxmap *parseText(const char *text)
{
	return parseVoxmapText(text, strlen(text), NULL);
}

/* Every step of `out` (start tile -> out[0] -> ... -> out[n-1] == goal) must
 * pass entityWalkableStep, and the last tile must be the goal. */
static void assertValidChain(const Voxmap *map, int fx, int fz,
			     const int (*out)[2], int n, int tx, int tz)
{
	int i;
	int px = fx;
	int pz = fz;

	if (n == 0) {
		TEST_ASSERT_EQUAL_INT(fx, tx);
		TEST_ASSERT_EQUAL_INT(fz, tz);
		return;
	}
	for (i = 0; i < n; i++) {
		TEST_ASSERT_TRUE(entityWalkableStep(map, px, pz, out[i][0],
						    out[i][1]));
		px = out[i][0];
		pz = out[i][1];
	}
	TEST_ASSERT_EQUAL_INT(tx, out[n - 1][0]);
	TEST_ASSERT_EQUAL_INT(tz, out[n - 1][1]);
}

/* A 5x1 flat corridor: the path is the exact straight line, goal included,
 * start excluded. */
static void test_straight_line(void)
{
	Voxmap *map = parseText("11111\n");
	int out[8][2];
	int n = -1;
	int st = findPath(map, 0, 0, 4, 0, out, 8, &n);

	TEST_ASSERT_EQUAL_INT(PATH_OK, st);
	TEST_ASSERT_EQUAL_INT(4, n);
	TEST_ASSERT_EQUAL_INT(1, out[0][0]);
	TEST_ASSERT_EQUAL_INT(0, out[0][1]);
	TEST_ASSERT_EQUAL_INT(4, out[3][0]);
	TEST_ASSERT_EQUAL_INT(0, out[3][1]);
	assertValidChain(map, 0, 0, out, n, 4, 0);
	destroyVoxmap(map);

	/* Same column, different row: exercises the second operand of the
	 * start == goal guard. */
	map = parseText("111\n111\n111\n");
	n = -1;
	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 1, 0, 1, 2, out, 8, &n));
	TEST_ASSERT_EQUAL_INT(2, n);
	assertValidChain(map, 1, 0, out, n, 1, 2);
	destroyVoxmap(map);
}

/* A wall (void column x2, z1..z3) between start and goal: the path detours
 * over the top; its exact tile sequence, length and >-Manhattan length are
 * pinned. */
static void test_wall_detour(void)
{
	Voxmap *map = parseText("11111\n11.11\n11.11\n11.11\n11111\n");
	static const int expect[8][2] = {
		{ 1, 2 }, { 1, 1 }, { 1, 0 }, { 2, 0 },
		{ 3, 0 }, { 3, 1 }, { 3, 2 }, { 4, 2 },
	};
	int out[16][2];
	int n = -1;
	int st = findPath(map, 0, 2, 4, 2, out, 16, &n);
	int i;

	TEST_ASSERT_EQUAL_INT(PATH_OK, st);
	TEST_ASSERT_EQUAL_INT(8, n);
	TEST_ASSERT_TRUE(n > pathfindManhattan(0, 2, 4, 2));
	for (i = 0; i < n; i++) {
		TEST_ASSERT_EQUAL_INT(expect[i][0], out[i][0]);
		TEST_ASSERT_EQUAL_INT(expect[i][1], out[i][1]);
	}
	assertValidChain(map, 0, 2, out, n, 4, 2);
	/* It really goes around: no waypoint is the wall cell. */
	for (i = 0; i < n; i++)
		TEST_ASSERT_FALSE(out[i][0] == 2 && out[i][1] == 2);
	destroyVoxmap(map);
}

/* A wall with a single doorway: the path threads the gap in a straight line. */
static void test_through_doorway(void)
{
	Voxmap *map = parseText("1111111\n1111111\n1111111\n111.111\n111.111\n");
	int out[16][2];
	int n = -1;
	int st = findPath(map, 0, 2, 6, 2, out, 16, &n);
	int i;

	TEST_ASSERT_EQUAL_INT(PATH_OK, st);
	TEST_ASSERT_EQUAL_INT(6, n);
	TEST_ASSERT_EQUAL_INT(6, pathfindManhattan(0, 2, 6, 2));
	for (i = 0; i < n; i++) {
		TEST_ASSERT_EQUAL_INT(i + 1, out[i][0]);
		TEST_ASSERT_EQUAL_INT(2, out[i][1]);
	}
	assertValidChain(map, 0, 2, out, n, 6, 2);
	destroyVoxmap(map);
}

/* A goal boxed in by void on all four sides: no path. */
static void test_unreachable_enclosed(void)
{
	Voxmap *map = parseText("11111\n11.11\n1.1.1\n11.11\n11111\n");
	int out[8][2];
	int n = -1;
	int st = findPath(map, 0, 4, 2, 2, out, 8, &n);

	TEST_ASSERT_EQUAL_INT(PATH_NO_PATH, st);
	TEST_ASSERT_EQUAL_INT(0, n);
	destroyVoxmap(map);
}

/* start == goal is success with an empty path. */
static void test_start_equals_goal(void)
{
	Voxmap *map = parseText("111\n111\n111\n");
	int out[4][2];
	int n = -1;
	int st = findPath(map, 1, 1, 1, 1, out, 4, &n);

	TEST_ASSERT_EQUAL_INT(PATH_OK, st);
	TEST_ASSERT_EQUAL_INT(0, n);
	destroyVoxmap(map);
}

/* Void / out-of-bounds / NULL start and goal, with the start checked first. */
static void test_bad_endpoints(void)
{
	Voxmap *map = parseText("11\n1.\n");
	int out[4][2];
	int n = -1;

	/* (1,1) is void: bad goal when the start is fine. */
	TEST_ASSERT_EQUAL_INT(PATH_BAD_GOAL,
			      findPath(map, 0, 0, 1, 1, out, 4, &n));
	TEST_ASSERT_EQUAL_INT(0, n);
	/* Void start wins over the goal. */
	TEST_ASSERT_EQUAL_INT(PATH_BAD_START,
			      findPath(map, 1, 1, 0, 0, out, 4, &n));
	/* Out of bounds either endpoint. */
	TEST_ASSERT_EQUAL_INT(PATH_BAD_START,
			      findPath(map, 9, 9, 0, 0, out, 4, &n));
	TEST_ASSERT_EQUAL_INT(PATH_BAD_GOAL,
			      findPath(map, 0, 0, 9, 9, out, 4, &n));
	/* NULL map is a bad start. */
	TEST_ASSERT_EQUAL_INT(PATH_BAD_START,
			      findPath(NULL, 0, 0, 1, 0, out, 4, &n));
	destroyVoxmap(map);
}

/* The output cap: a path longer than `cap` is TOO_LONG, and a NULL out is
 * treated as cap 0 (only start == goal succeeds). */
static void test_output_cap(void)
{
	Voxmap *map = parseText("11111\n");
	int out[8][2];
	int n = -1;

	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, out, 3, &n));
	TEST_ASSERT_EQUAL_INT(0, n);
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, out, 0, &n));
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, NULL, 8, &n));
	/* Exact fit is OK. */
	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 0, 0, 4, 0, out, 4, &n));
	TEST_ASSERT_EQUAL_INT(4, n);
	/* start == goal still succeeds with a NULL out. */
	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 2, 0, 2, 0, NULL, 0, &n));
	TEST_ASSERT_EQUAL_INT(0, n);
	destroyVoxmap(map);
}

/* The node-store cap: an unreachable goal on a large flat map forces the
 * search to flood past PATHFIND_MAX_NODES, which fails safely as TOO_LONG
 * (not NO_PATH). Coupled to the expansion order: a reordering that reaches
 * the enclosed goal before the cap would flip this to NO_PATH — keep both
 * in sync. */
static void test_node_cap_too_long(void)
{
	char buf[48 * 49 + 1];
	Voxmap *map;
	int out[PATHFIND_MAX_NODES][2];
	int n = -1;
	int x;
	int z;
	int i = 0;

	for (z = 0; z < 48; z++) {
		for (x = 0; x < 48; x++) {
			int ring = (x == 23 && z == 24) ||
				   (x == 25 && z == 24) ||
				   (x == 24 && z == 23) ||
				   (x == 24 && z == 25);

			buf[i++] = ring ? '.' : '1';
		}
		buf[i++] = '\n';
	}
	buf[i] = '\0';
	map = parseVoxmapText(buf, (size_t)i, NULL);
	TEST_ASSERT_NOT_NULL(map);
	/* The goal (24,24) is valid but boxed in by void: the flood fills the
	 * flat map and trips the node cap. */
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 24, 24, out, PATHFIND_MAX_NODES,
				       &n));
	destroyVoxmap(map);
}

/* Two identical runs return the identical tile list. */
static void test_determinism(void)
{
	Voxmap *map = parseText("11111\n11.11\n11.11\n11.11\n11111\n");
	int a[16][2];
	int b[16][2];
	int na = -1;
	int nb = -1;

	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 0, 2, 4, 2, a, 16, &na));
	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 0, 2, 4, 2, b, 16, &nb));
	TEST_ASSERT_EQUAL_INT(na, nb);
	TEST_ASSERT_EQUAL_INT(0, memcmp(a, b, (size_t)na * sizeof(a[0])));
	destroyVoxmap(map);
}

/* Manhattan distance: symmetric, >= 0, additive along a line. */
static void test_manhattan(void)
{
	TEST_ASSERT_EQUAL_INT(7, pathfindManhattan(0, 0, 3, 4));
	TEST_ASSERT_EQUAL_INT(7, pathfindManhattan(3, 4, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, pathfindManhattan(5, 5, 5, 5));
	TEST_ASSERT_EQUAL_INT(4, pathfindManhattan(-2, 0, 2, 0));
	TEST_ASSERT_EQUAL_INT(6, pathfindManhattan(0, 0, 2, 4));
}

/* An empty / one-tile map: the goal is the start or invalid, never a crash. */
static void test_degenerate_maps(void)
{
	Voxmap *map = parseText("1\n");
	int out[4][2];
	int n = -1;

	TEST_ASSERT_EQUAL_INT(PATH_OK, findPath(map, 0, 0, 0, 0, out, 4, &n));
	TEST_ASSERT_EQUAL_INT(PATH_BAD_GOAL,
			      findPath(map, 0, 0, 1, 0, out, 4, &n));
	destroyVoxmap(map);
}

/* NULL out / outLen and negative caps are safe: outLen NULL is accepted on
 * success and failure, and a negative cap clamps to 0. */
static void test_null_outlen_and_negative_cap(void)
{
	Voxmap *map = parseText("11111\n");
	Voxmap *enc = parseText("11111\n11.11\n1.1.1\n11.11\n11111\n");
	int out[8][2];

	TEST_ASSERT_EQUAL_INT(PATH_OK,
			      findPath(map, 0, 0, 4, 0, out, 8, NULL));
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, out, 2, NULL));
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, out, -5, NULL));
	TEST_ASSERT_EQUAL_INT(PATH_NO_PATH,
			      findPath(enc, 0, 4, 2, 2, out, 8, NULL));
	/* A NULL out is safe too. */
	TEST_ASSERT_EQUAL_INT(PATH_TOO_LONG,
			      findPath(map, 0, 0, 4, 0, NULL, 8, NULL));
	destroyVoxmap(map);
	destroyVoxmap(enc);
}

/* A witness map where A* first reaches a tile by a longer route and later
 * re-relaxes it with a smaller g (the open-node improvement arm). */
static void test_open_node_improvement(void)
{
	Voxmap *map = parseText("11111111\n"
				"111111.1\n"
				"11.111.1\n"
				"1111111.\n"
				"1111111.\n"
				"1111.1.1\n"
				"11111111\n"
				"11111111\n");
	int out[64][2];
	int n = -1;
	int st = findPath(map, 0, 3, 4, 0, out, 64, &n);

	TEST_ASSERT_EQUAL_INT(PATH_OK, st);
	TEST_ASSERT_TRUE(n > 0);
	TEST_ASSERT_TRUE(n >= pathfindManhattan(0, 3, 4, 0));
	assertValidChain(map, 0, 3, out, n, 4, 0);
	destroyVoxmap(map);
}

void run_test_pathfind(void);

void run_test_pathfind(void)
{
	RUN_TEST(test_straight_line);
	RUN_TEST(test_wall_detour);
	RUN_TEST(test_through_doorway);
	RUN_TEST(test_unreachable_enclosed);
	RUN_TEST(test_start_equals_goal);
	RUN_TEST(test_bad_endpoints);
	RUN_TEST(test_output_cap);
	RUN_TEST(test_null_outlen_and_negative_cap);
	RUN_TEST(test_open_node_improvement);
	RUN_TEST(test_node_cap_too_long);
	RUN_TEST(test_determinism);
	RUN_TEST(test_manhattan);
	RUN_TEST(test_degenerate_maps);
}
