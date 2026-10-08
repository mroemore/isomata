/*
 * Draw list tests (CTOL rung 1: unit + boundary).
 *
 * Pins the capacity-bounded core append, the typed voxel-face helper's pinned
 * atlas UVs, and the painter sort: far-to-near by camera-space centre depth,
 * deterministic ties (kind, then position, then stable insertion order), the
 * voxel-before-sprite rule at equal depth, and a yaw 0 vs yaw 90 case where
 * the near/far order flips.
 *
 * Pure: links drawlist.c (+ camera deps) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_drawlist().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/sprites.h"

#include <string.h>

#define EPS 1e-4f

/* A flat 1x1 quad centred on (cx, cy, cz) in the y = cy plane. */
static DrawItem makeItem(float cx, float cy, float cz, uint8_t kind,
			 uint32_t tint)
{
	const float quad[4][3] = {
		{ cx - 0.5f, cy, cz - 0.5f },
		{ cx + 0.5f, cy, cz - 0.5f },
		{ cx + 0.5f, cy, cz + 0.5f },
		{ cx - 0.5f, cy, cz + 0.5f },
	};
	const float uv[4][2] = ATLAS_UV_TOP;
	DrawItem item;

	memcpy(item.worldQuad, quad, sizeof(quad));
	memcpy(item.uv, uv, sizeof(uv));
	item.tint = tint;
	item.kind = kind;
	return item;
}

static Camera3D cameraAtYaw(float yawDeg)
{
	Camera3D camera;

	initCamera3D(&camera);
	if (yawDeg != 0.0f) {
		int steps = (int)(yawDeg / CAMERA_STEP_DEG);

		while (steps-- > 0) {
			cameraRotateStep(&camera, 1);
			updateCamera3D(&camera, CAMERA_TURN_SECONDS);
		}
	}
	return camera;
}

/* The core append is capacity-bounded and clear resets the count only. */
static void test_append_capacity_bounded(void)
{
	DrawList list;
	DrawItem item = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 7);

	initDrawList(&list, 2);
	TEST_ASSERT_EQUAL_INT(2, (int)list.capacity);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	TEST_ASSERT_TRUE(appendDrawItem(&list, &item));
	TEST_ASSERT_TRUE(appendDrawItem(&list, &item));
	TEST_ASSERT_FALSE(appendDrawItem(&list, &item));	/* full */
	TEST_ASSERT_EQUAL_INT(2, (int)drawListCount(&list));

	clearDrawList(&list);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	TEST_ASSERT_EQUAL_INT(2, (int)list.capacity);	/* storage kept */
	TEST_ASSERT_TRUE(appendDrawItem(&list, &item));

	TEST_ASSERT_NULL(drawListItem(&list, 5));
	TEST_ASSERT_EQUAL_INT(7, (int)drawListItem(&list, 0)->tint);
	destroyDrawList(&list);
}

/* NULL / unallocated / out-of-range arguments are safe. */
static void test_append_null_and_empty_list(void)
{
	DrawList list;
	DrawItem item = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 7);

	initDrawList(&list, 0);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	TEST_ASSERT_FALSE(appendDrawItem(&list, &item));
	TEST_ASSERT_FALSE(appendDrawItem(NULL, &item));
	TEST_ASSERT_FALSE(appendDrawItem(&list, NULL));
	clearDrawList(NULL);
	destroyDrawList(NULL);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(NULL));
	TEST_ASSERT_NULL(drawListItem(NULL, 0));
	destroyDrawList(&list);
}

/* The typed voxel-face helper stamps kind and the pinned atlas UVs. */
static void test_voxel_face_uvs_and_kind(void)
{
	const float quad[4][3] = {
		{ 0.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f },
		{ 1.0f, 1.0f, 1.0f },
		{ 0.0f, 1.0f, 1.0f },
	};
	const float top[4][2] = ATLAS_UV_TOP;
	const float side[4][2] = ATLAS_UV_SIDE;
	DrawList list;
	uint32_t tint = DRAW_TINT(10, 20, 30, 40);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, quad, DRAW_FACE_TOP, tint));
	TEST_ASSERT_TRUE(appendVoxelFace(&list, quad, DRAW_FACE_SIDE, tint));
	TEST_ASSERT_EQUAL_INT(2, (int)drawListCount(&list));

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_MEMORY(top, drawListItem(&list, 0)->uv, sizeof(top));
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 1)->kind);
	TEST_ASSERT_EQUAL_MEMORY(side, drawListItem(&list, 1)->uv, sizeof(side));
	TEST_ASSERT_EQUAL_INT((int)tint, (int)drawListItem(&list, 0)->tint);
	TEST_ASSERT_EQUAL_MEMORY(quad, drawListItem(&list, 0)->worldQuad,
				 sizeof(quad));

	TEST_ASSERT_FALSE(appendVoxelFace(&list, NULL, DRAW_FACE_TOP, tint));
	destroyDrawList(&list);
}

/* Far items are drawn first: at yaw 0 depth grows with world z, so the
 * smaller-z item (farther) comes first. */
