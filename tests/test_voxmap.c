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
#include "render/materials.h"
#include "render/textures.h"
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
	return loadVoxmap(path, NULL);
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

/* '0' and '.' are both void: height 0 is the "nothing" level, not a
 * ground-level cell. */
static void test_zero_and_dot_are_void(void)
{
	Voxmap *map = loadTemp("vm_zero.txt", "0.\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 0, 0));
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
	TEST_ASSERT_NULL(loadVoxmap(NULL, NULL));
	TEST_ASSERT_NULL(loadVoxmap(".tmp_files/definitely_missing_xyz.txt",
				    NULL));
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

	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(1, 2, 3, 4));

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
	voxmapEmitFaces(map, NULL, &list, &camera, tint);
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

	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(1, 2, 3, 4));
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
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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
	voxmapEmitFaces(NULL, NULL, &list, NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, drawListCount(&list));
	voxmapEmitFaces(NULL, NULL, NULL, NULL, 0);
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
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	/* (0,0) top only; (0,1) top + +Z side; (1,1) top + +Z side. */
	TEST_ASSERT_EQUAL_INT(5, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, NULL, &camera, 0);		/* valid map, NULL list */
	TEST_ASSERT_EQUAL_INT(0, drawListCount(&list));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* parseVoxmapText: the in-memory entry point parses the same format. */
static void test_parse_memory_basic(void){
	const char *text = "12\n34\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);

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
	map = parseVoxmapText(buf, 3, NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 1, 0));
	destroyVoxmap(map);

	map = parseVoxmapText("12\n34\n", 2, NULL);	/* slice stops mid-line */
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(1, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 1, 0));
	destroyVoxmap(map);
}

/* Void cells through the memory path: '0' and '.' are both void. */
static void test_parse_memory_void_and_zero(void)
{
	const char *text = "0.\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	destroyVoxmap(map);
}

/* Malformed / NULL / empty memory input returns NULL. */
static void test_parse_memory_rejects_bad_input(void)
{
	TEST_ASSERT_NULL(parseVoxmapText(NULL, 0, NULL));
	TEST_ASSERT_NULL(parseVoxmapText("", 0, NULL));
	TEST_ASSERT_NULL(parseVoxmapText("\n\n", 2, NULL));
	TEST_ASSERT_NULL(parseVoxmapText("1x\n", 3, NULL));
	TEST_ASSERT_NULL(parseVoxmapText("12\n3\n", 5, NULL));
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
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Tint of the top face at column x (0 = even, 1 = odd), or 0 when absent. */
static uint32_t topTintAt(const DrawList *list, int x)
{
	const float top[4][2] = ATLAS_UV_TOP;
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);

		if (memcmp(item->uv, top, sizeof(top)) == 0 &&
		    item->worldQuad[0][0] == (float)x)
			return item->tint;
	}
	return 0;
}

/* Tint of the side face with the given direction (0 = +Z .. 3 = -X). */
static uint32_t sideTintByDir(const DrawList *list, int dir)
{
	const float side[4][2] = ATLAS_UV_SIDE;
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);

		if (memcmp(item->uv, side, sizeof(side)) == 0 &&
		    sideDirOf(item) == dir)
			return item->tint;
	}
	return 0;
}

/* Tint of the +Z side face whose low corner x equals `x` (column selector
 * when several +Z sides are emitted). */
static uint32_t plusZSideTintAt(const DrawList *list, int x)
{
	const float side[4][2] = ATLAS_UV_SIDE;
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);

		if (memcmp(item->uv, side, sizeof(side)) == 0 &&
		    sideDirOf(item) == 0 && item->worldQuad[0][0] == (float)x)
			return item->tint;
	}
	return 0;
}

/* The shade constants: top strictly brightest; four distinct side values in
 * [0.55, 0.92]; checker boost in [1.02, 1.10]. */
