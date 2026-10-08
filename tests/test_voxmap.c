/*
 * Voxmap tests (CTOL rung 1: unit + boundary).
 *
 * Pins the ASCII heightmap contract: digit cells are column heights, '.' is
 * void, any other cell fails the load with NULL; CRLF and trailing whitespace
 * are tolerated and blank lines ignored; ragged/empty/over-large maps fail;
 * the height query returns -1 for void / out-of-bounds / NULL and 0..9 for a
 * real cell; and face generation emits a top face per non-void column plus the
 * exposed side faces, culled by the dot-product test against the camera's
 * ground direction (one side at an axis-aligned yaw, two at 45 degrees,
 * continuous through a tween).
 *
 * Pure: links only voxmap.c (+ drawlist/camera deps) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_voxmap().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/voxmap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define EPS 1e-4f

/* Write content to .tmp_files/<name> (created if needed) and return the path
 * in a static buffer. Returns NULL when the file cannot be created. */
static const char *writeTemp(const char *name, const char *content)
{
	static char path[256];
	FILE *file;

	mkdir(".tmp_files", 0755);
	snprintf(path, sizeof(path), ".tmp_files/%s", name);
	file = fopen(path, "wb");
	if (file == NULL)
		return NULL;
	fwrite(content, 1, strlen(content), file);
	fclose(file);
	return path;
}

static Voxmap *loadTemp(const char *name, const char *content)
{
	const char *path = writeTemp(name, content);

	if (path == NULL)
		return NULL;
	return loadVoxmap(path);
}

/* Basic dimensions and per-cell heights, plus the height-0 / void split. */
static void test_load_dimensions_and_heights(void)
{
	Voxmap *map = loadTemp("vm_basic.txt", "12\n34\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 1));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 1));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
	destroyVoxmap(map);
}

/* '0' is a real ground-level cell (height 0, not void); '.' is void. */
static void test_height_zero_is_not_void(void)
{
	Voxmap *map = loadTemp("vm_zero.txt", "0.\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	destroyVoxmap(map);
}

/* CRLF line endings, trailing spaces/tabs and blank lines are all tolerated. */
static void test_crlf_trailing_whitespace_and_blank_lines(void)
{
	Voxmap *map = loadTemp("vm_crlf.txt", "12  \r\n34\t\r\n\r\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 1));
	destroyVoxmap(map);
}

/* Out-of-bounds and NULL queries are pinned to -1 / void. */
static void test_bounds_and_null_queries(void)
{
	Voxmap *map = loadTemp("vm_bounds.txt", "55\n55\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, -1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 0, -1));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 2, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 0, 2));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 2, 2));

	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(NULL, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(NULL, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapWidth(NULL));
	TEST_ASSERT_EQUAL_INT(0, voxmapDepth(NULL));
	destroyVoxmap(map);
	destroyVoxmap(NULL);
}

/* A non-digit, non-void cell fails the load with NULL. */
static void test_invalid_cell_fails(void)
{
	TEST_ASSERT_NULL(loadTemp("vm_bad.txt", "1x\n"));
	TEST_ASSERT_NULL(loadTemp("vm_bad2.txt", "1-\n"));
}

/* Ragged rows, an empty map and a missing file all fail. */
static void test_malformed_maps_fail(void)
{
	TEST_ASSERT_NULL(loadTemp("vm_ragged.txt", "12\n3\n"));
	TEST_ASSERT_NULL(loadTemp("vm_empty.txt", ""));
	TEST_ASSERT_NULL(loadTemp("vm_blank.txt", "\n\n"));
	TEST_ASSERT_NULL(loadVoxmap(NULL));
	TEST_ASSERT_NULL(loadVoxmap(".tmp_files/definitely_missing_xyz.txt"));
}

/* Which side a face is, from its world quad: 0 = +Z, 1 = +X, 2 = -Z,
 * 3 = -X (voxmap.c's kSideDx/kSideDz order). Side quads have exactly one
 * coordinate constant (x = 0/1 or z = 0/1); the corner values are exact
 * integers, so the comparison is exact. */
static int sideDirOf(const DrawItem *item)
{
	bool x1 = true;
	bool z0 = true;
	bool z1 = true;
	int i;

	for (i = 0; i < 4; i++) {
		if (item->worldQuad[i][0] != 1.0f)
			x1 = false;
		if (item->worldQuad[i][2] != 0.0f)
			z0 = false;
		if (item->worldQuad[i][2] != 1.0f)
			z1 = false;
	}
	if (z1)
		return 0;
	if (x1)
		return 1;
	if (z0)
		return 2;
	return 3;
}

/* Emit a single-column map at `yawDeg` (a multiple of 45) and report which
 * of the four sides were emitted plus the top-face count. */
static void emitColumnAtYaw(float yawDeg, bool sides[4], int *tops)
{
	Voxmap *map = loadTemp("vm_cull.txt", "2\n");
	DrawList list;
	Camera3D camera;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	if (yawDeg != 0.0f) {
		int steps = (int)lroundf(yawDeg / CAMERA_STEP_DEG);

		while (steps-- > 0) {
			cameraRotateStep(&camera, 1);
			updateCamera3D(&camera, CAMERA_TURN_SECONDS);
		}
	}
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, yawDeg, cameraYawDeg(&camera));

	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(1, 2, 3, 4));

	sides[0] = sides[1] = sides[2] = sides[3] = false;
	*tops = 0;
	{
		const float side[4][2] = ATLAS_UV_SIDE;

		for (i = 0; i < drawListCount(&list); i++) {
			const DrawItem *item = drawListItem(&list, i);

			if (memcmp(item->uv, side, sizeof(side)) == 0)
				sides[sideDirOf(item)] = true;
			else
				(*tops)++;
		}
	}
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A single height-2 column at axis-aligned yaw 0: one top face plus the one
 * exposed camera-facing (+Z) side. */
static void test_faces_single_column_yaw_zero(void)
{
	Voxmap *map = loadTemp("vm_col.txt", "2\n");
	DrawList list;
	Camera3D camera;
	const DrawItem *item;
	uint32_t tint = DRAW_TINT(255, 255, 255, 255);

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);		/* yaw 0 */
	voxmapEmitFaces(map, &list, &camera, tint);
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));

	/* Item 0 is the top face at y = 2. */
	item = drawListItem(&list, 0);
	TEST_ASSERT_NOT_NULL(item);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, item->kind);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, item->worldQuad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, item->worldQuad[2][1]);
	{
		const float top[4][2] = ATLAS_UV_TOP;

		TEST_ASSERT_EQUAL_MEMORY(top, item->uv, sizeof(top));
	}

	/* Item 1 is the +Z side: corners at z = 1, spanning y 0..2. */
	item = drawListItem(&list, 1);
	TEST_ASSERT_NOT_NULL(item);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, item->worldQuad[0][2]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, item->worldQuad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, item->worldQuad[2][1]);
	{
		const float side[4][2] = ATLAS_UV_SIDE;

		TEST_ASSERT_EQUAL_MEMORY(side, item->uv, sizeof(side));
	}

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* The cull set at every 45-degree rest yaw. A single column exposes all
 * four sides, so the emitted set is exactly the sides whose outward normal
 * has a positive dot with the ground direction toward the camera. */