static void test_sort_far_to_near(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	DrawItem near = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 111);
	DrawItem far = makeItem(0.5f, 0.0f, -1.5f, DRAW_KIND_VOXEL, 222);

	initDrawList(&list, 4);
	appendDrawItem(&list, &near);
	appendDrawItem(&list, &far);
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(222, (int)drawListItem(&list, 0)->tint);	/* far */
	TEST_ASSERT_EQUAL_INT(111, (int)drawListItem(&list, 1)->tint);	/* near */
	destroyDrawList(&list);
}

/* Equal depth: kind breaks the tie (VOXEL before SPRITE), then insertion order
 * keeps equal-kind items stable. */
static void test_sort_equal_depth_ties(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	DrawItem sprite_a = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_SPRITE, 10);
	DrawItem voxel = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 20);
	DrawItem sprite_b = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_SPRITE, 30);

	initDrawList(&list, 4);
	appendDrawItem(&list, &sprite_a);
	appendDrawItem(&list, &voxel);
	appendDrawItem(&list, &sprite_b);
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(20, (int)drawListItem(&list, 0)->tint);	/* voxel */
	TEST_ASSERT_EQUAL_INT(10, (int)drawListItem(&list, 1)->tint);	/* sprite a */
	TEST_ASSERT_EQUAL_INT(30, (int)drawListItem(&list, 2)->tint);	/* sprite b */
	destroyDrawList(&list);
}

/* A sprite appended before a same-depth voxel is reordered behind it, so the
 * transparent billboard blends over the opaque terrain. */
static void test_sort_sprite_after_voxel_at_equal_depth(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	DrawItem sprite = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_SPRITE, 1);
	DrawItem voxel = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 2);

	initDrawList(&list, 4);
	appendDrawItem(&list, &sprite);	/* appended first */
	appendDrawItem(&list, &voxel);
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(2, (int)drawListItem(&list, 0)->tint);	/* voxel */
	TEST_ASSERT_EQUAL_INT(1, (int)drawListItem(&list, 1)->tint);	/* sprite */
	destroyDrawList(&list);
}

/* Exact duplicates keep insertion order (stable sort). */
static void test_sort_is_stable_for_identical_keys(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	DrawItem a = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 1);
	DrawItem b = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 2);
	DrawItem c = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 3);
	size_t i;

	initDrawList(&list, 4);
	appendDrawItem(&list, &a);
	appendDrawItem(&list, &b);
	appendDrawItem(&list, &c);
	sortDrawList(&list, &camera);

	for (i = 0; i < 3; i++)
		TEST_ASSERT_EQUAL_INT((int)(i + 1),
				      (int)drawListItem(&list, i)->tint);
	destroyDrawList(&list);
}

/* Rotating the camera 90 degrees flips which item is nearer: A (small x, large
 * z) is nearer at yaw 0 but farther at yaw 90. */static void test_sort_yaw_flip(void)
{
	DrawList list;
	Camera3D yaw0 = cameraAtYaw(0.0f);
	Camera3D yaw90 = cameraAtYaw(90.0f);
	DrawItem a = makeItem(0.5f, 0.0f, 2.5f, DRAW_KIND_VOXEL, 1);
	DrawItem b = makeItem(2.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 2);

	initDrawList(&list, 4);
	appendDrawItem(&list, &a);
	appendDrawItem(&list, &b);
	sortDrawList(&list, &yaw0);
	TEST_ASSERT_EQUAL_INT(2, (int)drawListItem(&list, 0)->tint);	/* b far */
	TEST_ASSERT_EQUAL_INT(1, (int)drawListItem(&list, 1)->tint);	/* a near */

	/* Re-append in the original order and sort at yaw 90. */
	clearDrawList(&list);
	appendDrawItem(&list, &a);
	appendDrawItem(&list, &b);
	sortDrawList(&list, &yaw90);
	TEST_ASSERT_EQUAL_INT(1, (int)drawListItem(&list, 0)->tint);	/* a far */
	TEST_ASSERT_EQUAL_INT(2, (int)drawListItem(&list, 1)->tint);	/* b near */
	destroyDrawList(&list);
}

/* At yaw 0 depth does not depend on x, so two same-kind items with equal depth
 * fall through to the position tie-break: smaller centre x first. */
static void test_sort_position_tiebreak_by_x(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	DrawItem right = makeItem(2.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 1);
	DrawItem left = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 2);

	initDrawList(&list, 4);
	appendDrawItem(&list, &right);
	appendDrawItem(&list, &left);
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(2, (int)drawListItem(&list, 0)->tint);	/* x 0.5 */
	TEST_ASSERT_EQUAL_INT(1, (int)drawListItem(&list, 1)->tint);	/* x 2.5 */
	destroyDrawList(&list);
}