static void test_shade_constants(void)
{
	const float sides[4] = { VOXMAP_SHADE_SIDE_PZ, VOXMAP_SHADE_SIDE_PX,
				 VOXMAP_SHADE_SIDE_NZ, VOXMAP_SHADE_SIDE_NX };
	int i;
	int j;

	TEST_ASSERT_TRUE(VOXMAP_SHADE_TOP > VOXMAP_SHADE_SIDE_PZ);
	TEST_ASSERT_TRUE(VOXMAP_SHADE_TOP > VOXMAP_SHADE_SIDE_PX);
	TEST_ASSERT_TRUE(VOXMAP_SHADE_TOP > VOXMAP_SHADE_SIDE_NZ);
	TEST_ASSERT_TRUE(VOXMAP_SHADE_TOP > VOXMAP_SHADE_SIDE_NX);
	for (i = 0; i < 4; i++) {
		TEST_ASSERT_TRUE(sides[i] >= 0.55f && sides[i] <= 0.92f);
		for (j = i + 1; j < 4; j++)
			TEST_ASSERT_TRUE(fabsf(sides[i] - sides[j]) > 1e-4f);
	}
	TEST_ASSERT_TRUE(VOXMAP_CHECKER_BOOST >= 1.02f);
	TEST_ASSERT_TRUE(VOXMAP_CHECKER_BOOST <= 1.10f);
}

/* Odd tiles are brightened by exactly the checker boost on the top face; the
 * even tile keeps the bare top shade. Alpha is preserved. */
static void test_checker_odd_even_top(void)
{
	/* Two height-2 columns in one row: (0,0) even, (1,0) odd. */
	Voxmap *map = loadTemp("vm_checker_top.txt", "22\n");
	DrawList list;
	Camera3D camera;
	int evenR;
	int oddR;
	int expectedOdd;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 77));

	evenR = (int)((topTintAt(&list, 0) >> 24) & 0xffu);
	oddR = (int)((topTintAt(&list, 1) >> 24) & 0xffu);
	expectedOdd = (int)(200.0f * VOXMAP_SHADE_TOP * VOXMAP_CHECKER_BOOST +
			    0.5f);

	TEST_ASSERT_EQUAL_INT(200, evenR);		/* (0+0) even */
	TEST_ASSERT_EQUAL_INT(expectedOdd, oddR);	/* (1+0) odd */
	/* RGB-only: alpha preserved on both. */
	TEST_ASSERT_EQUAL_INT(77, (int)(topTintAt(&list, 0) & 0xffu));
	TEST_ASSERT_EQUAL_INT(77, (int)(topTintAt(&list, 1) & 0xffu));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* The checker applies to sides too, on top of the side shade. */
static void test_checker_odd_even_side(void)
{
	Voxmap *map = loadTemp("vm_checker_side.txt", "22\n");
	DrawList list;
	Camera3D camera;
	int evenR;
	int oddR;
	int expectedOdd;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);		/* yaw 0: +Z sides only */
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));

	evenR = (int)((plusZSideTintAt(&list, 0) >> 24) & 0xffu);
	oddR = (int)((plusZSideTintAt(&list, 1) >> 24) & 0xffu);
	expectedOdd = (int)(200.0f * VOXMAP_SHADE_SIDE_PZ *
			    VOXMAP_CHECKER_BOOST + 0.5f);

	TEST_ASSERT_EQUAL_INT((int)(200.0f * VOXMAP_SHADE_SIDE_PZ + 0.5f),
			      evenR);
	TEST_ASSERT_EQUAL_INT(expectedOdd, oddR);
	TEST_ASSERT_TRUE(oddR > evenR);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A saturating tint exercises the per-channel clamp: an odd tile's boosted
 * channel exceeds 255 and must clamp, alpha preserved. */
static void test_checker_clamps_saturating_tint(void)
{
	Voxmap *map = loadTemp("vm_checker_clamp.txt", "22\n");
	DrawList list;
	Camera3D camera;
	int evenR;
	int oddR;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 42));

	evenR = (int)((topTintAt(&list, 0) >> 24) & 0xffu);
	oddR = (int)((topTintAt(&list, 1) >> 24) & 0xffu);

	TEST_ASSERT_EQUAL_INT(255, evenR);	/* 255 * 1.0 at the clamp */
	TEST_ASSERT_EQUAL_INT(255, oddR);	/* 255 * 1.06 clamps */
	TEST_ASSERT_EQUAL_INT(42, (int)(topTintAt(&list, 1) & 0xffu));

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* At a 45-degree yaw the two visible sides carry their distinct shade
 * constants: +Z bright, +X mid, and the two differ. */
