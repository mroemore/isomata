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
#include "render/lightgrid.h"
#include "render/materials.h"
#include "render/textures.h"
#include "render/voxmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* '0' is a solid ground-level cell (height 0, emits a top face at y=0) with
 * the default material; '.' is void. */
static void test_zero_is_solid_dot_is_void(void)
{
	Voxmap *map = loadTemp("vm_zero.txt", "0.\n");
	DrawList list;
	Camera3D camera;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAt(map, 1, 0));

	/* The height-0 cell emits its top face at y = 0 (and nothing else:
	 * its only neighbour is void/ground, so no side is exposed). */
	initDrawList(&list, 8);
	initCamera3D(&camera);
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(1, (int)drawListCount(&list));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, drawListItem(&list, 0)->worldQuad[0][1]);
	destroyDrawList(&list);
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

	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(1, 2, 3, 4));

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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, tint);
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

	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(1, 2, 3, 4));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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
	voxmapEmitFaces(NULL, NULL, NULL, &list, NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, drawListCount(&list));
	voxmapEmitFaces(NULL, NULL, NULL, NULL, NULL, 0);
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	/* (0,0) top only; (0,1) top + +Z side; (1,1) top + +Z side. */
	TEST_ASSERT_EQUAL_INT(5, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, NULL, NULL, &camera, 0);		/* valid map, NULL list */
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

/* '0' is solid (height 0) and '.' is void through the memory path too. */
static void test_parse_memory_zero_solid_dot_void(void)
{
	const char *text = "0.\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(2, drawListCount(&list));
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 77));

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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));

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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 42));

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

	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(200, 200, 200, 255));

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

/* A legend can override a digit's height and material; a legend height of 0
 * is a SOLID ground-level cell (not void). */
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
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 1, 0));	/* solid */
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 1, 0));
	destroyVoxmap(map);
}

/* A legend char with height 0 is solid and carries its material; only '.' is
 * void. */
static void test_legend_height_zero_is_solid(void)
{
	MaterialTable t;
	Voxmap *map;
	const char *text = "@ z 0 grass\nz.\n";

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_FALSE(voxmapIsVoid(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));	/* grass */
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
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
	voxmapEmitFaces(map, &t, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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
	voxmapEmitFaces(map, NULL, NULL, &list, &camera, DRAW_TINT(0, 0, 0, 123));
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
	voxmapEmitFaces(map, &small, NULL, &list, &camera, DRAW_TINT(255, 255, 255, 255));
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

/* A legend line whose char is a high byte (>= 128) must not write out of
 * bounds: the entry is skipped and parsing continues (regression for the
 * stack OOB in parseLegend). Both the spaced and attached forms are covered,
 * and the high byte is never a valid cell. */
static void test_legend_high_byte_char_skipped(void)
{
	MaterialTable t;
	Voxmap *map;
	const char *spaced = "@ \x80 2 grass\n@ g 2 grass\ngg\n";
	const char *attached = "@\x80 2 grass\n@ g 3 grass\ng\n";

	buildTestTable(&t);

	map = parseVoxmapText(spaced, strlen(spaced), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 0, 0));	/* later legend */
	TEST_ASSERT_EQUAL_INT(2, voxmapHeightAt(map, 1, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);

	map = parseVoxmapText(attached, strlen(attached), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 0));
	destroyVoxmap(map);

	/* The high byte was never registered: using it as a cell fails the load. */
	TEST_ASSERT_NULL(parseVoxmapText("@ \x80 2 grass\n\x80\n",
					 strlen("@ \x80 2 grass\n\x80\n"), &t));
}

/* --- `$` light parsing -------------------------------------------------- */

static void test_light_parse_point_and_spot(void)
{
	const char *text = "$ point 1 2 3 255 128 64 5\n"
			   "$ spot 4 5 6 10 20 30 0 -1 0 45\n"
			   "11\n11\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);
	const VoxmapLight *p;
	const VoxmapLight *s;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLightCount(map));

	p = voxmapLightAt(map, 0);
	TEST_ASSERT_NOT_NULL(p);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_POINT, p->kind);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, p->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, p->y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, p->z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 255.0f, p->r);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 128.0f, p->g);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 64.0f, p->b);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, p->radius);

	s = voxmapLightAt(map, 1);
	TEST_ASSERT_NOT_NULL(s);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_SPOT, s->kind);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 4.0f, s->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, s->y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 6.0f, s->z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, s->r);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 20.0f, s->g);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, s->b);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->dir[0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, s->dir[1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->dir[2]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 45.0f, s->halfAngleDeg);
	TEST_ASSERT_FLOAT_WITHIN(EPS, VOXMAP_LIGHT_DEFAULT_RADIUS, s->radius);

	/* The map itself still parsed. */
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	destroyVoxmap(map);
}

/* A non-unit direction is normalised; out-of-range channels clamp; an omitted
 * radius defaults to 0 for a point. */
static void test_light_parse_normalizes_and_clamps(void)
{
	const char *text = "$ spot 0 0 0 255 0 0 0 -2 0 30\n"
			   "$ point 0 0 0 300 -5 128\n"
			   "1\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);
	const VoxmapLight *s;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLightCount(map));
	s = voxmapLightAt(map, 0);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_SPOT, s->kind);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->dir[0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, s->dir[1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->dir[2]);
	s = voxmapLightAt(map, 1);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 255.0f, s->r);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->g);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 128.0f, s->b);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s->radius);
	destroyVoxmap(map);
}

/* Malformed light lines are skipped (never a load failure); a valid one after
 * them still lands. */
static void test_light_parse_malformed_skipped(void)
{
	const char *text =
		"$ point 1 2 3 255 0\n"			/* too few */
		"$ point 1 2 3 255 0 0 5 extra\n"	/* too many */
		"$ point a 2 3 255 0 0\n"		/* bad number */
		"$ point 1 2 3 inf 0 0\n"		/* non-finite */
		"$ spot 1 2 3 255 0 0 0 0 0 45\n"	/* zero dir */
		"$ spot 1 2 3 255 0 0 0 -1 0 0\n"	/* zero angle */
		"$ spot 1 2 3 255 0 0 0 -1 0\n"		/* too few */
		"$ bogus 1 2 3\n"			/* unknown kind */
		"$\n"					/* empty */
		"$ spot 1 2 3 255 0 0 0 -1 0 45 2\n"	/* valid */
		"1\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);
	const VoxmapLight *s;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapLightCount(map));
	s = voxmapLightAt(map, 0);
	TEST_ASSERT_NOT_NULL(s);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_SPOT, s->kind);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, s->radius);
	destroyVoxmap(map);
}

/* Every bad token position (the parse chain short-circuits), leading spaces,
 * and an overflowing direction are all skipped. */
static void test_light_parse_token_failures(void)
{
	const char *text =
		"$ point x 2 3 255 0 0\n"
		"$ point 1 x 3 255 0 0\n"
		"$ point 1 2 x 255 0 0\n"
		"$ point 1 2 3 x 0 0\n"
		"$ point 1 2 3 255 x 0\n"
		"$ point 1 2 3 255 0 x\n"
		"$ point 1 2 3 255 0 0 x\n"
		"$ spot x 2 3 255 0 0 0 -1 0 45\n"
		"$ spot 1 x 3 255 0 0 0 -1 0 45\n"
		"$ spot 1 2 x 255 0 0 0 -1 0 45\n"
		"$ spot 1 2 3 x 0 0 0 -1 0 45\n"
		"$ spot 1 2 3 255 x 0 0 -1 0 45\n"
		"$ spot 1 2 3 255 0 x 0 -1 0 45\n"
		"$ spot 1 2 3 255 0 0 x -1 0 45\n"
		"$ spot 1 2 3 255 0 0 0 x 0 45\n"
		"$ spot 1 2 3 255 0 0 0 -1 x 45\n"
		"$ spot 1 2 3 255 0 0 0 -1 0 x\n"
		"$ spot 1 2 3 255 0 0 0 -1 0 45 x\n"
		"$ spot 1 2 3 255 0 0 3e38 3e38 3e38 45\n"	/* inf dir */
		"1\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapLightCount(map));
	destroyVoxmap(map);
}

/* A light line longer than the line buffer is skipped; the next one lands. */
static void test_light_parse_over_long_line_skipped(void)
{
	char text[640];
	size_t pos = 0;
	int i;

	pos += (size_t)snprintf(text + pos, sizeof(text) - pos,
				"$ point 1 2 3 255 0 0 ");
	for (i = 0; i < 300; i++)
		text[pos++] = '1';
	text[pos++] = '\n';
	pos += (size_t)snprintf(text + pos, sizeof(text) - pos,
				"$ point 1 2 3 255 0 0 4\n1\n");
	{
		Voxmap *map = parseVoxmapText(text, pos, NULL);

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_EQUAL_INT(1, voxmapLightCount(map));
		destroyVoxmap(map);
	}
}

/* Accessors and the VOXMAP_MAX_LIGHTS cap. */static void test_light_bounds_and_accessors(void)
{
	Voxmap *map;
	char text[4096];
	size_t pos = 0;
	int i;

	TEST_ASSERT_EQUAL_INT(0, voxmapLightCount(NULL));
	TEST_ASSERT_NULL(voxmapLightAt(NULL, 0));

	for (i = 0; i < VOXMAP_MAX_LIGHTS + 6; i++)
		pos += (size_t)snprintf(text + pos, sizeof(text) - pos,
					"$ point 1 1 1 255 0 0 4\n");
	pos += (size_t)snprintf(text + pos, sizeof(text) - pos, "1\n");
	map = parseVoxmapText(text, pos, NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_MAX_LIGHTS, voxmapLightCount(map));
	TEST_ASSERT_NULL(voxmapLightAt(map, -1));
	TEST_ASSERT_NULL(voxmapLightAt(map, VOXMAP_MAX_LIGHTS));
	TEST_ASSERT_NOT_NULL(voxmapLightAt(map, VOXMAP_MAX_LIGHTS - 1));
	destroyVoxmap(map);
}

