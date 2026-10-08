/*
 * Frame draw-list construction tests (CTOL rung 1: unit + boundary).
 *
 * Pins buildFrameDrawList: clear -> voxel faces -> sprites -> painter sort,
 * the NULL-argument rules, the opaque-white voxel tint, and that the result
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
#include "render/math3d.h"
#include "render/sprites.h"
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
	return loadVoxmap(path);
}

static Camera3D yaw0Camera(void)
{
	Camera3D camera;

	initCamera3D(&camera);
	return camera;
}

/* View-space z of an item's centre (farther = more negative). */
static float viewZ(const DrawItem *item, const Camera3D *camera)
{
	float cx = 0.0f;
	float cy = 0.0f;
	float cz = 0.0f;
	Mat4 view = cameraView(camera);
	Vec4 v;
	int k;

	for (k = 0; k < 4; k++) {
		cx += item->worldQuad[k][0];
		cy += item->worldQuad[k][1];
		cz += item->worldQuad[k][2];
	}
	v = mat4TransformPoint(&view, (Vec3){ cx / 4.0f, cy / 4.0f, cz / 4.0f });
	return v.z;
}

static SpriteEntity demoSprites[3] = {
	{ 1.5f, 0.0f, 1.5f, 1.0f, 1.5f, DRAW_TINT(255, 0, 0, 255) },
	{ 0.5f, 1.0f, 2.5f, 1.0f, 1.5f, DRAW_TINT(0, 255, 0, 255) },
	{ 2.5f, 2.0f, 0.5f, 1.0f, 1.5f, DRAW_TINT(0, 0, 255, 255) },
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

	TEST_ASSERT_FALSE(buildFrameDrawList(NULL, demoSprites, 3, &camera, NULL));
	TEST_ASSERT_FALSE(buildFrameDrawList(NULL, NULL, 0, NULL, NULL));
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
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, demoSprites, 3, &camera, &list));
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));

	for (i = 0; i < 3; i++)
		appendSprite(&ref, &demoSprites[i], &camera);
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

	TEST_ASSERT_TRUE(buildFrameDrawList(map, demoSprites, 3, &camera, &list));

	voxmapEmitFaces(map, &ref, &camera, FRAME_VOXEL_TINT);
	for (i = 0; i < 3; i++)
		appendSprite(&ref, &demoSprites[i], &camera);
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

/* The top face of an even tile carries the pinned opaque-white tint (face
 * shade 1.0, no checker boost); sprites keep their own. Side faces and odd
 * tiles are shaded by voxmap (see test_voxmap). */
static void test_voxel_tint_pinned(void)
{
	Voxmap *map = loadTemp("frame_tint.txt", "1\n");
	DrawList list;
	Camera3D camera = yaw0Camera();
	const DrawItem *item;

	TEST_ASSERT_NOT_NULL(map);
	initDrawList(&list, 16);
	TEST_ASSERT_TRUE(buildFrameDrawList(map, NULL, 0, &camera, &list));
	TEST_ASSERT_TRUE(drawListCount(&list) > 0);
	item = drawListItem(&list, 0);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, item->kind);
	TEST_ASSERT_EQUAL_INT((int)FRAME_VOXEL_TINT, (int)item->tint);
	destroyDrawList(&list);
	destroyVoxmap(map);
}

/* A NULL camera sorts against the identity view and still builds. */
static void test_null_camera_and_sprites(void)
{
	DrawList list;

	initDrawList(&list, 8);
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, demoSprites, 3, NULL, &list));
	TEST_ASSERT_EQUAL_INT(3, (int)drawListCount(&list));

	clearDrawList(&list);
	TEST_ASSERT_TRUE(buildFrameDrawList(NULL, NULL, 5, NULL, &list));
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	destroyDrawList(&list);
}

void run_test_frame(void);

void run_test_frame(void)
{
	RUN_TEST(test_null_list_refused);
	RUN_TEST(test_sprites_only);
	RUN_TEST(test_map_and_sprites_sorted_and_composed);
	RUN_TEST(test_voxel_tint_pinned);
	RUN_TEST(test_null_camera_and_sprites);
}