static void test_sides_distinct_at_45(void)
{
	Voxmap *map = loadTemp("vm_sides45.txt", "2\n");
	DrawList list;
	Camera3D camera;
	int pzR;
	int pxR;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	cameraRotateStep(&camera, 1);
	updateCamera3D(&camera, CAMERA_TURN_SECONDS);
	TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, cameraYawDeg(&camera));

	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));
	pzR = (int)((sideTintByDir(&list, 0) >> 24) & 0xffu);
	pxR = (int)((sideTintByDir(&list, 1) >> 24) & 0xffu);

	TEST_ASSERT_EQUAL_INT((int)(200.0f * VOXMAP_SHADE_SIDE_PZ + 0.5f), pzR);
	TEST_ASSERT_EQUAL_INT((int)(200.0f * VOXMAP_SHADE_SIDE_PX + 0.5f), pxR);
	TEST_ASSERT_TRUE(pzR != pxR);
	TEST_ASSERT_TRUE(pzR > pxR);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* The top face is strictly brighter than any side for the same column/tint. */
static void test_top_brightest(void)
{
	Voxmap *map = loadTemp("vm_topbright.txt", "2\n");
	DrawList list;
	Camera3D camera;
	int topR;
	int sideR;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));

	topR = (int)((topTintAt(&list, 0) >> 24) & 0xffu);
	sideR = (int)((sideTintByDir(&list, 0) >> 24) & 0xffu);
	TEST_ASSERT_TRUE(topR > sideR);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Build a two-material table: id 0 "default" (opaque), id 1 "grass" with six
 * distinct face rects and cutout alpha. Returns the table via *t. */
static void buildTestTable(MaterialTable *t)
{
	AtlasRect r;
	int grass;
	int f;

	memset(t, 0, sizeof(*t));
	materialTableAdd(t, "default", ALPHA_OPAQUE);
	grass = materialTableAdd(t, "grass", ALPHA_CUTOUT);
	TEST_ASSERT_EQUAL_INT(1, grass);
	for (f = 0; f < 6; f++) {
		r.u0 = 0.01f * (float)(f + 1);
		r.v0 = 0.02f * (float)(f + 1);
		r.u1 = r.u0 + 0.05f;
		r.v1 = r.v0 + 0.05f;
		materialTableSetRect(t, grass, (FaceId)f, r);
	}
}

/* Legend lines map a char to a height + material; digits keep their built-in
 * default (height = digit, "default" material). */
static void test_legend_maps_char_to_material(void)
{
	MaterialTable t;
	Voxmap *map;
	const char *text = "@ g 2 grass\n@ s 3 default\ngs\nsg\n";

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 1));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));	/* grass */
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 1, 0));	/* default */
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 1, 1));	/* grass */
	destroyVoxmap(map);

	/* A plain digit map: height = digit, material = default id. */
	map = parseVoxmapText("45\n", 3, &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(4, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);
}

/* A legend can override a digit's height and material; height 0 is void. */
static void test_legend_overrides_digit(void)
{
	MaterialTable t;
	Voxmap *map;
	const char *text = "@ 1 5 grass\n@ z 0 default\n1z\n";

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(5, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));	/* legend height 0 */
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 1, 0));
	destroyVoxmap(map);
}

/* The attached `@g` legend form and the bare `@ g` form both parse. */
static void test_legend_attached_and_spaced(void)
{
	MaterialTable t;
	Voxmap *map;

	buildTestTable(&t);
	map = parseVoxmapText("@g 2 grass\ng\n", 13, &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);
}

/* An unknown legend material logs and falls back to the default material; a
 * malformed legend line fails the load. */
static void test_legend_unknown_material_and_bad_line(void)
{
	MaterialTable t;
	Voxmap *map;

	buildTestTable(&t);
	map = parseVoxmapText("@ g 2 nope\ng\n", 13, &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));	/* default */
	destroyVoxmap(map);

	TEST_ASSERT_NULL(parseVoxmapText("@ g 2\n", 6, &t));	/* too few */
	TEST_ASSERT_NULL(parseVoxmapText("@ g x grass\ng\n", 14, &t));
	TEST_ASSERT_NULL(parseVoxmapText("@ gg 2 grass\ng\n", 15, &t));
}