/* File and in-memory entry points yield the same lights. */
static void test_light_roundtrip_file_and_memory(void)
{
	const char *content = "$ point 2 3 4 10 20 30 6\n"
			      "$ spot 5 6 7 40 50 60 1 0 0 20\n"
			      "12\n34\n";
	Voxmap *mem = parseVoxmapText(content, strlen(content), NULL);
	Voxmap *file = loadTemp("vm_lights.txt", content);
	int i;

	TEST_ASSERT_NOT_NULL(mem);
	TEST_ASSERT_NOT_NULL(file);
	TEST_ASSERT_EQUAL_INT(2, voxmapLightCount(mem));
	TEST_ASSERT_EQUAL_INT(voxmapLightCount(mem), voxmapLightCount(file));
	for (i = 0; i < voxmapLightCount(mem); i++) {
		const VoxmapLight *a = voxmapLightAt(mem, i);
		const VoxmapLight *b = voxmapLightAt(file, i);

		TEST_ASSERT_EQUAL_INT(a->kind, b->kind);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->x, b->x);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->y, b->y);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->z, b->z);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->r, b->r);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->g, b->g);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->b, b->b);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->radius, b->radius);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->dir[0], b->dir[0]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->dir[1], b->dir[1]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, a->halfAngleDeg, b->halfAngleDeg);
	}
	destroyVoxmap(mem);
	destroyVoxmap(file);
}

/* Light lines between rows do not change the map's width/depth. */
static void test_light_lines_not_map_rows(void)
{
	const char *text = "$ point 0 0 0 255 255 255 4\n"
			   "11\n"
			   "$ spot 0 1 0 255 0 0 0 -1 0 30\n"
			   "\n"
			   "   $ point 1 1 1 10 10 10\n"
			   "11\n";
	Voxmap *map = parseVoxmapText(text, strlen(text), NULL);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(3, voxmapLightCount(map));
	destroyVoxmap(map);
}

/* --- 18. smooth lighting, AO, toggles and debug view (T15) ------------- */

/* The first top face whose 4 corners share the plane y = height, or NULL. */
static const DrawItem *findTopPlaneAt(const DrawList *list, float height)
{
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);
		int k;
		bool flat = true;

		for (k = 0; k < 4; k++)
			if (item->worldQuad[k][1] != height)
				flat = false;
		if (flat)
			return item;
	}
	return NULL;
}

/* The +Z side face whose low-corner x equals `x`, or NULL. */
static const DrawItem *findPlusZSideAt(const DrawList *list, int x)
{
	const float side[4][2] = ATLAS_UV_SIDE;
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);

		if (memcmp(item->uv, side, sizeof(side)) == 0 &&
		    sideDirOf(item) == 0 && item->worldQuad[0][0] == (float)x)
			return item;
	}
	return NULL;
}

/* Smooth top-face corner light averages the 2x2 block around each corner; the
 * flat path keeps the single T14 sample. Map "11" with a lamp over column 0:
 * corner factors {108, 118, 118, 108}, flat sample 151. Tint 240. */
static void test_smooth_light_corner_tints(void)
{
	Voxmap *map = loadTemp("vm_smooth_light.txt", "11\n");
	LightGrid *g = lightGridCreate(2, 1, 2);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, map);

	initDrawList(&list, 32);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	/* 240*108/255 = 102, 240*118/255 = 111. */
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(102, 102, 102, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(111, 111, 111, 255),
			      (int)top->cornerTint[1]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(111, 111, 111, 255),
			      (int)top->cornerTint[2]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(102, 102, 102, 255),
			      (int)top->cornerTint[3]);
	/* The uniform fallback is corner 0. */
	TEST_ASSERT_EQUAL_INT((int)top->cornerTint[0], (int)top->tint);

	/* Flat path (the T14 entry point): one sample (0,1,0) = 151 ->
	 * 240*151/255 = 142 on every corner, byte-identical to T14. */
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(240, 240, 240, 255));
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(142, 142, 142, 255),
			      (int)top->tint);
	TEST_ASSERT_EQUAL_INT((int)top->tint, (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)top->tint, (int)top->cornerTint[3]);

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Smooth top-face AO: a taller neighbour darkens the two corners that touch
 * it. Map "12": the height-1 column's top has AO {0,1,1,0} at 82% for the
 * occluded corners (light 93 everywhere). */
static void test_smooth_ao_corner_tints(void)
{
	Voxmap *map = loadTemp("vm_smooth_ao.txt", "12\n");
	LightGrid *g = lightGridCreate(2, 1, 3);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, map);	/* sky only, no lamps */

	initDrawList(&list, 32);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);	/* (0,0) top at y = 1 */
	TEST_ASSERT_NOT_NULL(top);
	/* Unoccluded 240*93/255 = 88; occluded 240*0.82*93/255 = 72. */
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(88, 88, 88, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(72, 72, 72, 255),
			      (int)top->cornerTint[1]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(72, 72, 72, 255),
			      (int)top->cornerTint[2]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(88, 88, 88, 255),
			      (int)top->cornerTint[3]);

	/* Flat path: no AO, all corners 88. */
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(240, 240, 240, 255));
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(88, 88, 88, 255), (int)top->tint);

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Smooth side face: the single span samples the neighbour-side air cells at the
 * bottom (y0) and top (y1) levels, so a wall base reads dark and its top
 * bright. Map "12", the (0,0) +Z face (neighbour void -> y0 = 0, y1 = 1). */
static void test_smooth_side_corner_tints(void)
{
	Voxmap *map = loadTemp("vm_smooth_side.txt", "12\n");
	LightGrid *g = lightGridCreate(2, 1, 3);
	DrawList list;
	Camera3D cam;
	const DrawItem *side;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, map);

	initDrawList(&list, 32);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	side = findPlusZSideAt(&list, 0);
	TEST_ASSERT_NOT_NULL(side);
	/* y0 corners: AO 2 (ground below both edges) -> 240*0.9*0.65*93/255 =
	 * 51; y1 corners: AO 0 -> 240*0.9*93/255 = 79. */
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(51, 51, 51, 255),
			      (int)side->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(51, 51, 51, 255),
			      (int)side->cornerTint[1]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(79, 79, 79, 255),
			      (int)side->cornerTint[2]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(79, 79, 79, 255),
			      (int)side->cornerTint[3]);

	/* Flat path: one sample (0,0,1) = 93 -> 240*0.9*93/255 = 79 all
	 * corners (byte-identical to T14). */
	clearDrawList(&list);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(240, 240, 240, 255));
	side = findPlusZSideAt(&list, 0);
	TEST_ASSERT_NOT_NULL(side);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(79, 79, 79, 255), (int)side->tint);

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Smooth side sampling covers every side direction (the along-axis and
 * reversed-span arms): a single tall column emits one side at each 90-degree
 * yaw. The base is darker than the top (contact shadow + level sampling). */
static void test_smooth_sides_all_directions(void)
{
	Voxmap *map = loadTemp("vm_smooth_sides.txt", "2\n");
	LightGrid *g = lightGridCreate(1, 1, 3);
	Camera3D cam;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };
	const int yaws[4] = { 0, 90, 180, 270 };
	int i;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, map);

	for (i = 0; i < 4; i++) {
		DrawList list;
		const DrawItem *side = NULL;
		size_t n;

		initDrawList(&list, 16);
		initCamera3D(&cam);
		{
			int steps = yaws[i] / (int)CAMERA_STEP_DEG;

			while (steps-- > 0) {
				cameraRotateStep(&cam, 1);
				updateCamera3D(&cam, CAMERA_TURN_SECONDS);
			}
		}
		voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
		TEST_ASSERT_EQUAL_INT(2, (int)drawListCount(&list));
		for (n = 0; n < drawListCount(&list); n++) {
			const float s[4][2] = ATLAS_UV_SIDE;

			if (memcmp(drawListItem(&list, n)->uv, s,
				   sizeof(s)) == 0)
				side = drawListItem(&list, n);
		}
		TEST_ASSERT_NOT_NULL(side);
		/* Bottom corners (indices 0,1) are darker than the top (2,3). */
		TEST_ASSERT_TRUE((int)side->cornerTint[0] <
				 (int)side->cornerTint[2]);
		TEST_ASSERT_TRUE((int)side->cornerTint[1] <
				 (int)side->cornerTint[3]);
		destroyDrawList(&list);
	}
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Debug view: white UV + the light colour as the tint (no material/shade/AO),
 * corner-interpolated when smooth; NULL debugUV falls back to the spare. */
static void test_light_debug_view(void)
{
	Voxmap *map = loadTemp("vm_debug.txt", "11\n");
	LightGrid *g = lightGridCreate(2, 1, 2);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;
	const float debugUV[4][2] = {
		{ 0.25f, 0.25f }, { 0.75f, 0.25f },
		{ 0.75f, 0.75f }, { 0.25f, 0.75f }
	};
	const float spare[4][2] = ATLAS_UV_SPARE;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, true,
				   debugUV };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, map);

	initDrawList(&list, 32);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_MEMORY(debugUV, top->uv, sizeof(debugUV));
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(108, 108, 108, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(118, 118, 118, 255),
			      (int)top->cornerTint[1]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(118, 118, 118, 255),
			      (int)top->cornerTint[2]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(108, 108, 108, 255),
			      (int)top->cornerTint[3]);
	/* Debug forces opaque even though the fallback material blends. */
	TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, top->alphaMode);

	/* Debug + flat: the single sample (151) on every corner. */
	clearDrawList(&list);
	opts.smooth = false;
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(151, 151, 151, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(151, 151, 151, 255),
			      (int)top->cornerTint[3]);

	/* NULL debugUV falls back to the built-in spare region. */
	clearDrawList(&list);
	opts.debugUV = NULL;
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_MEMORY(spare, top->uv, sizeof(spare));

	/* NULL options emits nothing. */
	clearDrawList(&list);
	voxmapEmitFacesOpt(map, NULL, g, &list, &cam, NULL);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Smooth emission with a NULL light grid stays full-brightness (same as the
 * flat path) and smooth with no grid is still valid. */
static void test_smooth_null_grid_full_brightness(void)
{
	Voxmap *map = loadTemp("vm_smooth_null.txt", "1\n");
	DrawList list;
	Camera3D cam;
	const DrawItem *top;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, NULL, &list, &cam, &opts);
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(240, 240, 240, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)top->cornerTint[0], (int)top->cornerTint[3]);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* --- T13a: 3D occupancy, slice sections, runs + bottoms ---------------- */

static Voxmap *parseText(const char *text)
{
	return parseVoxmapText(text, strlen(text), NULL);
}

static bool itemHasUV(const DrawItem *item, const float uv[4][2])
{
	return memcmp(item->uv, uv, 4 * 2 * sizeof(float)) == 0;
}

static int countUV(const DrawList *list, const float uv[4][2])
{
	int n = 0;
	size_t i;

	for (i = 0; i < drawListCount(list); i++)
		if (itemHasUV(drawListItem(list, i), uv))
			n++;
	return n;
}

/* A multi-section file (2x2, 3 levels): a solid floor, a level with a
 * doorway gap, and a solid roof. Section order is ascending (first = y 0). */