static void test_faces_cull_set_at_rest_yaws(void)
{
	static const struct {
		float yaw;
		bool expect[4];	/* +Z, +X, -Z, -X */
	} cases[] = {
		{ 0.0f,   { true,  false, false, false } },	/* +Z */
		{ 45.0f,  { true,  true,  false, false } },	/* +Z +X */
		{ 90.0f,  { false, true,  false, false } },	/* +X */
		{ 135.0f, { false, true,  true,  false } },	/* +X -Z */
		{ 180.0f, { false, false, true,  false } },	/* -Z */
		{ 225.0f, { false, false, true,  true  } },	/* -Z -X */
		{ 270.0f, { false, false, false, true  } },	/* -X */
		{ 315.0f, { true,  false, false, true  } },	/* +Z -X */
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bool sides[4];
		int tops = 0;
		int d;

		emitColumnAtYaw(cases[i].yaw, sides, &tops);
		TEST_ASSERT_EQUAL_INT(1, tops);
		for (d = 0; d < 4; d++)
			TEST_ASSERT_EQUAL_INT(cases[i].expect[d], sides[d]);
	}
}

/* At a 45-degree rest yaw two sides face the camera (+Z and +X): 1 top +
 * 2 sides. */
static void test_faces_yaw45_emits_two_sides(void)
{
	bool sides[4];
	int tops = 0;

	emitColumnAtYaw(45.0f, sides, &tops);
	TEST_ASSERT_EQUAL_INT(1, tops);
	TEST_ASSERT_TRUE(sides[0]);
	TEST_ASSERT_TRUE(sides[1]);
	TEST_ASSERT_FALSE(sides[2]);
	TEST_ASSERT_FALSE(sides[3]);
}

/* Mid-tween the rule is continuous: at 22.5 degrees +Z and +X still face
 * the camera while -Z and -X are culled (no all-sides special case). */