/* Sorting a NULL/empty list is a no-op, including an unallocated list. */
static void test_sort_null_safe(void)
{
	DrawList list;
	DrawList unallocated = { NULL, 0, 0 };

	sortDrawList(NULL, NULL);
	initDrawList(NULL, 4);		/* NULL list is safe */
	initDrawList(&list, 4);
	sortDrawList(&list, NULL);
	TEST_ASSERT_EQUAL_INT(0, (int)drawListCount(&list));
	sortDrawList(&unallocated, NULL);	/* items == NULL */
	destroyDrawList(&list);
}

/* The sprite depth bias keeps a billboard in front of a coplanar tile face.
 * A face whose centre is 0.01 world units CLOSER than the sprite's anchor
 * would otherwise draw after the sprite (its view depth is larger); the
 * 0.02 bias puts the sprite back in front. This is the flicker regression:
 * without the bias the face is closer and draws last. */
static void test_sort_sprite_biased_over_coplanar_face(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	Mat4 view = cameraView(&camera);
	SpriteEntity sprite = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
				DRAW_TINT(255, 255, 255, 255) };
	/* Offset the face centre along the camera-backward axis (row 2 of the
	 * view, unit length) so its view depth is ~0.01 nearer than the
	 * origin the sprite stands on. */
	DrawItem face = makeItem(0.01f * view.m[2], 0.01f * view.m[6],
				 0.01f * view.m[10], DRAW_KIND_VOXEL, 77);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendDrawItem(&list, &face));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
}

/* No overreach: a face genuinely 0.5 units in front of the sprite (far
 * beyond the 0.02 bias) still draws after the sprite. */
static void test_sort_bias_does_not_hide_a_nearer_face(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	Mat4 view = cameraView(&camera);
	SpriteEntity sprite = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
				DRAW_TINT(255, 255, 255, 255) };
	DrawItem face = makeItem(0.5f * view.m[2], 0.5f * view.m[6],
				 0.5f * view.m[10], DRAW_KIND_VOXEL, 77);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendDrawItem(&list, &face));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
}

/* Real geometry: a billboard standing on a tile centre (its anchor is the
 * tile top's centre) must draw after that tile's top face for every camera
 * configuration — yaws, zooms and targets. */
static void test_sort_sprite_over_tile_top_sweep(void)
{
	static const float yaws[] = { 0.0f, 45.0f, 90.0f, 135.0f, 180.0f,
				      225.0f, 270.0f, 315.0f };
	static const float zoomAmounts[] = { -100.0f, 0.0f, 100.0f };
	static const float panX[] = { 0.0f, 2.5f, -3.0f };
	static const float panY[] = { 0.0f, 2.5f, 7.0f };
	const float tileTop[4][3] = {
		{ 2.0f, 1.0f, 2.0f }, { 3.0f, 1.0f, 2.0f },
		{ 3.0f, 1.0f, 3.0f }, { 2.0f, 1.0f, 3.0f },
	};
	SpriteEntity sprite = { 2.5f, 1.0f, 2.5f, 1.2f, 1.8f,
				DRAW_TINT(255, 255, 255, 255) };
	size_t i;
	size_t j;
	size_t k;

	for (i = 0; i < sizeof(yaws) / sizeof(yaws[0]); i++) {
		for (j = 0; j < sizeof(zoomAmounts) / sizeof(zoomAmounts[0]); j++) {
			for (k = 0; k < sizeof(panX) / sizeof(panX[0]); k++) {
				DrawList list;
				Camera3D camera = cameraAtYaw(yaws[i]);

				cameraZoom(&camera, zoomAmounts[j]);
				cameraPan(&camera, panX[k], panY[k]);
				initDrawList(&list, 4);
				TEST_ASSERT_TRUE(appendVoxelFace(
					&list, tileTop, DRAW_FACE_TOP,
					DRAW_TINT(10, 20, 30, 40)));
				TEST_ASSERT_TRUE(
					appendSprite(&list, &sprite, &camera));
				sortDrawList(&list, &camera);
				TEST_ASSERT_EQUAL_INT(
					DRAW_KIND_VOXEL,
					drawListItem(&list, 0)->kind);
				TEST_ASSERT_EQUAL_INT(
					DRAW_KIND_SPRITE,
					drawListItem(&list, 1)->kind);
				destroyDrawList(&list);
			}
		}
	}
}

void run_test_drawlist(void);

void run_test_drawlist(void)
{
	RUN_TEST(test_append_capacity_bounded);
	RUN_TEST(test_append_null_and_empty_list);
	RUN_TEST(test_voxel_face_uvs_and_kind);
	RUN_TEST(test_sort_far_to_near);
	RUN_TEST(test_sort_equal_depth_ties);
	RUN_TEST(test_sort_sprite_after_voxel_at_equal_depth);
	RUN_TEST(test_sort_is_stable_for_identical_keys);
	RUN_TEST(test_sort_yaw_flip);
	RUN_TEST(test_sort_position_tiebreak_by_x);
	RUN_TEST(test_sort_null_safe);
	RUN_TEST(test_sort_sprite_biased_over_coplanar_face);
	RUN_TEST(test_sort_bias_does_not_hide_a_nearer_face);
	RUN_TEST(test_sort_sprite_over_tile_top_sweep);
}