static void test_slice_parse_and_queries(void)
{
	const char *text =
		"11\n"
		"11\n"
		"---\n"
		"1.\n"
		"11\n"
		"---\n"
		"11\n"
		"11\n";
	Voxmap *map = parseText(text);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(3, voxmapLevels(map));
	/* y 0: floor, all solid. */
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 1, 0, 1));
	/* y 1: (1,0) is the doorway (air), the rest solid. */
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 1, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 1, 0));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 1, 1));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 1, 1, 1));
	/* y 2: roof, all solid. */
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 1, 2, 0));
	/* heightAt = topmost solid level + 1. Column (1,0) has a gap at y 1 but
	 * is solid at y 2, so its top surface is y 3. */
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(3, voxmapHeightAt(map, 1, 0));
	/* The doorway cell is solid at no level, so it is air. */
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAtVoxel(map, 1, 1, 0) + 1);
	/* Bounds: y out of range / NULL are false / -1. */
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 3, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, -1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 0, 3, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(NULL, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(NULL, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapLevels(NULL));
	destroyVoxmap(map);
}

/* Slice mode assigns per-voxel materials (the legend height is ignored). */
static void test_slice_per_voxel_materials(void)
{
	MaterialTable t;
	const char *text =
		"@ g 5 grass\n"
		"g1\n"
		"---\n"
		"1g\n";
	Voxmap *map;

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	/* Level 0: (0,0) grass, (1,0) digit -> default. Level 1: reversed. */
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAtVoxel(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAtVoxel(map, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAtVoxel(map, 0, 1, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAtVoxel(map, 1, 1, 0));
	/* The column material query reads the topmost solid voxel. */
	TEST_ASSERT_EQUAL_INT(0, voxmapMaterialAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 1, 0));
	destroyVoxmap(map);
}

/* A space is air in slice mode, and a trailing space is a real cell (only a
 * trailing CR is stripped). */
static void test_slice_space_is_air(void)
{
	const char *text =
		"1 \n"
		"11\n"
		"---\n"
		"..\n"
		"..\n";
	Voxmap *map = parseText(text);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 0, 0));	/* the space */
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 1));
	destroyVoxmap(map);
}

/* Malformed section files are diagnostics, never crashes. */
static void test_slice_malformed_sections(void)
{
	TEST_ASSERT_NULL(parseText("---\n11\n"));		/* leading sep */
	TEST_ASSERT_NULL(parseText("11\n---\n"));		/* trailing sep */
	TEST_ASSERT_NULL(parseText("11\n---\n---\n11\n"));	/* empty section */
	TEST_ASSERT_NULL(parseText("11\n11\n---\n1\n"));	/* depth differs */
	TEST_ASSERT_NULL(parseText("11\n1\n---\n11\n11\n"));	/* ragged */
	TEST_ASSERT_NULL(parseText("11\n---\n111\n"));		/* width differs */
	TEST_ASSERT_NULL(parseText("1x\n---\n11\n"));		/* bad cell */
	TEST_ASSERT_NULL(parseText("11\n---\n1x\n"));		/* bad cell y1 */
	/* A single-section file with no separator is a heightmap: a space is an
	 * invalid cell (though a TRAILING space is stripped as before). */
	TEST_ASSERT_NULL(parseText("1 1\n"));
}

/* The strongest backward-compat pin: a single-section heightmap and the slice
 * stack that encodes the same occupancy produce byte-identical draw lists. */
static void test_heightmap_and_slice_emission_identical(void)
{
	/* Heightmap: row z0 = heights 1,2; row z1 = 1,1. */
	Voxmap *heightmap = parseText("12\n11\n");
	/* The same occupancy as slices: y0 all solid; y1 only (1,0). */
	Voxmap *slices = parseText("11\n11\n---\n.1\n..\n");
	DrawList a;
	DrawList b;
	Camera3D cam;
	size_t i;

	TEST_ASSERT_NOT_NULL(heightmap);
	TEST_ASSERT_NOT_NULL(slices);
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(heightmap));
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(slices));
	initDrawList(&a, 64);
	initDrawList(&b, 64);
	initCamera3D(&cam);
	voxmapEmitFaces(heightmap, NULL, NULL, &a, &cam,
			DRAW_TINT(200, 200, 200, 255));
	voxmapEmitFaces(slices, NULL, NULL, &b, &cam,
			DRAW_TINT(200, 200, 200, 255));
	TEST_ASSERT_EQUAL_INT((int)drawListCount(&a), (int)drawListCount(&b));
	for (i = 0; i < drawListCount(&a); i++) {
		const DrawItem *ia = drawListItem(&a, i);
		const DrawItem *ib = drawListItem(&b, i);

		TEST_ASSERT_EQUAL_MEMORY(ia->worldQuad, ib->worldQuad,
					 sizeof(ia->worldQuad));
		TEST_ASSERT_EQUAL_MEMORY(ia->uv, ib->uv, sizeof(ia->uv));
		TEST_ASSERT_EQUAL_INT((int)ia->tint, (int)ib->tint);
	}
	destroyDrawList(&a);
	destroyDrawList(&b);
	destroyVoxmap(heightmap);
	destroyVoxmap(slices);
}

/* A floating slab: a top face, a NEW bottom face, and the exposed side runs. */
static void test_float_slab_top_bottom_sides(void)
{
	const char *text =
		".\n"
		"---\n"
		".\n"
		"---\n"
		"1\n"
		"---\n"
		".\n";
	const float topUV[4][2] = ATLAS_UV_TOP;
	const float sideUV[4][2] = ATLAS_UV_SIDE;
	const float bottomUV[4][2] = ATLAS_UV_SPARE;
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;
	const DrawItem *bottom;
	const DrawItem *side;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(4, voxmapLevels(map));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 2, 0));
	initDrawList(&list, 16);
	initCamera3D(&cam);		/* yaw 0: only the +Z side is exposed */
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	/* top (y 3) + bottom (y 2) + one +Z side spanning [2, 3). */
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));
	TEST_ASSERT_EQUAL_INT(1, countUV(&list, topUV));
	TEST_ASSERT_EQUAL_INT(1, countUV(&list, bottomUV));
	TEST_ASSERT_EQUAL_INT(1, countUV(&list, sideUV));

	top = findTopPlaneAt(&list, 3.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_TRUE(itemHasUV(top, topUV));

	bottom = NULL;
	side = NULL;
	{
		size_t i;

		for (i = 0; i < drawListCount(&list); i++) {
			const DrawItem *item = drawListItem(&list, i);

			if (itemHasUV(item, bottomUV))
				bottom = item;
			if (itemHasUV(item, sideUV))
				side = item;
		}
	}
	TEST_ASSERT_NOT_NULL(bottom);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, bottom->worldQuad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, bottom->worldQuad[2][1]);
	TEST_ASSERT_NOT_NULL(side);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, side->worldQuad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, side->worldQuad[2][1]);

	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A wall with a doorway gap: the +Z side splits into a run below the gap and a
 * run above it. */
static void test_doorway_side_runs(void)
{
	const char *text =
		"1\n"
		"---\n"
		".\n"
		"---\n"
		"1\n";
	const float topUV[4][2] = ATLAS_UV_TOP;
	const float sideUV[4][2] = ATLAS_UV_SIDE;
	const float bottomUV[4][2] = ATLAS_UV_SPARE;
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	int lower = 0;
	int upper = 0;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapLevels(map));
	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	/* 2 tops (y1 over the gap block, y3 slab top) + 1 bottom (y2) + 2 +Z
	 * side runs ([0,1) and [2,3)). */
	TEST_ASSERT_EQUAL_INT(5, (int)drawListCount(&list));
	TEST_ASSERT_EQUAL_INT(2, countUV(&list, topUV));
	TEST_ASSERT_EQUAL_INT(1, countUV(&list, bottomUV));
	TEST_ASSERT_EQUAL_INT(2, countUV(&list, sideUV));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (!itemHasUV(item, sideUV))
			continue;
		if (item->worldQuad[0][1] == 0.0f &&
		    item->worldQuad[2][1] == 1.0f)
			lower++;
		if (item->worldQuad[0][1] == 2.0f &&
		    item->worldQuad[2][1] == 3.0f)
			upper++;
	}
	TEST_ASSERT_EQUAL_INT(1, lower);
	TEST_ASSERT_EQUAL_INT(1, upper);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* An interior cavity emits its walls, floor and ceiling: the centre column has
 * a top at y 1 (cavity floor) and a bottom at y 2 (cavity ceiling), and the
 * surrounding wall emits an inward-facing side run. */
static void test_interior_cavity_faces(void)
{
	const char *text =
		"111\n111\n111\n"
		"---\n"
		"111\n1.1\n111\n"
		"---\n"
		"111\n111\n111\n";
	const float sideUV[4][2] = ATLAS_UV_SIDE;
	const float bottomUV[4][2] = ATLAS_UV_SPARE;
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	const DrawItem *floorTop;
	const DrawItem *ceilingBottom = NULL;
	const DrawItem *wall = NULL;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapLevels(map));
	initDrawList(&list, 128);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));

	/* The cavity floor is the top of the (1,0,1) voxel at y 1. */
	floorTop = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(floorTop);
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (itemHasUV(item, bottomUV) &&
		    item->worldQuad[0][1] == 2.0f)
			ceilingBottom = item;
		/* The +Z-facing cavity wall: z = 1, spanning y [1, 2). */
		if (itemHasUV(item, sideUV) &&
		    item->worldQuad[0][2] == 1.0f &&
		    item->worldQuad[0][1] == 1.0f &&
		    item->worldQuad[2][1] == 2.0f)
			wall = item;
	}
	TEST_ASSERT_NOT_NULL(ceilingBottom);
	TEST_ASSERT_NOT_NULL(wall);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Bottom-face UV (the material's bottom slot) and shade (the darkest face). */
static void test_bottom_face_uv_shade(void)
{
	MaterialTable t;
	const char *text =
		"@ g 0 grass\n"
		".\n"
		"---\n"
		".\n"
		"---\n"
		"g\n"
		"---\n"
		".\n";
	float expectBottom[4][2];
	Voxmap *map;
	DrawList list;
	Camera3D cam;
	const DrawItem *bottom = NULL;
	size_t i;

	buildTestTable(&t);
	map = parseVoxmapText(text, strlen(text), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(4, voxmapLevels(map));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 2, 0));
	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, &t, NULL, &list, &cam, DRAW_TINT(200, 200, 200, 255));
	materialFaceUV(&t.items[1].rect[FACE_BOTTOM], FACE_BOTTOM, expectBottom);
	for (i = 0; i < drawListCount(&list); i++)
		if (itemHasUV(drawListItem(&list, i), expectBottom))
			bottom = drawListItem(&list, i);
	TEST_ASSERT_NOT_NULL(bottom);
	/* The slab voxel is at level 2: its bottom face is at world y 2. */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, bottom->worldQuad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, bottom->worldQuad[2][1]);
	/* Darkest face: 200 * 0.55 = 110 (even column, no checker). */
	TEST_ASSERT_EQUAL_INT(110, (int)((bottom->tint >> 24) & 0xffu));
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(110, 110, 110, 255),
			      (int)bottom->tint);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* The bottom face samples the air cell BELOW the voxel (y - 1), not the cell
 * at its own level: a lamp below lights the underside differently. */