/* Legend lines are skipped when counting rows/width. */
static void test_legend_lines_not_map_rows(void)
{
	MaterialTable t;
	Voxmap *map;
	const char *text = "11\n@ g 1 grass\n11\n";

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	destroyVoxmap(map);
}

/* THE EMISSION ORIENTATION PIN: a cell's faces sample their material's
 * per-face rect, oriented upright (a side face's bottom corners take the
 * rect's v1, its top corners v0) and with the material's alpha mode. */
static void test_emission_uses_material_face_uvs(void)
{
	MaterialTable t;
	Voxmap *map;
	DrawList list;
	Camera3D camera;
	const DrawItem *top = NULL;
	const DrawItem *side = NULL;
	float expectTop[4][2];
	float expectSide[4][2];
	size_t i;

	buildTestTable(&t);
	map = parseVoxmapText("@ g 2 grass\ng\n", 14, &t);
	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, &t, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, (int)drawListCount(&list));

	materialFaceUV(&t.items[1].rect[FACE_TOP], FACE_TOP, expectTop);
	materialFaceUV(&t.items[1].rect[FACE_SOUTH], FACE_SOUTH, expectSide);

	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (memcmp(item->uv, expectTop, sizeof(expectTop)) == 0)
			top = item;
		if (memcmp(item->uv, expectSide, sizeof(expectSide)) == 0)
			side = item;
	}
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_NOT_NULL(side);
	/* Upright: the +Z side's world-bottom corners sample the rect bottom. */
	TEST_ASSERT_TRUE(side->uv[0][1] > side->uv[2][1]);
	TEST_ASSERT_EQUAL_INT(ALPHA_CUTOUT, top->alphaMode);
	TEST_ASSERT_EQUAL_INT(ALPHA_CUTOUT, side->alphaMode);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A NULL table falls back to the built-in atlas regions (blend alpha). */
static void test_emission_null_table_fallback(void)
{
	Voxmap *map = parseVoxmapText("2\n", 2, NULL);
	DrawList list;
	Camera3D camera;
	const float top[4][2] = ATLAS_UV_TOP;
	const float side[4][2] = ATLAS_UV_SIDE;
	int tops = 0;
	int sides = 0;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (memcmp(item->uv, top, sizeof(top)) == 0)
			tops++;
		else if (memcmp(item->uv, side, sizeof(side)) == 0)
			sides++;
		TEST_ASSERT_EQUAL_INT(ALPHA_BLEND, item->alphaMode);
	}
	TEST_ASSERT_EQUAL_INT(1, tops);
	TEST_ASSERT_EQUAL_INT(1, sides);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Legend parsing edge cases: too many tokens, an attached multi-char token,
 * a multi-char height, leading whitespace, an attached too-few form, and a
 * legend-only file all behave as documented. */
static void test_legend_parse_edge_cases(void)
{
	MaterialTable t;
	Voxmap *map;

	buildTestTable(&t);
	TEST_ASSERT_NULL(parseVoxmapText("@ g 2 grass a b c d e f\ng\n",
					 strlen("@ g 2 grass a b c d e f\ng\n"),
					 &t));
	TEST_ASSERT_NULL(parseVoxmapText("@gg 2 grass\ng\n",
					 strlen("@gg 2 grass\ng\n"), &t));
	TEST_ASSERT_NULL(parseVoxmapText("@ g 22 grass\ng\n",
					 strlen("@ g 22 grass\ng\n"), &t));
	TEST_ASSERT_NULL(parseVoxmapText("@g 2\n", strlen("@g 2\n"), &t));
	TEST_ASSERT_NULL(parseVoxmapText("notmaterial\n", 12, &t));
	/* A legend line may be indented. */
	map = parseVoxmapText("  @ g 2 grass\ng\n",
			      strlen("  @ g 2 grass\ng\n"), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 0, 0));
	destroyVoxmap(map);
	/* A file with only legend lines has no map rows. */
	TEST_ASSERT_NULL(parseVoxmapText("@ g 2 grass\n",
					 strlen("@ g 2 grass\n"), &t));
	/* An over-long legend line is refused (bounded line buffer). */
	{
		char big[400];
		size_t i;

		memcpy(big, "@ g 2 ", 6);
		for (i = 6; i < sizeof(big) - 1; i++)
			big[i] = 'a';
		big[sizeof(big) - 1] = '\0';
		TEST_ASSERT_NULL(parseVoxmapText(big, strlen(big), &t));
	}
}

