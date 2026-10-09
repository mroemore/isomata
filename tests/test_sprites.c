/*
 * Sprite tests (CTOL rung 1: unit + boundary).
 *
 * Pins the camera-anchored billboard: the quad is built from the camera's
 * right/up basis (not a hardcoded axis), so at yaw 0 its width runs along +X
 * and at yaw 90 along -Z; the anchor is the bottom-centre; the typed append
 * stamps DRAW_KIND_SPRITE and ATLAS_UV_SPRITE; and NULL/full arguments are
 * safe.
 *
 * Pure: links sprites.c (+ drawlist/camera deps) and the Unity subset.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_sprites().
 */

#include "unity.h"

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/sprites.h"
#include "render/textures.h"

#include <math.h>
#include <string.h>

#define EPS 1e-3f

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

/* At yaw 0 the camera's right is world +X and its up is (0, cos, -sin) of the
 * pitch, so the billboard's width runs along +X and its height leans back. */
static void test_quad_basis_yaw_zero(void)
{
	Camera3D camera = cameraAtYaw(0.0f);
	SpriteEntity sprite = { 0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 0xffffffffu, -1 };
	float quad[4][3];
	float pitch = CAMERA_DEFAULT_PITCH_DEG * (3.14159265358979323846f / 180.0f);
	float cp = cosf(pitch);
	float sp = sinf(pitch);

	buildSpriteQuad(&sprite, &camera, quad);

	/* bottom-left = anchor - right*halfWidth */
	TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, quad[0][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[0][2]);
	/* bottom-right - bottom-left = right*width = (2, 0, 0) */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, quad[1][0] - quad[0][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[1][1] - quad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[1][2] - quad[0][2]);
	/* top-left - bottom-left = up*height = (0, cp*3, -sp*3) */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[3][0] - quad[0][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, cp * 3.0f, quad[3][1] - quad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -sp * 3.0f, quad[3][2] - quad[0][2]);
}

/* At yaw 90 the camera's right is world -Z, so the width runs along -Z. This
 * is the discriminator that proves the basis comes from the camera. */
static void test_quad_basis_yaw_ninety(void)
{
	Camera3D camera = cameraAtYaw(90.0f);
	SpriteEntity sprite = { 0.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0xffffffffu, -1 };
	float quad[4][3];

	buildSpriteQuad(&sprite, &camera, quad);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[1][0] - quad[0][0]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[1][1] - quad[0][1]);
	TEST_ASSERT_FLOAT_WITHIN(EPS, -2.0f, quad[1][2] - quad[0][2]);
}

/* The anchor is the bottom-centre: the two bottom corners straddle it and the
 * top corners rise by `height`. */
static void test_anchor_is_bottom_centre(void)
{
	Camera3D camera = cameraAtYaw(0.0f);
	SpriteEntity sprite = { 5.0f, 1.0f, 7.0f, 4.0f, 2.0f, 0xffffffffu, -1 };
	float quad[4][3];
	float midX;
	float midZ;

	buildSpriteQuad(&sprite, &camera, quad);
	midX = (quad[0][0] + quad[1][0]) * 0.5f;
	midZ = (quad[0][2] + quad[1][2]) * 0.5f;
	TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, midX);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 7.0f, midZ);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quad[0][1]);	/* bottom at anchor y */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quad[1][1]);
	TEST_ASSERT_TRUE(quad[2][1] > quad[0][1]);		/* top above bottom */
}

/* appendSprite stamps kind/UV/tint and joins the same list as voxel faces. */
static void test_append_sprite_stamps_kind_uv_tint(void)
{
	Camera3D camera = cameraAtYaw(0.0f);
	SpriteEntity sprite = { 2.0f, 0.0f, 3.0f, 1.0f, 1.0f, DRAW_TINT(9, 8, 7, 6), -1 };
	const float spriteUV[4][2] = ATLAS_UV_SPRITE;
	DrawList list;

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	TEST_ASSERT_EQUAL_INT(1, (int)drawListCount(&list));
	TEST_ASSERT_EQUAL_INT(DRAW_KIND_SPRITE, drawListItem(&list, 0)->kind);
	TEST_ASSERT_EQUAL_MEMORY(spriteUV, drawListItem(&list, 0)->uv,
				 sizeof(spriteUV));
	TEST_ASSERT_EQUAL_INT((int)sprite.tint,
			      (int)drawListItem(&list, 0)->tint);

	/* A full list rejects the append. */
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	TEST_ASSERT_FALSE(appendSprite(&list, &sprite, &camera, NULL));
	destroyDrawList(&list);
}