static void test_bottom_face_light_sampling(void)
{
	const char *text =
		".\n"
		"---\n"
		".\n"
		"---\n"
		"1\n"
		"---\n"
		".\n";
	const float bottomUV[4][2] = ATLAS_UV_SPARE;
	Voxmap *map = parseText(text);
	LightGrid *g = lightGridCreate(1, 1, 4);
	DrawList list;
	Camera3D cam;
	const DrawItem *bottom = NULL;
	uint8_t below[3];
	uint8_t own[3];
	int expectedBelow;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	/* A weak lamp at the ground: level 1 reads brighter than level 2. */
	lightGridSeedPoint(g, 0.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, map);
	lightGridFactorAt(g, 0, 1, 0, below);
	lightGridFactorAt(g, 0, 2, 0, own);
	TEST_ASSERT_TRUE(below[0] != own[0]);	/* the two cells differ */

	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(200, 200, 200, 255));
	for (i = 0; i < drawListCount(&list); i++)
		if (itemHasUV(drawListItem(&list, i), bottomUV))
			bottom = drawListItem(&list, i);
	TEST_ASSERT_NOT_NULL(bottom);
	/* 200 * 0.55 * factor/255, rounded. */
	expectedBelow = (int)(200.0f * VOXMAP_SHADE_BOTTOM *
			      (float)below[0] / 255.0f + 0.5f);
	TEST_ASSERT_EQUAL_INT(expectedBelow, (int)((bottom->tint >> 24) & 0xffu));
	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Build a slice file: one section of `rows` rows of `w` '1's, then a
 * separator and a single "1" row. Caller frees. */
static char *buildWideSlice(int w, int rows)
{
	size_t n = (size_t)rows * ((size_t)w + 1) + 8;
	char *buf = malloc(n);
	size_t p = 0;
	int r;
	int x;

	for (r = 0; r < rows; r++) {
		for (x = 0; x < w; x++)
			buf[p++] = '1';
		buf[p++] = '\n';
	}
	buf[p++] = '-'; buf[p++] = '-'; buf[p++] = '-'; buf[p++] = '\n';
	buf[p++] = '1'; buf[p++] = '\n';
	buf[p] = '\0';
	return buf;
}

/* Build a slice file with `sections` sections of `d` rows of `w` '1's. */
static char *buildBigSlice(int w, int d, int sections)
{
	size_t n = (size_t)sections * ((size_t)d * ((size_t)w + 1) + 4) + 1;
	char *buf = malloc(n);
	size_t p = 0;
	int s;
	int r;
	int x;

	for (s = 0; s < sections; s++) {
		if (s > 0) {
			buf[p++] = '-'; buf[p++] = '-'; buf[p++] = '-';
			buf[p++] = '\n';
		}
		for (r = 0; r < d; r++) {
			for (x = 0; x < w; x++)
				buf[p++] = '1';
			buf[p++] = '\n';
		}
	}
	buf[p] = '\0';
	return buf;
}

/* Blank lines inside and between sections are ignored; a CRLF slice file and
 * a missing trailing newline both parse. */
static void test_slice_whitespace_and_crlf(void)
{
	Voxmap *map = parseText("1\n\n1\n---\n1\n1\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	destroyVoxmap(map);

	map = parseText("1\r\n1\r\n---\r\n1\r\n1\r\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	destroyVoxmap(map);

	/* No trailing newline on the last section row. */
	map = parseText("1\n1\n---\n1\n1");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	destroyVoxmap(map);
}

/* A middle section whose depth differs from the first is rejected. */
static void test_slice_middle_depth_mismatch(void)
{
	TEST_ASSERT_NULL(parseText("11\n11\n---\n11\n---\n11\n11\n"));
}

/* Separator lines tolerate surrounding whitespace; a dash line that is not
 * exactly three dashes is not a separator (and is then an invalid cell). */
static void test_separator_line_variants(void)
{
	Voxmap *map = parseText("1\n \t---  \t\n1\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	destroyVoxmap(map);

	TEST_ASSERT_NULL(parseText("1--\n"));	/* 3 chars, not all '-' */
	TEST_ASSERT_NULL(parseText("-1-\n"));
	TEST_ASSERT_NULL(parseText("----\n"));	/* too long */
}

/* Directive lines with leading tabs are still recognised. */
static void test_directive_leading_whitespace(void)
{
	MaterialTable t;
	Voxmap *map;

	buildTestTable(&t);
	map = parseVoxmapText("\t@ g 1 grass\ng\n",
			      strlen("\t@ g 1 grass\ng\n"), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);

	/* Leading spaces too (the other whitespace arm). */
	map = parseVoxmapText("  @ g 1 grass\ng\n",
			      strlen("  @ g 1 grass\ng\n"), &t);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAt(map, 0, 0));
	destroyVoxmap(map);

	map = parseVoxmapText("\t$ point 0.5 0.5 0.5 255 0 0\n1\n", strlen("\t$ point 0.5 0.5 0.5 255 0 0\n1\n"), NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapLightCount(map));
	destroyVoxmap(map);

	map = parseVoxmapText("  $ point 0.5 0.5 0.5 255 0 0\n1\n", strlen("  $ point 0.5 0.5 0.5 255 0 0\n1\n"), NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapLightCount(map));
	destroyVoxmap(map);
}

/* A legend line with more tokens than the table accepts is malformed. */
static void test_legend_many_tokens(void)
{
	TEST_ASSERT_NULL(parseText("@ g 1 grass a b c d e f\n1\n"));
}

/* The debug view renders a bottom face too (white UV, opaque, light tint). */
static void test_debug_bottom_face(void)
{
	const char *text =
		".\n"
		"---\n"
		".\n"
		"---\n"
		"1\n"
		"---\n"
		".\n";
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	VoxmapEmitOptions opts = { DRAW_TINT(1, 2, 3, 4), true, true, NULL };
	const DrawItem *bottom = NULL;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, NULL, &list, &cam, &opts);
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (item->worldQuad[0][1] == 2.0f &&
		    item->worldQuad[2][1] == 2.0f)
			bottom = item;
	}
	TEST_ASSERT_NOT_NULL(bottom);
	TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, bottom->alphaMode);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A slice row wider than VOXMAP_MAX_DIM, a section deeper than it, and a
 * volume over VOXMAP_MAX_CELLS all fail cleanly. */
static void test_slice_size_guards(void)
{
	char *wide = buildWideSlice(VOXMAP_MAX_DIM + 1, 1);
	char *deep = buildWideSlice(1, VOXMAP_MAX_DIM + 1);
	char *big = buildBigSlice(256, 256, 17);
	char *levels = buildBigSlice(1, 1, VOXMAP_MAX_DIM + 1);

	TEST_ASSERT_NULL(parseVoxmapText(wide, strlen(wide), NULL));
	TEST_ASSERT_NULL(parseVoxmapText(deep, strlen(deep), NULL));
	TEST_ASSERT_NULL(parseVoxmapText(big, strlen(big), NULL));
	TEST_ASSERT_NULL(parseVoxmapText(levels, strlen(levels), NULL));
	free(wide);
	free(deep);
	free(big);
	free(levels);
}

/* A high byte and a non-printable control byte are both invalid slice cells
 * (diagnostics, not crashes). */
static void test_slice_nonprintable_cells(void)
{
	TEST_ASSERT_NULL(parseText("\x01\n---\n1\n"));
	TEST_ASSERT_NULL(parseText("\x80\n---\n1\n"));
}

/* Bounds for the 3D voxel queries: every out-of-range axis, plus NULL. */
static void test_voxel_query_bounds(void)
{
	Voxmap *map = parseText("11\n11\n---\n1.\n11\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_FALSE(voxmapSolidAt(map, -1, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, -1, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 0, -1));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 2, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 2, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 0, 2));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, -1, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 0, -1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 0, 0, -1));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 2, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 0, 2, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 0, 0, 2));
	/* An air voxel reads -1 even in bounds. */
	TEST_ASSERT_EQUAL_INT(-1, voxmapMaterialAtVoxel(map, 1, 1, 0));
	destroyVoxmap(map);
}

/* The smooth path computes per-corner tints for a bottom face too. */
static void test_smooth_bottom_face(void)
{
	const char *text =
		".\n"
		"---\n"
		".\n"
		"---\n"
		"1\n"
		"---\n"
		".\n";
	const float bottomUV[4][2] = ATLAS_UV_SPARE;
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	const DrawItem *bottom = NULL;
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, false,
				   NULL };
	int k;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFacesOpt(map, NULL, NULL, &list, &cam, &opts);
	for (i = 0; i < drawListCount(&list); i++)
		if (itemHasUV(drawListItem(&list, i), bottomUV))
			bottom = drawListItem(&list, i);
	TEST_ASSERT_NOT_NULL(bottom);
	/* NULL grid: light 255, AO 100 -> 240 * 0.55 = 132 at every corner. */
	for (k = 0; k < 4; k++)
		TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(132, 132, 132, 255),
				      (int)bottom->cornerTint[k]);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* --- T17 sub-voxel shapes ---------------------------------------------- */

/* Put the camera at a 45-degree-multiple yaw (a rest step). */
static void shapeSetYaw(Camera3D *cam, float yawDeg)
{
	int steps = (int)lroundf(yawDeg / CAMERA_STEP_DEG);

	initCamera3D(cam);
	while (steps-- > 0) {
		cameraRotateStep(cam, 1);
		updateCamera3D(cam, CAMERA_TURN_SECONDS);
	}
}

static bool quadMatches(const DrawItem *item, const float q[4][3])
{
	int k;
	int c;

	for (k = 0; k < 4; k++)
		for (c = 0; c < 3; c++)
			if (fabsf(item->worldQuad[k][c] - q[k][c]) > EPS)
				return false;
	return true;
}

