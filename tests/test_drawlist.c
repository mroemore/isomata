/*
 * Draw list tests (CTOL rung 1: unit + boundary).
 *
 * Pins the capacity-bounded core append, the typed voxel-face helper's pinned
 * atlas UVs, and the painter sort: far-to-near by camera-space depth of the
 * sort key (a vertical face's base line, otherwise the quad centre),
 * deterministic ties (kind, then position, then stable insertion order), the
 * voxel-before-sprite rule at equal depth, a yaw 0 vs yaw 90 case where the
 * near/far order flips, and the demo tower/sprite base-line cases.
 *
 * Pure: links drawlist.c (+ camera deps) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_drawlist().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/materials.h"
#include "render/sprites.h"
#include "render/textures.h"

#include <string.h>

#define EPS 1e-4f

/* Pinned UV quads for the typed-helper tests (fallback 2x2 atlas). */
static const float kTopUV[4][2] = ATLAS_UV_TOP;
static const float kSideUV[4][2] = ATLAS_UV_SIDE;

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
	item.alphaMode = ALPHA_OPAQUE;
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
	TEST_ASSERT_TRUE(appendVoxelFace(&list, quad, kTopUV, ALPHA_OPAQUE,
					 tint));
	TEST_ASSERT_TRUE(appendVoxelFace(&list, quad, kSideUV, ALPHA_CUTOUT,
					 tint));
	TEST_ASSERT_EQUAL_INT(2, (int)drawListCount(&list));

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_MEMORY(top, drawListItem(&list, 0)->uv, sizeof(top));
	TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, drawListItem(&list, 0)->alphaMode);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 1)->kind);
	TEST_ASSERT_EQUAL_MEMORY(side, drawListItem(&list, 1)->uv, sizeof(side));
	TEST_ASSERT_EQUAL_INT(ALPHA_CUTOUT, drawListItem(&list, 1)->alphaMode);
	TEST_ASSERT_EQUAL_INT((int)tint, (int)drawListItem(&list, 0)->tint);
	TEST_ASSERT_EQUAL_MEMORY(quad, drawListItem(&list, 0)->worldQuad,
				 sizeof(quad));

	TEST_ASSERT_FALSE(appendVoxelFace(&list, NULL, kTopUV, ALPHA_OPAQUE,
					  tint));
	TEST_ASSERT_FALSE(appendVoxelFace(&list, quad, NULL, ALPHA_OPAQUE,
					  tint));
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

/* Same raw centre: the sprite depth bias puts the sprite after the coplanar
 * voxel (VOXEL before SPRITE), then insertion order keeps equal-kind items
 * stable. After the bias these keys are no longer exactly equal, so this
 * pins the bias ordering, NOT the kind tie-break arm — that arm is only
 * reachable for keys equal after the bias. */
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

/* A sprite appended before a coplanar voxel is reordered behind it (the
 * sprite bias moves it toward the camera), so the transparent billboard
 * blends over the opaque terrain. */
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

/* Equal depth, equal kind and equal key x fall through to the key-point y
 * tie-break: with the identity view (NULL camera) two flat quads at the same z
 * have identical depth, so the smaller key y draws first. (This is the arm the
 * tie-break uses now that it keys on the same point as the depth.) */
static void test_sort_key_point_tiebreak_by_y(void)
{
	DrawList list;
	DrawItem low = makeItem(0.5f, 0.0f, 0.5f, DRAW_KIND_VOXEL, 1);
	DrawItem high = makeItem(0.5f, 2.0f, 0.5f, DRAW_KIND_VOXEL, 2);

	initDrawList(&list, 4);
	appendDrawItem(&list, &high);
	appendDrawItem(&list, &low);
	sortDrawList(&list, NULL);	/* identity view: depth == z */

	TEST_ASSERT_EQUAL_INT(1, (int)drawListItem(&list, 0)->tint);	/* y 0 */
	TEST_ASSERT_EQUAL_INT(2, (int)drawListItem(&list, 1)->tint);	/* y 2 */
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
				DRAW_TINT(255, 255, 255, 255), -1 };
	/* Offset the face centre along the camera-backward axis (row 2 of the
	 * view, unit length) so its view depth is ~0.01 nearer than the
	 * origin the sprite stands on. */
	DrawItem face = makeItem(0.01f * view.m[2], 0.01f * view.m[6],
				 0.01f * view.m[10], DRAW_KIND_VOXEL, 77);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendDrawItem(&list, &face));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
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
				DRAW_TINT(255, 255, 255, 255), -1 };
	DrawItem face = makeItem(0.5f * view.m[2], 0.5f * view.m[6],
				 0.5f * view.m[10], DRAW_KIND_VOXEL, 77);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendDrawItem(&list, &face));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
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
				DRAW_TINT(255, 255, 255, 255), -1 };
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
					&list, tileTop, kTopUV, ALPHA_OPAQUE,
					DRAW_TINT(10, 20, 30, 40)));
				TEST_ASSERT_TRUE(
					appendSprite(&list, &sprite, &camera, NULL));
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