/* NULL arguments are safe: a NULL sprite writes a zero quad, a NULL output is
 * a no-op, and a NULL sprite/list append returns false. */
static void test_null_safe(void)
{
	Camera3D camera = cameraAtYaw(0.0f);
	DrawList list;
	float quad[4][3] = { { 9.0f, 9.0f, 9.0f } };
	size_t i;

	buildSpriteQuad(NULL, &camera, quad);
	for (i = 0; i < 4; i++) {
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[i][0]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[i][1]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, quad[i][2]);
	}
	buildSpriteQuad(NULL, NULL, NULL);	/* no crash */

	initDrawList(&list, 2);
	TEST_ASSERT_FALSE(appendSprite(&list, NULL, &camera, NULL));
	TEST_ASSERT_FALSE(appendSprite(NULL, NULL, &camera, NULL));
	destroyDrawList(&list);
}

/* appendSprite samples the sprite material's front (south) face, oriented
 * upright, and carries the material's alpha mode; an unusable table/id falls
 * back to the pinned sprite region and blend. */
static void test_append_sprite_uses_material(void)
{
	MaterialTable t;
	AtlasRect r = { 0.1f, 0.2f, 0.3f, 0.4f };
	const float fallback[4][2] = ATLAS_UV_SPRITE;
	float expect[4][2];
	Camera3D camera = cameraAtYaw(0.0f);
	SpriteEntity sprite = { 2.0f, 0.0f, 3.0f, 1.0f, 1.0f,
				DRAW_TINT(9, 8, 7, 6), 0 };
	DrawList list;

	memset(&t, 0, sizeof(t));
	materialTableAdd(&t, "sprite", ALPHA_CUTOUT);
	materialTableSetRect(&t, 0, FACE_SOUTH, r);
	materialFaceUV(&r, FACE_SOUTH, expect);

	initDrawList(&list, 4);
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, &t));
	TEST_ASSERT_EQUAL_MEMORY(expect, drawListItem(&list, 0)->uv,
				 sizeof(expect));
	TEST_ASSERT_EQUAL_INT(ALPHA_CUTOUT, drawListItem(&list, 0)->alphaMode);
	/* Upright: corner 0 (world bottom) takes the rect bottom. */
	TEST_ASSERT_TRUE(drawListItem(&list, 0)->uv[0][1] >
			 drawListItem(&list, 0)->uv[2][1]);
	destroyDrawList(&list);

	/* Out-of-range material id falls back to the pinned sprite region. */
	initDrawList(&list, 4);
	sprite.material = 7;
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, &t));
	TEST_ASSERT_EQUAL_MEMORY(fallback, drawListItem(&list, 0)->uv,
				 sizeof(fallback));
	TEST_ASSERT_EQUAL_INT(ALPHA_BLEND, drawListItem(&list, 0)->alphaMode);
	destroyDrawList(&list);

	/* A negative material id and a NULL table both fall back too. */
	initDrawList(&list, 4);
	sprite.material = -1;
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, &t));
	TEST_ASSERT_EQUAL_MEMORY(fallback, drawListItem(&list, 0)->uv,
				 sizeof(fallback));
	TEST_ASSERT_TRUE(appendSprite(&list, &sprite, &camera, NULL));
	TEST_ASSERT_EQUAL_MEMORY(fallback, drawListItem(&list, 1)->uv,
				 sizeof(fallback));
	destroyDrawList(&list);
}

void run_test_sprites(void);

void run_test_sprites(void)
{
	RUN_TEST(test_quad_basis_yaw_zero);
	RUN_TEST(test_quad_basis_yaw_ninety);
	RUN_TEST(test_anchor_is_bottom_centre);
	RUN_TEST(test_append_sprite_stamps_kind_uv_tint);
	RUN_TEST(test_null_safe);
	RUN_TEST(test_append_sprite_uses_material);
}