/* Emit `map` at `yawDeg` and copy the FIRST item whose quad matches `q` into
 * `*out`. Returns false when no face has that exact geometry. */
static bool emitFindQuad(Voxmap *map, float yawDeg, const float q[4][3],
			 DrawItem *out)
{
	DrawList list;
	Camera3D cam;
	bool found = false;
	size_t i;

	initDrawList(&list, 32);
	shapeSetYaw(&cam, yawDeg);
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	for (i = 0; i < drawListCount(&list); i++) {
		if (quadMatches(drawListItem(&list, i), q)) {
			*out = *drawListItem(&list, i);
			found = true;
			break;
		}
	}
	destroyDrawList(&list);
	return found;
}

/* A two-level slice map: level 0 is air, level 1 is `row` (a 1-wide voxel with
 * the legend's shape). */
static Voxmap *shapeSlice1(const char *legend, const char *row)
{
	static char buf[512];

	snprintf(buf, sizeof(buf), "%s.\n---\n%s\n", legend, row);
	return parseText(buf);
}

/* A single shape in a 1x1 column: query defaults + bounds. */
static void test_shape_query_defaults_and_bounds(void)
{
	Voxmap *map = loadTemp("vm_shape_def.txt", "1\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 0, 0));
	TEST_ASSERT_TRUE(voxmapFullAt(map, 0, 0, 0));
	/* Air / out of bounds / NULL: FULL (0) and no dir / not full. */
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 1, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 5, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 5, 0, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, 1, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(NULL, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(NULL, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(NULL, 0, 0, 0));
	destroyVoxmap(map);
}

/* In a heightmap a legend shape applies to EVERY voxel of the column. */
static void test_heightmap_shape_applies_to_column(void)
{
	Voxmap *map = parseText(
		"@ r 3 stone shape=ramp dir=west\n"
		"r\n");
	int y;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapLevels(map));
	for (y = 0; y < 3; y++) {
		TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_RAMP,
				      voxmapShapeAt(map, 0, y, 0));
		TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_WEST,
				      voxmapShapeDirAt(map, 0, y, 0));
		TEST_ASSERT_FALSE(voxmapFullAt(map, 0, y, 0));
	}
	destroyVoxmap(map);
}

/* A shaped height-0 legend entry is ignored: there is no voxel to shape, so the
 * ground tile stays a full flat top at y = 0. */
static void test_height_zero_shape_ignored(void)
{
	Voxmap *map = parseText(
		"@ r 0 stone shape=ramp dir=north\n"
		"r\n");
	DrawList list;
	Camera3D cam;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(0, voxmapHeightAt(map, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, 0, 0));	/* ground: no voxel */
	initDrawList(&list, 8);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	TEST_ASSERT_EQUAL_INT(1, (int)drawListCount(&list));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, drawListItem(&list, 0)->worldQuad[0][1]);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* In slice mode the shape applies per voxel. */
static void test_slice_shape_per_voxel(void)
{
	Voxmap *map = parseText(
		"@ h 1 stone shape=half\n"
		"@ e 1 stone shape=ramp dir=east\n"
		".\n"
		"---\n"
		"h\n"
		"---\n"
		"e\n");

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(3, voxmapLevels(map));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_HALF, voxmapShapeAt(map, 0, 1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 1, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_RAMP, voxmapShapeAt(map, 0, 2, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_EAST, voxmapShapeDirAt(map, 0, 2, 0));
	destroyVoxmap(map);
}

/* HALF: a bottom slab. At yaw 0 (camera +Z) the emitted faces are the 1x1 top
 * at y + 0.5, the 1x1 bottom at y, and the +Z side 0.5 high. */
static void test_half_exact_corners(void)
{
	const float top[4][3] = {
		{ 0, 1.5f, 0 }, { 1, 1.5f, 0 }, { 1, 1.5f, 1 }, { 0, 1.5f, 1 },
	};
	const float bottom[4][3] = {
		{ 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 },
	};
	const float plusZ[4][3] = {
		{ 0, 1, 1 }, { 1, 1, 1 }, { 1, 1.5f, 1 }, { 0, 1.5f, 1 },
	};
	Voxmap *map = shapeSlice1("@ H 1 stone shape=half\n", "H");
	DrawItem out;
	const float topUV[4][2] = ATLAS_UV_TOP;
	const float spareUV[4][2] = ATLAS_UV_SPARE;
	const float sideUV[4][2] = ATLAS_UV_SIDE;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, top, &out));
	TEST_ASSERT_TRUE(itemHasUV(&out, topUV));
	TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, bottom, &out));
	TEST_ASSERT_TRUE(itemHasUV(&out, spareUV));
	TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, plusZ, &out));
	TEST_ASSERT_TRUE(itemHasUV(&out, sideUV));
	destroyVoxmap(map);
}

/* One row of the ramp expected-corner tables: the rise direction, the slope
 * (always emitted), the tall-edge back face and its yaw, and the two triangles
 * with the yaws at which each is camera-facing. */
typedef struct RampCase {
	int dir;
	float slope[4][3];
	float back[4][3];
	float backYaw;
	float tri0[4][3];	/* VOXMAP_DIR_N/S: west (x0); W/E: north (z0) */
	float tri0Yaw;
	float tri1[4][3];	/* N/S: east (x1); W/E: south (z1) */
	float tri1Yaw;
} RampCase;

/* Check one ramp family (ramp or half-ramp) across all four directions; the
 * expected quads are literal, `rise` only drives nothing (kept for the doc). */
static void checkRampCase(const char *legend, const RampCase *c)
{
	const float topUV[4][2] = ATLAS_UV_TOP;
	const float sideUV[4][2] = ATLAS_UV_SIDE;
	char full[256];
	Voxmap *map;
	DrawItem out;

	/* "@ X 1 ..." -> the row is the legend's char (index 2). */
	snprintf(full, sizeof(full), "%s.\n---\n%c\n", legend, legend[2]);
	map = parseText(full);
	TEST_ASSERT_NOT_NULL(map);
	/* Slope (TOP slot) is never camera-culled. */
	TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, c->slope, &out));
	TEST_ASSERT_TRUE(itemHasUV(&out, topUV));
	/* Back face at its camera-facing yaw. */
	TEST_ASSERT_TRUE(emitFindQuad(map, c->backYaw, c->back, &out));
	TEST_ASSERT_TRUE(itemHasUV(&out, sideUV));
	/* Triangles at their camera-facing yaws: the first three UV corners
	 * are the side slot, the degenerate corner mirrors the third. */
	TEST_ASSERT_TRUE(emitFindQuad(map, c->tri0Yaw, c->tri0, &out));
	TEST_ASSERT_EQUAL_MEMORY(sideUV, out.uv, 3 * 2 * sizeof(float));
	TEST_ASSERT_EQUAL_MEMORY(&out.uv[2], &out.uv[3], 2 * sizeof(float));
	TEST_ASSERT_TRUE(emitFindQuad(map, c->tri1Yaw, c->tri1, &out));
	TEST_ASSERT_EQUAL_MEMORY(sideUV, out.uv, 3 * 2 * sizeof(float));
	destroyVoxmap(map);
}

/* RAMP dir N: slope (x,y+1,z0)->(x+1,y+1,z0)->(x+1,y,z0+1)->(x,y,z0+1); back
 * is the full 1x1 north face; west/east triangles are the degenerate quads. */
static void test_ramp_dir_north_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_NORTH,
		{ { 0, 2, 0 }, { 1, 2, 0 }, { 1, 1, 1 }, { 0, 1, 1 } },
		{ { 1, 1, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 1, 2, 0 } }, 180.0f,
		{ { 0, 1, 0 }, { 0, 2, 0 }, { 0, 1, 1 }, { 0, 1, 1 } }, 270.0f,
		{ { 1, 1, 0 }, { 1, 2, 0 }, { 1, 1, 1 }, { 1, 1, 1 } }, 90.0f,
	};
	checkRampCase("@ R 1 stone shape=ramp dir=north\n", &c);
}

static void test_ramp_dir_south_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_SOUTH,
		{ { 0, 1, 0 }, { 1, 1, 0 }, { 1, 2, 1 }, { 0, 2, 1 } },
		{ { 0, 1, 1 }, { 1, 1, 1 }, { 1, 2, 1 }, { 0, 2, 1 } }, 0.0f,
		{ { 0, 1, 1 }, { 0, 2, 1 }, { 0, 1, 0 }, { 0, 1, 0 } }, 270.0f,
		{ { 1, 1, 1 }, { 1, 2, 1 }, { 1, 1, 0 }, { 1, 1, 0 } }, 90.0f,
	};
	checkRampCase("@ R 1 stone shape=ramp dir=south\n", &c);
}

static void test_ramp_dir_west_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_WEST,
		{ { 0, 2, 0 }, { 0, 2, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
		{ { 0, 1, 1 }, { 0, 1, 0 }, { 0, 2, 0 }, { 0, 2, 1 } }, 270.0f,
		{ { 0, 1, 0 }, { 0, 2, 0 }, { 1, 1, 0 }, { 1, 1, 0 } }, 180.0f,
		{ { 0, 1, 1 }, { 0, 2, 1 }, { 1, 1, 1 }, { 1, 1, 1 } }, 0.0f,
	};
	checkRampCase("@ R 1 stone shape=ramp dir=west\n", &c);
}

static void test_ramp_dir_east_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_EAST,
		{ { 1, 2, 0 }, { 1, 2, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
		{ { 1, 1, 0 }, { 1, 1, 1 }, { 1, 2, 1 }, { 1, 2, 0 } }, 90.0f,
		{ { 1, 1, 0 }, { 1, 2, 0 }, { 0, 1, 0 }, { 0, 1, 0 } }, 180.0f,
		{ { 1, 1, 1 }, { 1, 2, 1 }, { 0, 1, 1 }, { 0, 1, 1 } }, 0.0f,
	};
	checkRampCase("@ R 1 stone shape=ramp dir=east\n", &c);
}

/* HALF_RAMP dir N: the back face is 0.5 high (y..y+0.5) and the slope drops
 * from y+0.5; triangles taper to 0 at the north edge. */
static void test_half_ramp_dir_north_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_NORTH,
		{ { 0, 1.5f, 0 }, { 1, 1.5f, 0 }, { 1, 1, 1 }, { 0, 1, 1 } },
		{ { 1, 1, 0 }, { 0, 1, 0 }, { 0, 1.5f, 0 }, { 1, 1.5f, 0 } },
		180.0f,
		{ { 0, 1, 0 }, { 0, 1.5f, 0 }, { 0, 1, 1 }, { 0, 1, 1 } },
		270.0f,
		{ { 1, 1, 0 }, { 1, 1.5f, 0 }, { 1, 1, 1 }, { 1, 1, 1 } },
		90.0f,
	};
	checkRampCase("@ r 1 stone shape=half-ramp dir=north\n", &c);
}