/* Real geometry regression: the demo's sprite 1 near the tower (world
 * (6.5, 2.0, 9.5), src/scenes/level_scene.c) must stand IN FRONT of the
 * tower's +Z face at yaw 0. The tower cells are cols 6-7 rows 7-8 (height 9
 * on the height-2 plateau), so that face is the plane z=9 spanning x 6..8,
 * y 2..9.
 *
 * Depth is camera closeness (the dot of a point with the toward-camera axis);
 * at yaw 0 that axis gives +0.816 per +z and +0.577 per +y:
 *   face centre (7, 5.5, 9)  -> 0.816*9   + 0.577*5.5 = 10.52
 *   face base   (7, 2,   9)  -> 0.816*9   + 0.577*2   =  8.50
 *   sprite anchor (6.5,2,9.5) + bias 0.02 -> 0.816*9.5 + 0.577*2 + 0.02 = 8.93
 *
 * On the OLD centre key the face (10.52) sorts nearer than the sprite (8.93),
 * so it draws last and covers it (RED). On the base-line key the face (8.50)
 * sorts farther, the sprite draws last and is visible: its base is 0.5 world
 * units in front of the face plane. */
static void test_sort_tower_face_base_line_sprite_in_front_yaw0(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	const float towerFace[4][3] = {
		{ 6.0f, 2.0f, 9.0f }, { 8.0f, 2.0f, 9.0f },
		{ 8.0f, 9.0f, 9.0f }, { 6.0f, 9.0f, 9.0f },
	};
	SpriteEntity sprite = { 6.5f, 2.0f, 9.5f, 1.2f, 1.8f,
				DRAW_TINT(255, 255, 255, 255), -1 };

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, towerFace, kSideUV, ALPHA_OPAQUE,
					 DRAW_TINT(240, 240, 240, 255)));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
}

/* The same tower/sprite pair with the camera on the far side (yaw 180). Now
 * the tower's north -Z face (plane z=7, base (7,2,7)) genuinely interposes and
 * must keep drawing last. Closeness at yaw 180 is +0.577 per +y and -0.816
 * per +z:
 *   face base (7, 2, 7)      -> 0.577*2 - 0.816*7   = -4.56
 *   sprite (6.5, 2, 9.5) + bias -> 0.577*2 - 0.816*9.5 + 0.02 = -6.58
 * The face (-4.56) is nearer than the sprite (-6.58), so it draws last and
 * occludes — the fix must not break this case. */
static void test_sort_tower_face_base_line_still_occludes_yaw180(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(180.0f);
	const float towerNorthFace[4][3] = {
		{ 8.0f, 2.0f, 7.0f }, { 6.0f, 2.0f, 7.0f },
		{ 6.0f, 9.0f, 7.0f }, { 8.0f, 9.0f, 7.0f },
	};
	SpriteEntity sprite = { 6.5f, 2.0f, 9.5f, 1.2f, 1.8f,
				DRAW_TINT(255, 255, 255, 255), -1 };

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, towerNorthFace, kSideUV, ALPHA_OPAQUE,
					 DRAW_TINT(240, 240, 240, 255)));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
}

/* The base-line key must not over-reach: a ground tile that really is in
 * front of the face still sorts after it. The plateau top at grid (6,10)
 * (centre (6.5, 2, 10.5)) is +Z of the face plane z=9; at yaw 0 its closeness
 * 0.816*10.5 + 0.577*2 = 9.72 exceeds the face base 8.50, so the tile draws
 * last (in front). Both items are voxels, so the tints discriminate them. */
static void test_sort_tower_face_base_line_keeps_ground_tile_in_front(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	const float towerFace[4][3] = {
		{ 6.0f, 2.0f, 9.0f }, { 8.0f, 2.0f, 9.0f },
		{ 8.0f, 9.0f, 9.0f }, { 6.0f, 9.0f, 9.0f },
	};
	const float tileTop[4][3] = {
		{ 6.0f, 2.0f, 10.0f }, { 7.0f, 2.0f, 10.0f },
		{ 7.0f, 2.0f, 11.0f }, { 6.0f, 2.0f, 11.0f },
	};
	const uint32_t faceTint = DRAW_TINT(11, 0, 0, 255);
	const uint32_t tileTint = DRAW_TINT(22, 0, 0, 255);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, towerFace, kSideUV, ALPHA_OPAQUE,
					 faceTint));
	TEST_ASSERT_TRUE(appendVoxelFace(&list, tileTop, kTopUV, ALPHA_OPAQUE,
					 tileTint));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT((int)faceTint, (int)drawListItem(&list, 0)->tint);
	TEST_ASSERT_EQUAL_INT((int)tileTint, (int)drawListItem(&list, 1)->tint);
	destroyDrawList(&list);
}