static void test_faces_cull_mid_tween_yaw(void)
{
	Voxmap *map = loadTemp("vm_cull_mid.txt", "2\n");
	DrawList list;
	Camera3D camera;
	bool sides[4] = { false, false, false, false };
	int tops = 0;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	cameraRotateStep(&camera, 1);
	updateCamera3D(&camera, CAMERA_TURN_SECONDS * 0.5f);	/* yaw 22.5 */
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.5f, cameraYawDeg(&camera));

	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(1, 2, 3, 4));
	{
		const float side[4][2] = ATLAS_UV_SIDE;

		for (i = 0; i < drawListCount(&list); i++) {
			const DrawItem *item = drawListItem(&list, i);

			if (memcmp(item->uv, side, sizeof(side)) == 0)
				sides[sideDirOf(item)] = true;
			else
				tops++;
		}
	}
	TEST_ASSERT_EQUAL_INT(1, tops);
	TEST_ASSERT_TRUE(sides[0]);
	TEST_ASSERT_TRUE(sides[1]);
	TEST_ASSERT_FALSE(sides[2]);
	TEST_ASSERT_FALSE(sides[3]);
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* At an axis-aligned yaw the two edge-on sides have dot(n, toCameraGround)
 * exactly 0, which the <= CAMERA_CULL_EPS rule culls: only the one facing
 * side survives. */
static void test_faces_edge_on_side_culled(void)
{
	bool sides[4];
	int tops = 0;

	emitColumnAtYaw(0.0f, sides, &tops);
	TEST_ASSERT_TRUE(sides[0]);	/* +Z faces the camera */
	TEST_ASSERT_FALSE(sides[1]);	/* +X edge-on (dot 0): culled */
	TEST_ASSERT_FALSE(sides[2]);
	TEST_ASSERT_FALSE(sides[3]);	/* -X edge-on (dot 0): culled */

	emitColumnAtYaw(90.0f, sides, &tops);
	TEST_ASSERT_TRUE(sides[1]);
	TEST_ASSERT_FALSE(sides[0]);	/* +Z edge-on at yaw 90: culled */
	TEST_ASSERT_FALSE(sides[2]);
	TEST_ASSERT_FALSE(sides[3]);
}

/* A side is emitted only when the neighbour is lower; a same-height +Z
 * neighbour suppresses the +Z side (occlusion culling). */
static void test_faces_cull_hidden_side(void)
{
	/* Two columns of equal height along +Z: the front column's +Z neighbour
	 * is the back column (equal height) -> no side there; the back column
	 * has an out-of-bounds +Z neighbour (void, height 0) -> a +Z side. */
	Voxmap *map = loadTemp("vm_pair.txt", "5\n5\n");
	DrawList list;
	Camera3D camera;
	size_t i;
	int sides = 0;
	int tops = 0;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);		/* yaw 0 */
	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	/* 2 tops + 1 exposed +Z side (back column only). */
	TEST_ASSERT_EQUAL_INT(3, drawListCount(&list));
	for (i = 0; i < drawListCount(&list); i++) {
		const float side[4][2] = ATLAS_UV_SIDE;

		if (memcmp(drawListItem(&list, i)->uv, side, sizeof(side)) == 0)
			sides++;
		else
			tops++;
	}
	TEST_ASSERT_EQUAL_INT(1, sides);
	TEST_ASSERT_EQUAL_INT(2, tops);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* NULL map / list emit nothing. */
static void test_emit_null_safe(void)
{
	DrawList list;

	initDrawList(&list, 4);
	voxmapEmitFaces(NULL, &list, NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, drawListCount(&list));
	voxmapEmitFaces(NULL, NULL, NULL, 0);
	destroyDrawList(&list);
}

/* A file with no trailing newline still loads (the line scan reaches the
 * buffer end), and a blank line in the middle is skipped in both passes. */
static void test_no_trailing_newline_and_middle_blank(void)
{
	Voxmap *map = loadTemp("vm_noeol.txt", "12\n34");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 1));
	destroyVoxmap(map);

	map = loadTemp("vm_midblank.txt", "12\n\n34\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 1));
	destroyVoxmap(map);
}

/* A non-printable invalid byte fails the load (exercises the '?' diagnostic
 * path); a high byte also exercises the printability check's upper arm. */
static void test_nonprintable_cell_fails(void)
{
	TEST_ASSERT_NULL(loadTemp("vm_ctl.txt", "1\x01\n"));
	TEST_ASSERT_NULL(loadTemp("vm_high.txt", "1\x7f\n"));
}

/* Over-large dimensions fail rather than allocating. */
static void test_over_large_map_fails(void)
{
	char wide[VOXMAP_MAX_DIM + 4];
	char tall[(VOXMAP_MAX_DIM + 2) * 2];
	int i;

	for (i = 0; i < VOXMAP_MAX_DIM + 1; i++)
		wide[i] = '1';
	wide[VOXMAP_MAX_DIM + 1] = '\n';
	wide[VOXMAP_MAX_DIM + 2] = '\0';
	TEST_ASSERT_NULL(loadTemp("vm_wide.txt", wide));

	for (i = 0; i < VOXMAP_MAX_DIM + 1; i++) {
		tall[i * 2] = '1';
		tall[i * 2 + 1] = '\n';
	}
	tall[(VOXMAP_MAX_DIM + 1) * 2] = '\0';
	TEST_ASSERT_NULL(loadTemp("vm_tall.txt", tall));
}