static void test_half_ramp_dir_south_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_SOUTH,
		{ { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1.5f, 1 }, { 0, 1.5f, 1 } },
		{ { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1.5f, 1 }, { 0, 1.5f, 1 } },
		0.0f,
		{ { 0, 1, 1 }, { 0, 1.5f, 1 }, { 0, 1, 0 }, { 0, 1, 0 } },
		270.0f,
		{ { 1, 1, 1 }, { 1, 1.5f, 1 }, { 1, 1, 0 }, { 1, 1, 0 } },
		90.0f,
	};
	checkRampCase("@ r 1 stone shape=half-ramp dir=south\n", &c);
}

static void test_half_ramp_dir_west_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_WEST,
		{ { 0, 1.5f, 0 }, { 0, 1.5f, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
		{ { 0, 1, 1 }, { 0, 1, 0 }, { 0, 1.5f, 0 }, { 0, 1.5f, 1 } },
		270.0f,
		{ { 0, 1, 0 }, { 0, 1.5f, 0 }, { 1, 1, 0 }, { 1, 1, 0 } },
		180.0f,
		{ { 0, 1, 1 }, { 0, 1.5f, 1 }, { 1, 1, 1 }, { 1, 1, 1 } },
		0.0f,
	};
	checkRampCase("@ r 1 stone shape=half-ramp dir=west\n", &c);
}

static void test_half_ramp_dir_east_exact_corners(void)
{
	static const RampCase c = {
		VOXMAP_DIR_EAST,
		{ { 1, 1.5f, 0 }, { 1, 1.5f, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
		{ { 1, 1, 0 }, { 1, 1, 1 }, { 1, 1.5f, 1 }, { 1, 1.5f, 0 } },
		90.0f,
		{ { 1, 1, 0 }, { 1, 1.5f, 0 }, { 0, 1, 0 }, { 0, 1, 0 } },
		180.0f,
		{ { 1, 1, 1 }, { 1, 1.5f, 1 }, { 0, 1, 1 }, { 0, 1, 1 } },
		0.0f,
	};
	checkRampCase("@ r 1 stone shape=half-ramp dir=east\n", &c);
}

/* The degenerate quad's UV: the triangle's first three corners carry the side
 * UV quad and uv[3] mirrors uv[2] (the degenerate corner), so the zero-area
 * half has no UV area. The rendered triangle samples that side slot. */
static void test_triangle_uv_convention(void)
{
	Voxmap *map = shapeSlice1("@ R 1 stone shape=ramp dir=north\n", "R");
	DrawList list;
	Camera3D cam;
	const float tri[4][3] = {
		{ 0, 1, 0 }, { 0, 2, 0 }, { 0, 1, 1 }, { 0, 1, 1 },
	};
	bool found = false;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	shapeSetYaw(&cam, 270.0f);	/* west triangle is camera-facing */
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *it = drawListItem(&list, i);

		if (!quadMatches(it, tri))
			continue;
		found = true;
		{
			const float side[4][2] = ATLAS_UV_SIDE;

			TEST_ASSERT_EQUAL_MEMORY(side, it->uv, 3 * 2 * sizeof(float));
		}
		TEST_ASSERT_EQUAL_MEMORY(&it->uv[2], &it->uv[3],
					 2 * sizeof(float));
	}
	TEST_ASSERT_TRUE(found);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Conservative culling: a FULL side neighbour culls a HALF's side; the
 * surface above never culls a slope or a half top (see the regression in the
 * body). */
static void test_shape_culling_full_neighbour(void)
{
	/* Regression (T17 fix round): a RAMP dir N at level 1 with a FULL at
	 * level 2 still emits its slope — the neighbour above only meets the
	 * slope at its high edge, leaving an open wedge up to 1.0 at the low
	 * edge. RED before the fix (the slope was culled). */
	{
		const char *text =
			"@ R 1 stone shape=ramp dir=north\n"
			"@ F 1 stone\n"
			".\n"
			"---\n"
			"R\n"
			"---\n"
			"F\n";
		const float slope[4][3] = {
			{ 0, 2, 0 }, { 1, 2, 0 }, { 1, 1, 1 }, { 0, 1, 1 },
		};
		const float topUV[4][2] = ATLAS_UV_TOP;
		Voxmap *map = parseText(text);
		DrawItem out;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, slope, &out));
		TEST_ASSERT_TRUE(itemHasUV(&out, topUV));
		destroyVoxmap(map);
	}
	/* HALF at (0,1,0) beside FULL at (1,1,0): the half's +X side is culled
	 * at yaw 90, the full's is emitted. */
	{
		const char *text =
			"@ H 1 stone shape=half\n"
			"@ F 1 stone\n"
			"..\n"
			"---\n"
			"HF\n";
		Voxmap *map = parseText(text);
		DrawList list;
		Camera3D cam;
		const float side[4][2] = ATLAS_UV_SIDE;
		int sideX0 = 0;
		int sides = 0;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		initDrawList(&list, 32);
		shapeSetYaw(&cam, 90.0f);
		voxmapEmitFaces(map, NULL, NULL, &list, &cam,
				DRAW_TINT(255, 255, 255, 255));
		for (i = 0; i < drawListCount(&list); i++) {
			const DrawItem *it = drawListItem(&list, i);

			if (!itemHasUV(it, side))
				continue;
			sides++;
			if (it->worldQuad[0][0] == 0.0f &&
			    it->worldQuad[0][2] == 0.0f)
				sideX0++;
		}
		TEST_ASSERT_EQUAL_INT(1, sides);
		TEST_ASSERT_EQUAL_INT(0, sideX0);
		destroyDrawList(&list);
		destroyVoxmap(map);
	}
}

/* Run merging only merges consecutive FULL voxels: a HALF between two FULL
 * voxels breaks the +Z run into two 1-high runs plus the half's own side. */
static void test_shape_run_merge_breaks_on_shape(void)
{
	const char *text =
		"@ H 1 stone shape=half\n"
		"@ F 1 stone\n"
		"F\n"
		"---\n"
		"H\n"
		"---\n"
		"F\n";
	Voxmap *map = parseText(text);
	DrawList list;
	Camera3D cam;
	const float side[4][2] = ATLAS_UV_SIDE;
	int sides = 0;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 32);
	shapeSetYaw(&cam, 0.0f);
	voxmapEmitFaces(map, NULL, NULL, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *it = drawListItem(&list, i);

		if (!itemHasUV(it, side))
			continue;
		sides++;
		/* Never a merged run taller than one voxel (the shape breaks
		 * it): every emitted side spans <= 1.0. */
		TEST_ASSERT_TRUE(it->worldQuad[2][1] - it->worldQuad[0][1] <=
				 1.0f + EPS);
	}
	TEST_ASSERT_EQUAL_INT(3, sides);	/* [0,1), [1,1.5), [2,3) */
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* Light sampling: a slope and a half top sample the cell ABOVE the voxel
 * (level y + 1). Seed that cell and check the exact flat tint. */
static void test_shape_light_samples_cell_above(void)
{
	Voxmap *map = shapeSlice1("@ R 1 stone shape=ramp dir=north\n", "R");
	LightGrid *g = lightGridCreate(1, 1, 3);
	DrawList list;
	Camera3D cam;
	uint8_t f[3];
	const DrawItem *slope = NULL;
	uint32_t expect;
	const float topUV[4][2] = ATLAS_UV_TOP;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 2.5f, 0.5f, 200.0f, 200.0f, 200.0f, 0.0f);
	lightGridPropagate(g, map);
	lightGridFactorAt(g, 0, 2, 0, f);

	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(240, 240, 240, 240));
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *it = drawListItem(&list, i);

		if (itemHasUV(it, topUV))
			slope = it;
	}
	TEST_ASSERT_NOT_NULL(slope);
	expect = DRAW_TINT(
		(uint8_t)((240.0f * (float)f[0] / 255.0f) + 0.5f),
		(uint8_t)((240.0f * (float)f[1] / 255.0f) + 0.5f),
		(uint8_t)((240.0f * (float)f[2] / 255.0f) + 0.5f), 240);
	TEST_ASSERT_EQUAL_INT((int)expect, (int)slope->tint);

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* ASCII legend shape=/dir= attributes: valid values, the two documented
 * defaults (ramp without dir -> north; dir on a non-ramp -> ignored). */
static void test_legend_shape_attributes_ascii(void)
{
	Voxmap *map;

	map = parseText("@ g 1 grass shape=ramp dir=south\ng\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_RAMP, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_SOUTH, voxmapShapeDirAt(map, 0, 0, 0));
	destroyVoxmap(map);

	/* ramp without dir -> north (diagnostic). */
	map = parseText("@ g 1 grass shape=ramp\ng\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_NORTH, voxmapShapeDirAt(map, 0, 0, 0));
	destroyVoxmap(map);

	/* dir on a non-ramp -> ignored (diagnostic); shape still applies. */
	map = parseText("@ g 1 grass shape=half dir=east\ng\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_HALF, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 0, 0));
	destroyVoxmap(map);

	/* The attributes may follow the material in any order, but the base
	 * grammar is fixed: `@ <char> <height> <material>`. Putting shape=/
	 * dir= BEFORE the material leaves the material slot holding an
	 * attribute token and `grass` an unrecognised token, so the line is
	 * malformed. */
	map = parseText("@ g 1 shape=ramp dir=west grass\ng\n");
	TEST_ASSERT_NULL(map);
	destroyVoxmap(map);
}

/* Legend validation: an unknown shape/dir VALUE skips the entry (load
 * continues and the char keeps its built-in meaning), while a non-attribute
 * token is a malformed line (load fails). */
static void test_legend_shape_malformed(void)
{
	Voxmap *map;

	/* Unknown shape value on '1' (a built-in digit): entry skipped, the
	 * digit still parses as height 1 with the default material. */
	map = parseText("@ 1 1 grass shape=banana\n1\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 0));
	destroyVoxmap(map);

	map = parseText("@ 1 1 grass shape=ramp dir=sideways\n1\n");
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 0));
	destroyVoxmap(map);

	/* A non-attribute token is a malformed legend line. */
	TEST_ASSERT_NULL(parseText("@ g 1 grass bogus\ng\n"));
	TEST_ASSERT_NULL(parseText("@ g 1 grass shape=half extra\ng\n"));
	/* T17: a 3..8-token line with a junk token now FAILS the load (the
	 * base silently ignored such tokens; only >8 was strict before). */
	TEST_ASSERT_NULL(parseText("@ g 2 grass bogus\ng\n"));
}

/* voxmapBuildRawShaped copies the shape bytes; a NULL shape grid is all FULL. */
static void test_build_raw_shaped(void)
{
	uint8_t solid[1] = { 1 };
	int16_t mats[1] = { 3 };
	uint8_t shapes[1] = { VOXMAP_SHAPE_PACK(VOXMAP_SHAPE_HALF_RAMP,
						VOXMAP_DIR_EAST) };
	Voxmap *map = voxmapBuildRawShaped(1, 1, 1, solid, mats, shapes, NULL, 0);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_HALF_RAMP, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_EAST, voxmapShapeDirAt(map, 0, 0, 0));
	destroyVoxmap(map);

	map = voxmapBuildRawShaped(1, 1, 1, solid, mats, NULL, NULL, 0);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 0));
	TEST_ASSERT_TRUE(voxmapFullAt(map, 0, 0, 0));
	destroyVoxmap(map);
}