/* A height-1 wall face (plane z=2, x 0..1, y 0..1, base (0.5, 0, 2)) vs a
 * sprite standing in front of it and vs a sprite standing on top of it. The
 * base-line key (0.5,0,2) has closeness 0.816*2 = 1.63; a sprite in front at
 * (0.5, 0, 2.5) is 2.06 and a sprite on top at (0.5, 1, 1.5) is 1.82, both
 * nearer, so both draw after the face. The on-top case is also RED on the old
 * centre key (face centre (0.5,0.5,2) = 1.92 > 1.82 hides the sprite). */
static void test_sort_wall_face_base_line_sprite_in_front_and_on_top(void)
{
	Camera3D camera = cameraAtYaw(0.0f);
	const float wallFace[4][3] = {
		{ 0.0f, 0.0f, 2.0f }, { 1.0f, 0.0f, 2.0f },
		{ 1.0f, 1.0f, 2.0f }, { 0.0f, 1.0f, 2.0f },
	};
	SpriteEntity inFront = { 0.5f, 0.0f, 2.5f, 1.2f, 1.8f,
				 DRAW_TINT(255, 255, 255, 255), -1 };
	SpriteEntity onTop = { 0.5f, 1.0f, 1.5f, 1.2f, 1.8f,
			       DRAW_TINT(255, 255, 255, 255), -1 };
	DrawList list;

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, wallFace, kSideUV, ALPHA_OPAQUE,
					 DRAW_TINT(240, 240, 240, 255)));
	TEST_ASSERT_TRUE(appendSprite(&list, &inFront, &camera, NULL));
	sortDrawList(&list, &camera);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, wallFace, kSideUV, ALPHA_OPAQUE,
					 DRAW_TINT(240, 240, 240, 255)));
	TEST_ASSERT_TRUE(appendSprite(&list, &onTop, &camera, NULL));
	sortDrawList(&list, &camera);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
}

/* The base line is the midpoint of the quad's TWO LOWEST corners, not a fixed
 * pair: this face's corner order is deliberately permuted so the lowest
 * corners are indices 2 and 3 (heights 4,3,1,2). The base is therefore
 * ((0,1,0)+(0,2,0))/2 = (0, 1.5, 0), closeness 0.577*1.5 = 0.87 at yaw 0; a
 * sprite at (0,0,1.32) is 0.816*1.32 + 0.02 = 1.10, so the sprite draws last.
 * Using corners 0/1 (midpoint y=3.5 -> 2.02) or the centre (y=2.5 -> 1.44)
 * would both sort the face after the sprite, so this discriminates the
 * selection. It also exercises the min-scan and selection arms a canonical
 * bottom-first quad never reaches. */
static void test_sort_base_line_picks_two_lowest_corners(void)
{
	DrawList list;
	Camera3D camera = cameraAtYaw(0.0f);
	const float permutedFace[4][3] = {
		{ 0.0f, 4.0f, 0.0f }, { 0.0f, 3.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f }, { 0.0f, 2.0f, 0.0f },
	};
	SpriteEntity sprite = { 0.0f, 0.0f, 1.32f, 1.2f, 1.8f,
				DRAW_TINT(255, 255, 255, 255), -1 };

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendVoxelFace(&list, permutedFace, kSideUV, ALPHA_OPAQUE,
					 DRAW_TINT(240, 240, 240, 255)));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	sortDrawList(&list, &camera);

	TEST_ASSERT_EQUAL_INT(DRAW_KIND_VOXEL, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 1)->kind);
	destroyDrawList(&list);
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
	RUN_TEST(test_sort_key_point_tiebreak_by_y);
	RUN_TEST(test_sort_null_safe);
	RUN_TEST(test_sort_sprite_biased_over_coplanar_face);
	RUN_TEST(test_sort_bias_does_not_hide_a_nearer_face);
	RUN_TEST(test_sort_sprite_over_tile_top_sweep);
	RUN_TEST(test_sort_tower_face_base_line_sprite_in_front_yaw0);
	RUN_TEST(test_sort_tower_face_base_line_still_occludes_yaw180);
	RUN_TEST(test_sort_tower_face_base_line_keeps_ground_tile_in_front);
	RUN_TEST(test_sort_wall_face_base_line_sprite_in_front_and_on_top);
	RUN_TEST(test_sort_base_line_picks_two_lowest_corners);
}