/* A zero RGB tint drives every shaded channel to 0 (covers the shade clamp's
 * lower arm); alpha is preserved. */
static void test_shade_zero_channel(void)
{
	Voxmap *map = loadTemp("vm_zerochan.txt", "2\n");
	DrawList list;
	Camera3D camera;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, &list, &camera, DRAW_TINT(0, 0, 0, 123));
	TEST_ASSERT_TRUE(drawListCount(&list) > 0);
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		TEST_ASSERT_EQUAL_INT(0, (int)((item->tint >> 24) & 0xffu));
		TEST_ASSERT_EQUAL_INT(0, (int)((item->tint >> 16) & 0xffu));
		TEST_ASSERT_EQUAL_INT(0, (int)((item->tint >> 8) & 0xffu));
		TEST_ASSERT_EQUAL_INT(123, (int)(item->tint & 0xffu));
	}
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* voxmapMaterialAt mirrors the height bounds: -1 for void / OOB / NULL. */
static void test_material_at_bounds(void)
{
	Voxmap *map = loadTemp("vm_matbounds.txt", "2.\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 1, 0));	/* void */
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, -1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 0, -1));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 2, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 0, 2));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(NULL, 0, 0));
	destroyVoxmap(map);
}

/* A table without a "default" material still parses: digit cells resolve to
 * id 0. */
static void test_legend_no_default_material(void)
{
	MaterialTable t;
	Voxmap *map;

	memset(&t, 0, sizeof(t));
	materialTableAdd(&t, "grass", ALPHA_OPAQUE);
	map = parseVoxmapText("3\n", 2, &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);
}

/* Emitting a cell whose material id is out of range for the table in hand
 * falls back to the built-in regions (covers the id >= count arm). */
static void test_emission_material_id_out_of_range(void)
{
	MaterialTable full;
	MaterialTable small;
	Voxmap *map;
	DrawList list;
	Camera3D camera;
	const float top[4][2] = ATLAS_UV_TOP;
	int tops = 0;
	size_t i;

	buildTestTable(&full);
	memset(&small, 0, sizeof(small));
	materialTableAdd(&small, "default", ALPHA_OPAQUE);	/* count 1 */
	map = parseVoxmapText("@ g 2 grass\ng\n", 14, &full);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));

	initDrawList(&list, 16);
	initCamera3D(&camera);
	voxmapEmitFaces(map, &small, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (memcmp(item->uv, top, sizeof(top)) == 0)
			tops++;
		TEST_ASSERT_EQUAL_INT(ALPHA_BLEND, item->alphaMode);
	}
	TEST_ASSERT_EQUAL_INT(1, tops);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

void run_test_voxmap(void);

void run_test_voxmap(void)
{
	RUN_TEST(test_load_dimensions_and_heights);
	RUN_TEST(test_zero_and_dot_are_void);
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
	RUN_TEST(test_shade_constants);
	RUN_TEST(test_checker_odd_even_top);
	RUN_TEST(test_checker_odd_even_side);
	RUN_TEST(test_checker_clamps_saturating_tint);
	RUN_TEST(test_sides_distinct_at_45);
	RUN_TEST(test_top_brightest);
	RUN_TEST(test_legend_maps_char_to_material);
	RUN_TEST(test_legend_overrides_digit);
	RUN_TEST(test_legend_attached_and_spaced);
	RUN_TEST(test_legend_unknown_material_and_bad_line);
	RUN_TEST(test_legend_lines_not_map_rows);
	RUN_TEST(test_emission_uses_material_face_uvs);
	RUN_TEST(test_emission_null_table_fallback);
	RUN_TEST(test_legend_parse_edge_cases);
	RUN_TEST(test_legend_no_default_material);
	RUN_TEST(test_emission_material_id_out_of_range);
	RUN_TEST(test_material_at_bounds);
	RUN_TEST(test_shade_zero_channel);
}