/* Extra culling arms: shape bottoms culled by a FULL below / absent at level 0;
 * a ramp back face and a triangle culled by a FULL neighbour; a half top is
 * NOT culled by a FULL above (the 0.5 gap stays visible). */
static void test_shape_culling_extra(void)
{
	const float spare[4][2] = ATLAS_UV_SPARE;
	const float topUV[4][2] = ATLAS_UV_TOP;

	/* HALF at level 0: no bottom (world floor). */
	{
		Voxmap *map = parseText(
			"@ H 1 stone shape=half\nH\n---\n.\n");
		DrawList list;
		Camera3D cam;

		TEST_ASSERT_NOT_NULL(map);
		initDrawList(&list, 16);
		shapeSetYaw(&cam, 0.0f);
		voxmapEmitFaces(map, NULL, NULL, &list, &cam,
				DRAW_TINT(255, 255, 255, 255));
		{
			size_t i;

			for (i = 0; i < drawListCount(&list); i++)
				TEST_ASSERT_FALSE(
					itemHasUV(drawListItem(&list, i), spare));
		}
		destroyDrawList(&list);
		destroyVoxmap(map);
	}
	/* HALF at level 1 over FULL: bottom culled. */
	{
		Voxmap *map = parseText(
			"@ H 1 stone shape=half\n@ F 1 stone\nF\n---\nH\n");
		DrawList list;
		Camera3D cam;
		int bottoms = 0;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		initDrawList(&list, 16);
		shapeSetYaw(&cam, 0.0f);
		voxmapEmitFaces(map, NULL, NULL, &list, &cam,
				DRAW_TINT(255, 255, 255, 255));
		for (i = 0; i < drawListCount(&list); i++)
			if (itemHasUV(drawListItem(&list, i), spare))
				bottoms++;
		TEST_ASSERT_EQUAL_INT(0, bottoms);
		destroyDrawList(&list);
		destroyVoxmap(map);
	}
	/* RAMP back face culled by a FULL neighbour in the rise direction: a
	 * ramp at (0,1,1) dir N, with a FULL voxel at (0,1,0) (north). */
	{
		const float back[4][3] = {
			{ 1, 1, 1 }, { 0, 1, 1 }, { 0, 2, 1 }, { 1, 2, 1 },
		};
		Voxmap *withFull = parseText(
			"@ R 1 stone shape=ramp dir=north\n@ F 1 stone\n..\n..\n---\nF.\nR.\n");
		Voxmap *without = parseText(
			"@ R 1 stone shape=ramp dir=north\n..\n..\n---\n..\nR.\n");
		DrawItem out;

		TEST_ASSERT_NOT_NULL(withFull);
		TEST_ASSERT_NOT_NULL(without);
		/* Without a neighbour the back face (dir N at z1) is emitted. */
		TEST_ASSERT_TRUE(emitFindQuad(without, 180.0f, back, &out));
		/* With a FULL voxel directly north it is culled. */
		TEST_ASSERT_FALSE(emitFindQuad(withFull, 180.0f, back, &out));
		destroyVoxmap(withFull);
		destroyVoxmap(without);
	}
	/* A ramp's east triangle culled by a FULL east neighbour: ramp at
	 * (0,1,0) dir N, FULL at (1,1,0). */
	{
		const float eastTri[4][3] = {
			{ 1, 1, 0 }, { 1, 2, 0 }, { 1, 1, 1 }, { 1, 1, 1 },
		};
		Voxmap *withFull = parseText(
			"@ R 1 stone shape=ramp dir=north\n@ F 1 stone\n..\n---\nRF\n");
		Voxmap *without = parseText(
			"@ R 1 stone shape=ramp dir=north\n..\n---\nR.\n");
		DrawItem out;

		TEST_ASSERT_NOT_NULL(withFull);
		TEST_ASSERT_NOT_NULL(without);
		TEST_ASSERT_TRUE(emitFindQuad(without, 90.0f, eastTri, &out));
		TEST_ASSERT_FALSE(emitFindQuad(withFull, 90.0f, eastTri, &out));
		destroyVoxmap(withFull);
		destroyVoxmap(without);
	}
	/* Regression (T17 fix): a HALF's top is NEVER culled by a FULL above —
	 * the surface is at y + 0.5, so the neighbour (base y + 1) leaves a 0.5
	 * gap that is visible from a low side angle. RED before the fix. */
	{
		const float halfTop[4][3] = {
			{ 0, 1.5f, 0 }, { 1, 1.5f, 0 }, { 1, 1.5f, 1 },
			{ 0, 1.5f, 1 },
		};
		Voxmap *map = parseText(
			"@ H 1 stone shape=half\n@ F 1 stone\n.\n---\nH\n---\nF\n");
		DrawItem out;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, halfTop, &out));
		TEST_ASSERT_TRUE(itemHasUV(&out, topUV));
		destroyVoxmap(map);
	}
	/* Regression (T17 fix): a HALF_RAMP's slope is likewise never culled by
	 * a FULL above. */
	{
		const float slope[4][3] = {
			{ 0, 1.5f, 0 }, { 1, 1.5f, 0 }, { 1, 1, 1 },
			{ 0, 1, 1 },
		};
		Voxmap *map = parseText(
			"@ r 1 stone shape=half-ramp dir=north\n@ F 1 stone\n.\n---\nr\n---\nF\n");
		DrawItem out;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_TRUE(emitFindQuad(map, 0.0f, slope, &out));
		TEST_ASSERT_TRUE(itemHasUV(&out, topUV));
		destroyVoxmap(map);
	}
}

/* Test-local copy of voxmap's shadeChannel (multiply a 0..255 channel by a
 * factor, clamp, round to nearest). */
static uint8_t shapeShade(uint8_t c, float factor)
{
	float v = (float)c * factor;

	if (v <= 0.0f)
		return 0;
	if (v >= 255.0f)
		return 255;
	return (uint8_t)(v + 0.5f);
}

/* Per-face-type light sampling (flat path, tint 240, even tile):
 *   - HALF top samples the cell ABOVE the voxel (level y + 1);
 *   - HALF side samples its base-level side neighbour;
 *   - a ramp triangle flat-samples its side neighbour.
 * Each expected tint is computed from lightGridFactorAt at the target cell. */
static void test_shape_light_per_face_type(void)
{
	/* HALF top samples (0, 2, 0). */
	{
		Voxmap *map = shapeSlice1("@ H 1 stone shape=half\n", "H");
		LightGrid *g = lightGridCreate(1, 1, 3);
		DrawList list;
		Camera3D cam;
		uint8_t f[3];
		const float topUV[4][2] = ATLAS_UV_TOP;
		const DrawItem *top = NULL;
		uint32_t expect;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_NOT_NULL(g);
		lightGridSeedPoint(g, 0.5f, 2.5f, 0.5f, 200.0f, 200.0f, 200.0f,
				   0.0f);
		lightGridPropagate(g, map);
		lightGridFactorAt(g, 0, 2, 0, f);
		initDrawList(&list, 16);
		initCamera3D(&cam);
		voxmapEmitFaces(map, NULL, g, &list, &cam,
				DRAW_TINT(240, 240, 240, 240));
		for (i = 0; i < drawListCount(&list); i++)
			if (itemHasUV(drawListItem(&list, i), topUV))
				top = drawListItem(&list, i);
		TEST_ASSERT_NOT_NULL(top);
		expect = DRAW_TINT(shapeShade(240, (float)f[0] / 255.0f),
				   shapeShade(240, (float)f[1] / 255.0f),
				   shapeShade(240, (float)f[2] / 255.0f), 240);
		TEST_ASSERT_EQUAL_INT((int)expect, (int)top->tint);
		destroyDrawList(&list);
		destroyLightGrid(g);
		destroyVoxmap(map);
	}
	/* HALF side samples (0, 1, 1), base level 1, shade +Z 0.90. */
	{
		Voxmap *map = parseText(
			"@ H 1 stone shape=half\n..\n..\n---\nH.\n..\n");
		LightGrid *g = lightGridCreate(1, 2, 3);
		DrawList list;
		Camera3D cam;
		uint8_t f[3];
		const DrawItem *side;
		uint32_t expect;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_NOT_NULL(g);
		lightGridSeedPoint(g, 0.5f, 1.5f, 1.5f, 200.0f, 200.0f, 200.0f,
				   0.0f);
		lightGridPropagate(g, map);
		lightGridFactorAt(g, 0, 1, 1, f);
		initDrawList(&list, 16);
		initCamera3D(&cam);
		voxmapEmitFaces(map, NULL, g, &list, &cam,
				DRAW_TINT(240, 240, 240, 240));
		side = findPlusZSideAt(&list, 0);
		TEST_ASSERT_NOT_NULL(side);
		expect = DRAW_TINT(
			shapeShade(240, VOXMAP_SHADE_SIDE_PZ * (float)f[0] / 255.0f),
			shapeShade(240, VOXMAP_SHADE_SIDE_PZ * (float)f[1] / 255.0f),
			shapeShade(240, VOXMAP_SHADE_SIDE_PZ * (float)f[2] / 255.0f),
			240);
		TEST_ASSERT_EQUAL_INT((int)expect, (int)side->tint);
		/* The side spans y1..y1.5 (0.5 high). */
		TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, side->worldQuad[0][1]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, side->worldQuad[2][1]);
		(void)i;
		destroyDrawList(&list);
		destroyLightGrid(g);
		destroyVoxmap(map);
	}
	/* A ramp's east triangle flat-samples (1, 1, 0), shade +X 0.80. */
	{
		Voxmap *map = parseText(
			"@ R 1 stone shape=ramp dir=north\n..\n---\nR.\n");
		LightGrid *g = lightGridCreate(2, 1, 3);
		DrawList list;
		Camera3D cam;
		uint8_t f[3];
		const float tri[4][3] = {
			{ 1, 1, 0 }, { 1, 2, 0 }, { 1, 1, 1 }, { 1, 1, 1 },
		};
		const DrawItem *item = NULL;
		uint32_t expect;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_NOT_NULL(g);
		lightGridSeedPoint(g, 1.5f, 1.5f, 0.5f, 200.0f, 200.0f, 200.0f,
				   0.0f);
		lightGridPropagate(g, map);
		lightGridFactorAt(g, 1, 1, 0, f);
		initDrawList(&list, 16);
		shapeSetYaw(&cam, 90.0f);
		voxmapEmitFaces(map, NULL, g, &list, &cam,
				DRAW_TINT(240, 240, 240, 240));
		for (i = 0; i < drawListCount(&list); i++)
			if (quadMatches(drawListItem(&list, i), tri))
				item = drawListItem(&list, i);
		TEST_ASSERT_NOT_NULL(item);
		expect = DRAW_TINT(
			shapeShade(240, VOXMAP_SHADE_SIDE_PX * (float)f[0] / 255.0f),
			shapeShade(240, VOXMAP_SHADE_SIDE_PX * (float)f[1] / 255.0f),
			shapeShade(240, VOXMAP_SHADE_SIDE_PX * (float)f[2] / 255.0f),
			240);
		TEST_ASSERT_EQUAL_INT((int)expect, (int)item->tint);
		/* Every corner carries the same flat side sample. */
		TEST_ASSERT_EQUAL_INT((int)item->tint, (int)item->cornerTint[2]);
		destroyDrawList(&list);
		destroyLightGrid(g);
		destroyVoxmap(map);
	}
}

