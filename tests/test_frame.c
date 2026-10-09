/*
 * Frame draw-list construction tests (CTOL rung 1: unit + boundary).
 *
 * Pins buildFrameDrawList: clear -> voxel faces -> sprites -> painter sort,
 * the NULL-argument rules, the voxel tint (FRAME_VOXEL_TINT, 240 to leave
 * the checker boost headroom), and that the result
 * is exactly the primitives' composition (count and per-item equality
 * against a reference list built by hand) and is painter-sorted (view-space
 * depth is non-decreasing).
 *
 * Pure: links only frame.c (+ its pure deps) and the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_frame().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/frame.h"
#include "render/lightgrid.h"
#include "render/math3d.h"
#include "render/sprites.h"
#include "render/textures.h"
#include "render/voxmap.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define EPS 1e-4f

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

static Camera3D yaw0Camera(void)
{
	Camera3D camera;

	initCamera3D(&camera);
	return camera;
}

/* View-space z of an item's painter KEY point (farther = more negative).
 * Mirrors drawlist.c's key rule: a vertical voxel face keys on the midpoint of
 * its two lowest corners (its base line), everything else on its centre. */
static float viewZ(const DrawItem *item, const Camera3D *camera)
{
	float kx = 0.0f;
	float ky = 0.0f;
	float kz = 0.0f;
	float minY = item->worldQuad[0][1];
	float maxY = item->worldQuad[0][1];
	Mat4 view = cameraView(camera);
	Vec4 v;
	int k;

	for (k = 1; k < 4; k++) {
		if (item->worldQuad[k][1] < minY)
			minY = item->worldQuad[k][1];
		if (item->worldQuad[k][1] > maxY)
			maxY = item->worldQuad[k][1];
	}
	if (item->kind == DRAW_KIND_VOXEL && (maxY - minY) > 1e-4f) {
		int lo0 = 0;
		int lo1 = 1;
		int i;

		if (item->worldQuad[lo1][1] < item->worldQuad[lo0][1]) {
			lo0 = 1;
			lo1 = 0;
		}
		for (i = 2; i < 4; i++) {
			float y = item->worldQuad[i][1];

			if (y < item->worldQuad[lo0][1]) {
				lo1 = lo0;
				lo0 = i;
			} else if (y < item->worldQuad[lo1][1]) {
				lo1 = i;
			}
		}
		kx = (item->worldQuad[lo0][0] + item->worldQuad[lo1][0]) * 0.5f;
		ky = (item->worldQuad[lo0][1] + item->worldQuad[lo1][1]) * 0.5f;
		kz = (item->worldQuad[lo0][2] + item->worldQuad[lo1][2]) * 0.5f;
	} else {
		for (k = 0; k < 4; k++) {
			kx += item->worldQuad[k][0];
			ky += item->worldQuad[k][1];
			kz += item->worldQuad[k][2];
		}
		kx *= 0.25f;
		ky *= 0.25f;
		kz *= 0.25f;
	}
	v = mat4TransformPoint(&view, (Vec3){ kx, ky, kz });
	return v.z;
}

static SpriteEntity demoSprites[3] = {
	{ 1.5f, 0.0f, 1.5f, 1.0f, 1.5f, DRAW_TINT(255, 0, 0, 255), -1 },
	{ 0.5f, 1.0f, 2.5f, 1.0f, 1.5f, DRAW_TINT(0, 255, 0, 255), -1 },
	{ 2.5f, 2.0f, 0.5f, 1.0f, 1.5f, DRAW_TINT(0, 0, 255, 255), -1 },
};

/* Compare the meaningful DrawItem fields (padding bytes are not part of the
 * contract and differ between independently built items). */
static void assertItemsEqual(const DrawItem *a, const DrawItem *b)
{
	TEST_ASSERT_EQUAL_MEMORY(a->worldQuad, b->worldQuad, sizeof(a->worldQuad));
	TEST_ASSERT_EQUAL_MEMORY(a->uv, b->uv, sizeof(a->uv));
	TEST_ASSERT_EQUAL_INT((int)a->tint, (int)b->tint);
	TEST_ASSERT_EQUAL_INT(a->kind, b->kind);
}

/* A NULL list is refused; nothing else is required to be non-NULL. */
static void test_null_list_refused(void)
{
	Camera3D camera = yaw0Camera();

	TEST_ASSERT_FALSE(buildFrameDrawList(NULL, NULL, NULL, demoSprites, 3, &camera, NULL, NULL));
	TEST_ASSERT_FALSE(buildFrameDrawList(NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL));
}

/* With no map, the list is exactly the appended sprites. */
static void test_sprites_only(void)
{
	DrawList list;
	DrawList ref;
	Camera3D camera = yaw0Camera();
	size_t i;

	initDrawList(&list, 8);
	initDrawList(&ref, 8);
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, NULL, demoSprites, 3, &camera, &list, NULL));
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));

	for (i = 0; i < 3; i++)
		appendSprite(&ref, &demoSprites[i], &camera, NULL);
	sortDrawList(&ref, &camera);
	TEST_ASSERT_EQUAL_INT((int)drawListCount(&ref), (int)drawListCount(&list));
	for (i = 0; i < 3; i++)
		assertItemsEqual(drawListItem(&ref, i), drawListItem(&list, i));

	destroyDrawList(&list);
	destroyDrawList(&ref);
}

/* Map faces and sprites both land, in painter order, matching a reference
 * list built from the primitives. */