/* A void cell emits no faces; a valid map with a NULL list emits nothing. */
static void test_faces_void_cell_and_null_list(void)
{
	/* (1,0) is void: skipped. Exposed +Z sides at the back row only. */
	Voxmap *map = loadTemp("vm_void.txt", "1.\n11\n");
	DrawList list;
	Camera3D camera;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	/* (0,0) top only; (0,1) top + +Z side; (1,1) top + +Z side. */
	TEST_ASSERT_EQUAL_INT(5, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, &camera, 0);		/* valid map, NULL list */
	TEST_ASSERT_EQUAL_INT(0, drawListCount(&list));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* parseVoxmapText: the in-memory entry point parses the same format. */
static void test_parse_memory_basic(void){
	const char *text = "12\n34\n";
	Voxmap *map = parseVoxmapText(text, strlen(text));

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 1));
	destroyVoxmap(map);
}

/* parseVoxmapText is length-bounded and must not assume NUL termination: a
 * buffer with no NUL anywhere, and a slice that stops mid-line, both parse
 * exactly `length` bytes. A strlen-based reader would fail both (the trailing
 * garbage would be a ragged/invalid row). */
static void test_parse_memory_length_bounded(void)
{
	char buf[8];
	Voxmap *map;

	memset(buf, 'X', sizeof(buf));	/* no NUL in the whole buffer */
	buf[0] = '4';
	buf[1] = '4';
	buf[2] = '\n';
	map = parseVoxmapText(buf, 3);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 0));
	destroyVoxmap(map);

	map = parseVoxmapText("12\n34\n", 2);	/* slice stops mid-line */
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 1, 0));
	destroyVoxmap(map);
}

/* Void cells and height-0 cells through the memory path. */
static void test_parse_memory_void_and_zero(void)
{
	const char *text = "0.\n";
	Voxmap *map = parseVoxmapText(text, strlen(text));

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	destroyVoxmap(map);
}

/* Malformed / NULL / empty memory input returns NULL. */
static void test_parse_memory_rejects_bad_input(void)
{
	TEST_ASSERT_NULL(parseVoxmapText(NULL, 0));
	TEST_ASSERT_NULL(parseVoxmapText("", 0));
	TEST_ASSERT_NULL(parseVoxmapText("\n\n", 2));
	TEST_ASSERT_NULL(parseVoxmapText("1x\n", 3));
	TEST_ASSERT_NULL(parseVoxmapText("12\n3\n", 5));
}

/* A draw list too small for the map drops the overflow faces (appendVoxelFace
 * returns false) and leaves exactly capacity items; the one-time diagnostic
 * is emitted on the first drop and suppressed on the next. */
static void test_faces_overflow_drops_and_reports_once(void)
{
	Voxmap *map = loadTemp("vm_overflow.txt", "1111\n1111\n");
	DrawList list;
	Camera3D camera;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 2);		/* 8 columns -> far more than 2 faces */
	initCamera3D(&camera);
	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

void run_test_voxmap(void);

void run_test_voxmap(void)
{
	RUN_TEST(test_load_dimensions_and_heights);
	RUN_TEST(test_height_zero_is_not_void);
	RUN_TEST(test_crlf_trailing_whitespace_and_blank_lines);
	RUN_TEST(test_bounds_and_null_queries);
	RUN_TEST(test_invalid_cell_fails);
	RUN_TEST(test_malformed_maps_fail);
	RUN_TEST(test_no_trailing_newline_and_middle_blank);
	RUN_TEST(test_nonprintable_cell_fails);
	RUN_TEST(test_over_large_map_fails);
	RUN_TEST(test_parse_memory_basic);
	RUN_TEST(test_parse_memory_length_bounded);
	RUN_TEST(test_parse_memory_void_and_zero);
	RUN_TEST(test_parse_memory_rejects_bad_input);
	RUN_TEST(test_faces_single_column_yaw_zero);
	RUN_TEST(test_faces_cull_set_at_rest_yaws);
	RUN_TEST(test_faces_yaw45_emits_two_sides);
	RUN_TEST(test_faces_cull_mid_tween_yaw);
	RUN_TEST(test_faces_edge_on_side_culled);
	RUN_TEST(test_faces_cull_hidden_side);
	RUN_TEST(test_faces_void_cell_and_null_list);
	RUN_TEST(test_faces_overflow_drops_and_reports_once);
	RUN_TEST(test_emit_null_safe);
}