/* Query guard edges: each out-of-range axis, NULL, and a non-ramp dir. */
static void test_shape_query_guard_edges(void)
{
	Voxmap *map = parseText(
		"@ H 1 stone shape=half\n@ R 1 stone shape=ramp dir=south\nH\nR\n");

	TEST_ASSERT_NOT_NULL(map);
	/* (0,0,0) is a HALF: dir query returns -1 (non-ramp). */
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_DIR_SOUTH, voxmapShapeDirAt(map, 0, 0, 1));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, -1, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, -1, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, -1));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 9, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 9, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_SHAPE_FULL, voxmapShapeAt(map, 0, 0, 9));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, -1, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, -1, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 0, -1));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 9, 0, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 9, 0));
	TEST_ASSERT_EQUAL_INT(-1, voxmapShapeDirAt(map, 0, 0, 9));
	TEST_ASSERT_FALSE(voxmapFullAt(map, -1, 0, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, -1, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, 0, -1));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 9, 0, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, 9, 0));
	TEST_ASSERT_FALSE(voxmapFullAt(map, 0, 0, 9));
	destroyVoxmap(map);
}

/* A ramp's bottom is culled by a FULL below and absent at level 0. */
static void test_ramp_bottom_culling(void)
{
	const float spare[4][2] = ATLAS_UV_SPARE;
	/* Ramp at level 1 over a FULL at level 0: no bottom. */
	{
		Voxmap *map = parseText(
			"@ R 1 stone shape=ramp dir=north\n@ F 1 stone\nF\n---\nR\n");
		DrawList list;
		Camera3D cam;
		int bottoms = 0;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		initDrawList(&list, 16);
		shapeSetYaw(&cam, 0.0f);
		voxmapEmitFaces(map, NULL, NULL, &list, &cam,
				DRAW_TINT(255, 255, 255, 255));
		for (i = 0; i < drawListCount(&list); i++)
			if (itemHasUV(drawListItem(&list, i), spare))
				bottoms++;
		TEST_ASSERT_EQUAL_INT(0, bottoms);
		destroyDrawList(&list);
		destroyVoxmap(map);
	}
	/* Ramp at level 0: no bottom (world floor). */
	{
		Voxmap *map = parseText(
			"@ R 1 stone shape=ramp dir=north\nR\n---\n.\n");
		DrawList list;
		Camera3D cam;
		int bottoms = 0;
		size_t i;

		TEST_ASSERT_NOT_NULL(map);
		initDrawList(&list, 16);
		shapeSetYaw(&cam, 0.0f);
		voxmapEmitFaces(map, NULL, NULL, &list, &cam,
				DRAW_TINT(255, 255, 255, 255));
		for (i = 0; i < drawListCount(&list); i++)
			if (itemHasUV(drawListItem(&list, i), spare))
				bottoms++;
		TEST_ASSERT_EQUAL_INT(0, bottoms);
		destroyDrawList(&list);
		destroyVoxmap(map);
	}
}

/* A ramp triangle in the light-debug view uses the white debug UV and the
 * opaque alpha mode (the 1496/1497 arms). */
static void test_triangle_debug_view(void)
{
	Voxmap *map = shapeSlice1("@ R 1 stone shape=ramp dir=north\n", "R");
	const float westTri[4][3] = {
		{ 0, 1, 0 }, { 0, 2, 0 }, { 0, 1, 1 }, { 0, 1, 1 },
	};
	const float debugUV[4][2] = {
		{ 0.25f, 0.25f }, { 0.75f, 0.25f },
		{ 0.75f, 0.75f }, { 0.25f, 0.75f },
	};
	VoxmapEmitOptions opts = { DRAW_TINT(240, 240, 240, 255), true, true,
				   debugUV };
	DrawList list;
	Camera3D cam;
	bool found = false;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 32);
	shapeSetYaw(&cam, 270.0f);
	voxmapEmitFacesOpt(map, NULL, NULL, &list, &cam, &opts);
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *it = drawListItem(&list, i);

		if (!quadMatches(it, westTri))
			continue;
		found = true;
		TEST_ASSERT_TRUE(itemHasUV(it, debugUV));
		TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, it->alphaMode);
	}
	TEST_ASSERT_TRUE(found);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

void run_test_voxmap(void);

void run_test_voxmap(void)
{
	RUN_TEST(test_load_dimensions_and_heights);
	RUN_TEST(test_zero_is_solid_dot_is_void);
	RUN_TEST(test_crlf_trailing_whitespace_and_blank_lines);
	RUN_TEST(test_bounds_and_null_queries);
	RUN_TEST(test_invalid_cell_fails);
	RUN_TEST(test_malformed_maps_fail);
	RUN_TEST(test_no_trailing_newline_and_middle_blank);
	RUN_TEST(test_nonprintable_cell_fails);
	RUN_TEST(test_over_large_map_fails);
	RUN_TEST(test_parse_memory_basic);
	RUN_TEST(test_parse_memory_length_bounded);
	RUN_TEST(test_parse_memory_zero_solid_dot_void);
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
	RUN_TEST(test_legend_height_zero_is_solid);
	RUN_TEST(test_legend_high_byte_char_skipped);
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
	RUN_TEST(test_light_parse_point_and_spot);
	RUN_TEST(test_light_parse_normalizes_and_clamps);
	RUN_TEST(test_light_parse_malformed_skipped);
	RUN_TEST(test_light_parse_token_failures);
	RUN_TEST(test_light_parse_over_long_line_skipped);
	RUN_TEST(test_light_bounds_and_accessors);
	RUN_TEST(test_light_roundtrip_file_and_memory);
	RUN_TEST(test_light_lines_not_map_rows);
	RUN_TEST(test_smooth_light_corner_tints);
	RUN_TEST(test_smooth_ao_corner_tints);
	RUN_TEST(test_smooth_side_corner_tints);
	RUN_TEST(test_smooth_sides_all_directions);
	RUN_TEST(test_light_debug_view);
	RUN_TEST(test_smooth_null_grid_full_brightness);
	RUN_TEST(test_slice_parse_and_queries);
	RUN_TEST(test_slice_per_voxel_materials);
	RUN_TEST(test_slice_space_is_air);
	RUN_TEST(test_slice_malformed_sections);
	RUN_TEST(test_heightmap_and_slice_emission_identical);
	RUN_TEST(test_float_slab_top_bottom_sides);
	RUN_TEST(test_doorway_side_runs);
	RUN_TEST(test_interior_cavity_faces);
	RUN_TEST(test_bottom_face_uv_shade);
	RUN_TEST(test_bottom_face_light_sampling);
	RUN_TEST(test_slice_whitespace_and_crlf);
	RUN_TEST(test_slice_middle_depth_mismatch);
	RUN_TEST(test_slice_size_guards);
	RUN_TEST(test_slice_nonprintable_cells);
	RUN_TEST(test_voxel_query_bounds);
	RUN_TEST(test_smooth_bottom_face);
	RUN_TEST(test_separator_line_variants);
	RUN_TEST(test_directive_leading_whitespace);
	RUN_TEST(test_legend_many_tokens);
	RUN_TEST(test_debug_bottom_face);
	RUN_TEST(test_shape_query_defaults_and_bounds);
	RUN_TEST(test_heightmap_shape_applies_to_column);
	RUN_TEST(test_height_zero_shape_ignored);
	RUN_TEST(test_slice_shape_per_voxel);
	RUN_TEST(test_half_exact_corners);
	RUN_TEST(test_ramp_dir_north_exact_corners);
	RUN_TEST(test_ramp_dir_south_exact_corners);
	RUN_TEST(test_ramp_dir_west_exact_corners);
	RUN_TEST(test_ramp_dir_east_exact_corners);
	RUN_TEST(test_half_ramp_dir_north_exact_corners);
	RUN_TEST(test_half_ramp_dir_south_exact_corners);
	RUN_TEST(test_half_ramp_dir_west_exact_corners);
	RUN_TEST(test_half_ramp_dir_east_exact_corners);
	RUN_TEST(test_triangle_uv_convention);
	RUN_TEST(test_shape_culling_full_neighbour);
	RUN_TEST(test_shape_run_merge_breaks_on_shape);
	RUN_TEST(test_shape_light_samples_cell_above);
	RUN_TEST(test_legend_shape_attributes_ascii);
	RUN_TEST(test_legend_shape_malformed);
	RUN_TEST(test_build_raw_shaped);
	RUN_TEST(test_shape_culling_extra);
	RUN_TEST(test_shape_light_per_face_type);
	RUN_TEST(test_shape_query_guard_edges);
	RUN_TEST(test_ramp_bottom_culling);
	RUN_TEST(test_triangle_debug_view);
}