static void test_map_and_sprites_sorted_and_composed(void)
{
	Voxmap *map = loadTemp("frame_map.txt", "12\n34\n");
	DrawList list;
	DrawList ref;
	Camera3D camera = yaw0Camera();
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 128);
	initDrawList(&ref, 128);

	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, NULL, demoSprites, 3, &camera, &list, NULL));

	voxmapEmitFaces(map, NULL, NULL, &ref, &camera, FRAME_VOXEL_TINT);
	for (i = 0; i < 3; i++)
		appendSprite(&ref, &demoSprites[i], &camera, NULL);
	sortDrawList(&ref, &camera);

	TEST_ASSERT_EQUAL_INT((int)drawListCount(&ref), (int)drawListCount(&list));
	for (i = 0; i < drawListCount(&ref); i++)
		assertItemsEqual(drawListItem(&ref, i), drawListItem(&list, i));

	/* Painter order: view-space depth is non-decreasing. */
	for (i = 1; i < drawListCount(&list); i++) {
		float prev = viewZ(drawListItem(&list, i - 1), &camera);
		float cur = viewZ(drawListItem(&list, i), &camera);

		TEST_ASSERT_TRUE(prev <= cur + EPS);
	}

	destroyDrawList(&list);
	destroyDrawList(&ref);
	destroyVoxmap(map);
}

/* Voxel faces carry the pinned frame tint (below white so the voxmap shade
 * and checker boost have headroom); sprites keep their own. */
static void test_voxel_tint_pinned(void)
{
	Voxmap *map = loadTemp("frame_tint.txt", "1\n");
	DrawList list;
	Camera3D camera = yaw0Camera();
	const float topUV[4][2] = ATLAS_UV_TOP;
	const DrawItem *top = NULL;
	size_t i;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, NULL, NULL, 0, &camera, &list, NULL));
	TEST_ASSERT_TRUE(drawListCount(&list) > 0);
	/* The lone height-1 cell emits its top face (shade 1.0) and its +Z side.
	 * Locate the top by atlas UV rather than assuming index 0: the base-line
	 * key now sorts the side face (base y=0) ahead of the top (centre y=1). */
	for (i = 0; i < drawListCount(&list); i++) {
		const DrawItem *item = drawListItem(&list, i);

		if (memcmp(item->uv, topUV, sizeof(topUV)) == 0) {
			top = item;
			break;
		}
	}
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, top->kind);
	TEST_ASSERT_EQUAL_INT((int)FRAME_VOXEL_TINT, (int)top->tint);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A NULL camera sorts against the identity view and still builds. */
static void test_null_camera_and_sprites(void)
{
	DrawList list;

	initDrawList(&list, 8);
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, NULL, demoSprites, 3, NULL, &list, NULL));
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));

	clearDrawList(&list);
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, NULL, NULL, 5, NULL, &list, NULL));
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	destroyDrawList(&list);
}

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

/* FrameOptions reach the emitter: NULL is flat (uniform tint), smooth gives
 * per-corner tints, and the debug view swaps in the white UV. */
static void test_frame_options_plumbed(void)
{
	Voxmap *map = loadTemp("frame_opts.txt", "11\n");
	LightGrid *g = lightGridCreate(2, 1, 2);
	DrawList list;
	Camera3D camera = yaw0Camera();
	FrameOptions opts = { true, false, NULL };
	const float debugUV[4][2] = {
		{ 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.9f, 0.9f }, { 0.1f, 0.9f }
	};
	const DrawItem *top;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, map);
	initDrawList(&list, 64);

	/* Flat (NULL options): the top face is uniform 142. */
	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, g, NULL, 0, &camera, &list,
					    NULL));
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(142, 142, 142, 255),
			      (int)top->tint);

	/* Smooth: corner 0 = 102, corner 1 = 111. */
	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, g, NULL, 0, &camera, &list,
					    &opts));
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(102, 102, 102, 255),
			      (int)top->cornerTint[0]);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(111, 111, 111, 255),
			      (int)top->cornerTint[1]);

	/* Debug: the white UV replaces the material UV. */
	opts.lightDebug = true;
	opts.debugUV = debugUV;
	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, g, NULL, 0, &camera, &list,
					    &opts));
	top = findTopPlaneAt(&list, 1.0f);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_MEMORY(debugUV, top->uv, sizeof(debugUV));

	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

/* Sprites are lit by the flat factor at their base cell; a NULL grid leaves
 * the tint unchanged. */
static void test_frame_lights_sprites(void)
{
	LightGrid *g = lightGridCreate(3, 1, 2);
	SpriteEntity s = { 0.5f, 1.0f, 0.5f, 1.0f, 1.0f,
			   DRAW_TINT(200, 100, 50, 128), -1 };
	DrawList list;
	Camera3D camera = yaw0Camera();

	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, NULL);	/* all air */
	initDrawList(&list, 4);

	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, g, &s, 1, &camera, &list,
					    NULL));
	TEST_ASSERT_EQUAL_INT(1, (int)drawListCount(&list));
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(118, 59, 30, 128),
			      (int)drawListItem(&list, 0)->tint);
	TEST_ASSERT_EQUAL_INT((int)drawListItem(&list, 0)->tint,
			      (int)drawListItem(&list, 0)->cornerTint[2]);

	/* NULL grid: the sprite keeps its own tint. */
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, NULL, &s, 1, &camera,
					    &list, NULL));
	TEST_ASSERT_EQUAL_INT((int)s.tint, (int)drawListItem(&list, 0)->tint);

	destroyDrawList(&list);
	destroyLightGrid(g);
}

void run_test_frame(void);

void run_test_frame(void)
{
	RUN_TEST(test_null_list_refused);
	RUN_TEST(test_sprites_only);
	RUN_TEST(test_map_and_sprites_sorted_and_composed);
	RUN_TEST(test_voxel_tint_pinned);
	RUN_TEST(test_null_camera_and_sprites);
	RUN_TEST(test_frame_options_plumbed);
	RUN_TEST(test_frame_lights_sprites);
}
