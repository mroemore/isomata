/*
 * Entity registry + tile-movement tests (CTOL rung 1: unit + boundary).
 *
 * Covers: registry create/destroy/reuse/iterate/over-capacity and the
 * handle-validity rules; entityPlace (void rejected); the walkable-step table
 * (flat, one-step climb, cliff, void, ramp / half-ramp / height-0 flag,
 * diagonal + non-adjacent, NULL); tween positions at exact t with a fake-dt
 * sequence; the arrival pulse; snap vs ease; speed -> duration; y on a step
 * up/down; the queued path (step-by-step arrival + atomic rejection of a
 * non-adjacent / void / cliff / overflowing path); the entitiesUpdate dt clamp;
 * and run-to-run determinism.
 *
 * Pure: links entities.c + voxmap.c + easing.c and the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_entities().
 */

#include "unity.h"

#include "entities/entities.h"
#include "entities/easing.h"
#include "render/voxmap.h"

#include <math.h>
#include <string.h>

#define EPS 1e-3f

static Voxmap *parseText(const char *text)
{
	return parseVoxmapText(text, strlen(text), NULL);
}

/* A flat/stepped 4x4 heightmap: z0=1111, z1=1221, z2=1331, z3=1111
 * (surfaces 1..3). */
static Voxmap *stepMap(void)
{
	return parseText("1111\n1221\n1331\n1111\n");
}

/* 2x2 with the (1,1) cell void. */
static Voxmap *voidMap(void)
{
	return parseText("11\n1.\n");
}

/* Create a walker placed at (x, z): linear easing, animated, speed 1. */
static EntityHandle makeWalker(EntityRegistry *reg, int x, int z)
{
	EntityHandle h = entityCreate(reg);
	Entity *e = entityGet(reg, h);

	if (e == NULL || !entityPlace(e, x, z))
		return ENTITY_INVALID;
	e->speed = 1.0f;
	e->animated = true;
	e->easing = EASE_LINEAR;
	return h;
}

/* ---- registry ---------------------------------------------------------- */

static void test_registry_create_capacity(void)
{
	EntityRegistry reg;
	int i;

	entitiesInit(&reg, NULL);
	TEST_ASSERT_EQUAL_INT(0, entityCount(&reg));
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, entityFirst(&reg));

	for (i = 0; i < ENTITY_MAX; i++)
		TEST_ASSERT_EQUAL_INT(i, entityCreate(&reg));
	TEST_ASSERT_EQUAL_INT(ENTITY_MAX, entityCount(&reg));
	/* Full: the next create fails cleanly. */
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, entityCreate(&reg));
	TEST_ASSERT_EQUAL_INT(ENTITY_MAX, entityCount(&reg));

	/* Iteration visits every live handle exactly once, ascending. */
	{
		EntityHandle h;
		int seen = 0;

		for (h = entityFirst(&reg); h != ENTITY_INVALID;
		     h = entityNext(&reg, h)) {
			TEST_ASSERT_EQUAL_INT(seen, h);
			seen++;
		}
		TEST_ASSERT_EQUAL_INT(ENTITY_MAX, seen);
	}
}

static void test_registry_destroy_and_reuse(void)
{
	EntityRegistry reg;
	EntityHandle a;
	EntityHandle b;
	EntityHandle c;

	entitiesInit(&reg, NULL);
	a = entityCreate(&reg);
	b = entityCreate(&reg);
	TEST_ASSERT_TRUE(entityAlive(&reg, a));
	TEST_ASSERT_NOT_NULL(entityGet(&reg, a));

	/* Free a: it is dead, unknown/dead handles are rejected, and get is
	 * NULL. */
	TEST_ASSERT_TRUE(entityDestroy(&reg, a));
	TEST_ASSERT_FALSE(entityAlive(&reg, a));
	TEST_ASSERT_NULL(entityGet(&reg, a));
	TEST_ASSERT_FALSE(entityDestroy(&reg, a));
	TEST_ASSERT_FALSE(entityDestroy(&reg, -1));
	TEST_ASSERT_EQUAL_INT(1, entityCount(&reg));
	TEST_ASSERT_EQUAL_INT(b, entityFirst(&reg));

	/* The freed slot is handed out again (reuse), never aliased. */
	c = entityCreate(&reg);
	TEST_ASSERT_EQUAL_INT(a, c);
	TEST_ASSERT_TRUE(entityAlive(&reg, c));
	TEST_ASSERT_EQUAL_INT(2, entityCount(&reg));

	/* Out-of-range handles. */
	TEST_ASSERT_FALSE(entityAlive(&reg, -1));
	TEST_ASSERT_FALSE(entityAlive(&reg, ENTITY_MAX));
	TEST_ASSERT_NULL(entityGet(&reg, -1));
	TEST_ASSERT_NULL(entityGet(&reg, ENTITY_MAX));
	TEST_ASSERT_FALSE(entityDestroy(&reg, ENTITY_MAX));
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, entityNext(&reg, ENTITY_MAX));

	/* NULL-safe entry points. */
	entitiesInit(NULL, NULL);
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, entityCreate(NULL));
	TEST_ASSERT_EQUAL_INT(0, entityCount(NULL));
	TEST_ASSERT_EQUAL_INT(ENTITY_INVALID, entityFirst(NULL));
	TEST_ASSERT_NULL(entityGet(NULL, 0));
	TEST_ASSERT_FALSE(entityAlive(NULL, 0));
	TEST_ASSERT_FALSE(entityDestroy(NULL, 0));
	entitiesUpdate(NULL, 0.1f);	/* no crash */
}

/* ---- placement + sprite link ------------------------------------------ */

static void test_place_and_sprite(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	EntityHandle h;
	Entity *e;

	entitiesInit(&reg, map);
	h = makeWalker(&reg, 1, 1);
	e = entityGet(&reg, h);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(1, e->tileX);
	TEST_ASSERT_EQUAL_INT(1, e->tileZ);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, e->y);	/* surface of (1,1) */

	/* Sprite link. */
	entitySetSprite(e, 1.2f, 1.8f, 0x11223344u, 3);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.2f, e->width);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.8f, e->height);
	TEST_ASSERT_EQUAL_UINT(0x11223344u, e->tint);
	TEST_ASSERT_EQUAL_INT(3, e->material);
	entitySetSprite(NULL, 1.0f, 1.0f, 0, 0);	/* no crash */

	/* Void / NULL / no-map placement is rejected and leaves the entity. */
	TEST_ASSERT_TRUE(entityPlace(e, 0, 0));		/* valid re-place */
	TEST_ASSERT_EQUAL_INT(0, e->tileX);
	{
		Voxmap *vm = voidMap();

		entitiesInit(&reg, vm);
		h = makeWalker(&reg, 0, 0);
		e = entityGet(&reg, h);
		TEST_ASSERT_NOT_NULL(e);
		TEST_ASSERT_FALSE(entityPlace(e, 1, 1));	/* void */
		TEST_ASSERT_EQUAL_INT(0, e->tileX);		/* unchanged */
		TEST_ASSERT_FALSE(entityPlace(e, 5, 5));	/* OOB == void */
		destroyVoxmap(vm);
	}
	TEST_ASSERT_FALSE(entityPlace(NULL, 0, 0));
	{
		EntityRegistry bare;

		entitiesInit(&bare, NULL);
		h = entityCreate(&bare);
		TEST_ASSERT_FALSE(entityPlace(entityGet(&bare, h), 0, 0));
	}
	destroyVoxmap(map);
}

/* ---- walkability table ------------------------------------------------- */

static void test_walkable_step_table(void)
{
	Voxmap *map = stepMap();

	/* Flat + one-step climb, both directions. */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 0, 1, 0, 0));	/* 1 -> 1 */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 0, 1, 1, 1));	/* 1 -> 2 */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 1, 1, 1, 2));	/* 2 -> 3 */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 1, 1, 0, 1));	/* 2 -> 1 */

	/* A two-unit drop or rise is a cliff. */
	TEST_ASSERT_FALSE(entityWalkableStep(map, 0, 2, 1, 2));	/* 1 -> 3 */
	TEST_ASSERT_FALSE(entityWalkableStep(map, 1, 2, 0, 2));	/* 3 -> 1 */

	/* Non-adjacent (same row far, and diagonal) are rejected. */
	TEST_ASSERT_FALSE(entityWalkableStep(map, 0, 0, 2, 0));
	TEST_ASSERT_FALSE(entityWalkableStep(map, 0, 0, 1, 1));
	/* Same tile is not a step. */
	TEST_ASSERT_FALSE(entityWalkableStep(map, 0, 0, 0, 0));
	/* NULL map. */
	TEST_ASSERT_FALSE(entityWalkableStep(NULL, 0, 0, 1, 0));

	destroyVoxmap(map);

	/* Void target. */
	map = voidMap();
	TEST_ASSERT_FALSE(entityWalkableStep(map, 0, 1, 1, 1));
	destroyVoxmap(map);
}

/* Shapes: a ramp reads 0.5 at the centre and a half-ramp 0.25; a height-0
 * ground tile reads 0 and steps onto/off a level-1 full cell (diff 1). */
static void test_walkable_step_shapes(void)
{
	Voxmap *map = parseText("@ g 1 grass\n"
				"@ z 0 grass\n"
				"@ R 1 stone shape=ramp dir=north\n"
				"@ Q 1 stone shape=half-ramp dir=north\n"
				"@ H 1 stone shape=half\n"
				"gzRQ\n"
				"HHHH\n");

	TEST_ASSERT_NOT_NULL(map);
	/* g(1.0) -> z(0.0): height-0 flag, diff 1.0. */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 0, 0, 1, 0));
	/* z(0.0) -> R(0.5): ramp centre. */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 1, 0, 2, 0));
	/* R(0.5) -> Q(0.25): half-ramp centre. */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 2, 0, 3, 0));
	/* g(1.0) -> H(0.5): a half slab below the full top. */
	TEST_ASSERT_TRUE(entityWalkableStep(map, 0, 0, 0, 1));
	destroyVoxmap(map);
}

/* ---- movement + tween -------------------------------------------------- */

/* Flat adjacent move, linear easing, speed 1 -> duration 1s: exact positions
 * at t = 0.25 / 0.5 / 0.75 and the arrival pulse at t = 1. */
static void test_move_tween_positions(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, e->segDuration);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->z);	/* still at the start */

	/* 5 * 0.05 = 0.25s -> t = 0.25. */
	for (i = 0; i < 5; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FALSE(e->arrived);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.25f, e->z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, e->y);

	/* +10 * 0.05 -> t = 0.75. */
	for (i = 0; i < 10; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, e->z);

	/* +5 -> t = 1, arrived. */
	for (i = 0; i < 5; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->tileX);
	TEST_ASSERT_EQUAL_INT(0, e->tileZ);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->z);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->x);

	/* The pulse clears on the next update. */
	entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FALSE(e->arrived);

	destroyVoxmap(map);
}

/* Easing shapes the tween: at t = 0.5 the linear z is 1.0 and ease-in z is
 * 1.25 (ease(0.5) = 0.25). */
static void test_move_easing_curve(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	e->easing = EASE_IN;
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	for (i = 0; i < 10; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.25f, e->z);
	destroyVoxmap(map);
}

/* animated == false holds the from tile for the whole segment and snaps at
 * the end (same timing as the eased move). */
static void test_move_snap_mode(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	e->animated = false;
	e->easing = EASE_LINEAR;	/* must be ignored when not animated */
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));

	for (i = 0; i < 19; i++) {
		entitiesUpdate(&reg, 0.05f);
		TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->z);	/* held */
		TEST_ASSERT_FALSE(e->arrived);
	}
	entitiesUpdate(&reg, 0.05f);	/* t = 1 */
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->z);	/* snapped */
	destroyVoxmap(map);
}

/* speed -> duration: at speed 2 an adjacent segment lasts 0.5s, so it
 * completes on exactly the fifth 0.1s step. */
static void test_move_speed_to_duration(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	e->speed = 2.0f;
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->segDuration);
	for (i = 0; i < 4; i++) {
		entitiesUpdate(&reg, 0.1f);
		TEST_ASSERT_FALSE(e->arrived);
	}
	entitiesUpdate(&reg, 0.1f);
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FALSE(e->moving);
	destroyVoxmap(map);
}

/* y follows the surface: a step up lerps 1 -> 2 (mid 1.5), a step down the
 * reverse. */
static void test_move_y_follows_surface(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	e->easing = EASE_LINEAR;

	/* Up: (0,1) surface 1 -> (1,1) surface 2. */
	TEST_ASSERT_TRUE(entityMoveTo(e, 1, 1));
	for (i = 0; i < 10; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->y);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, e->x);
	for (i = 0; i < 10; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, e->y);

	/* Down: back to surface 1. */
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 1));
	for (i = 0; i < 10; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->y);
	destroyVoxmap(map);
}

/* A move to the current tile is a no-op true. */
static void test_move_to_same_tile(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 1));
	TEST_ASSERT_FALSE(e->moving);
	destroyVoxmap(map);
}

/* entityMoveTo rejects a void target and an adjacent cliff, leaving the entity
 * at rest. */
static void test_move_rejects_invalid(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 2));	/* surface 1 */
	TEST_ASSERT_FALSE(entityMoveTo(e, 1, 2));	/* 1 -> 3 cliff */
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_FALSE(entityMoveTo(e, 9, 9));	/* OOB / void */
	TEST_ASSERT_FALSE(entityMoveTo(NULL, 0, 0));
	destroyVoxmap(map);

	map = voidMap();
	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 0));
	TEST_ASSERT_FALSE(entityMoveTo(e, 1, 1));	/* void */
	destroyVoxmap(map);
}

/* ---- path queue -------------------------------------------------------- */

/* A 3-step path is consumed one segment at a time; arrival pulses on each. */
static void test_path_queue_steps(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int path[3][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityWalkPath(e, path, 3));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->toX);
	TEST_ASSERT_EQUAL_INT(0, e->toZ);	/* first waypoint active */
	TEST_ASSERT_EQUAL_INT(2, e->pathCount);	/* two still queued */

	/* 20 * 0.05 = 1s -> first segment complete. */
	for (i = 0; i < 20; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_EQUAL_INT(0, e->tileX);
	TEST_ASSERT_EQUAL_INT(0, e->tileZ);
	TEST_ASSERT_TRUE(e->moving);		/* next step started */
	TEST_ASSERT_EQUAL_INT(1, e->toX);

	for (i = 0; i < 20; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_EQUAL_INT(1, e->tileX);
	TEST_ASSERT_EQUAL_INT(0, e->tileZ);
	TEST_ASSERT_TRUE(e->moving);

	for (i = 0; i < 20; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_EQUAL_INT(2, e->tileX);
	TEST_ASSERT_EQUAL_INT(0, e->tileZ);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	destroyVoxmap(map);
}

/* n == 0 is a no-op; a non-adjacent / void / cliff step and an overflowing or
 * NULL path reject atomically (nothing queued). */
static void test_path_rejection_atomic(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int nonadj[1][2] = { { 2, 1 } };
	static const int cliff[1][2] = { { 1, 2 } };	/* (0,2) 1 -> 3 */
	int zero[1][2] = { { 0, 0 } };

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 2));
	TEST_ASSERT_TRUE(entityWalkPath(e, zero, 0));	/* no-op true */
	TEST_ASSERT_FALSE(e->moving);

	TEST_ASSERT_FALSE(entityWalkPath(e, nonadj, 1));
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);

	TEST_ASSERT_FALSE(entityWalkPath(e, cliff, 1));
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);

	TEST_ASSERT_FALSE(entityWalkPath(e, NULL, 1));
	TEST_ASSERT_FALSE(entityWalkPath(NULL, zero, 1));
	destroyVoxmap(map);

	/* Void neighbour. */
	map = voidMap();
	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_FALSE(entityWalkPath(e, nonadj, 1));	/* (2,1) OOB */
	{
		static const int voidStep[1][2] = { { 1, 1 } };

		TEST_ASSERT_FALSE(entityWalkPath(e, voidStep, 1));
	}
	destroyVoxmap(map);
}

/* Overflow: a 64-step flat bounce fills the queue, the 65th is rejected. */
static void test_path_overflow(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int big[ENTITY_MAX_PATH][2];
	int i;
	static const int one[1][2] = { { 0, 0 } };

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	for (i = 0; i < ENTITY_MAX_PATH; i++) {
		big[i][0] = 0;
		big[i][1] = (i & 1) ? 1 : 0;	/* bounce 0,1,0,1,... */
	}
	/* Drop the first (it is the tile we already stand on) so every step is a
	 * real move: start from (1,0). */
	big[0][1] = 0;
	/* Rebuild as a valid bounce from (0,1): first to (0,0), then alternate. */
	for (i = 0; i < ENTITY_MAX_PATH; i++)
		big[i][1] = (i & 1) ? 1 : 0;
	TEST_ASSERT_TRUE(entityWalkPath(e, big, ENTITY_MAX_PATH));
	/* The first step became the active segment; 63 remain queued. */
	TEST_ASSERT_EQUAL_INT(ENTITY_MAX_PATH - 1, e->pathCount);
	TEST_ASSERT_EQUAL_INT(0, e->toX);
	TEST_ASSERT_EQUAL_INT(0, e->toZ);
	TEST_ASSERT_FALSE(entityWalkPath(e, one, 1));	/* full */
	/* A path longer than the queue is rejected up front. */
	TEST_ASSERT_FALSE(entityWalkPath(e, big, ENTITY_MAX_PATH + 1));
	destroyVoxmap(map);
}

/* ---- update hygiene ---------------------------------------------------- */

/* dt is clamped to ENTITY_MAX_DT: a huge dt advances at most one max-step and
 * a NaN / negative dt does not move at all. */
static void test_update_dt_clamp(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));	/* speed 1, duration 1s */
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	entitiesUpdate(&reg, 1000.0f);		/* clamped to 0.1s */
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.4f, e->z);	/* only 0.1 progressed */

	entitiesUpdate(&reg, -1.0f);		/* clamped to 0 */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.4f, e->z);
	entitiesUpdate(&reg, (float)NAN);	/* 0 */
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.4f, e->z);
	destroyVoxmap(map);
}

/* Two runs with identical inputs land on bit-identical positions. */
static void test_determinism(void)
{
	Voxmap *map = stepMap();
	EntityRegistry regA;
	EntityRegistry regB;
	Entity *a;
	Entity *b;
	static const int path[3][2] = { { 1, 1 }, { 1, 2 }, { 2, 2 } };
	int i;

	entitiesInit(&regA, map);
	entitiesInit(&regB, map);
	a = entityGet(&regA, makeWalker(&regA, 0, 1));
	b = entityGet(&regB, makeWalker(&regB, 0, 1));
	a->easing = EASE_IN_OUT;
	b->easing = EASE_IN_OUT;
	TEST_ASSERT_TRUE(entityWalkPath(a, path, 3));
	TEST_ASSERT_TRUE(entityWalkPath(b, path, 3));

	for (i = 0; i < 97; i++) {
		entitiesUpdate(&regA, 0.037f);
		entitiesUpdate(&regB, 0.037f);
		TEST_ASSERT_EQUAL_INT(0, memcmp(&a->x, &b->x, sizeof(a->x)));
		TEST_ASSERT_EQUAL_INT(0, memcmp(&a->z, &b->z, sizeof(a->z)));
		TEST_ASSERT_EQUAL_INT(0, memcmp(&a->y, &b->y, sizeof(a->y)));
	}
	destroyVoxmap(map);
}

/* A far (non-adjacent) valid target is a single segment; speed <= 0 means an
 * instant arrival (duration 0). */
static void test_move_far_and_speed_zero(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityMoveTo(e, 2, 1));	/* 2 tiles away */
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(2, e->toX);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, e->segDuration);	/* 2 / speed 1 */

	TEST_ASSERT_TRUE(entityPlace(e, 1, 1));
	e->speed = 0.0f;
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 1));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, e->segDuration);
	entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->tileX);
	destroyVoxmap(map);
}

/* A move re-issued to the tile we started from cancels the in-flight
 * segment and snaps back. */
static void test_move_same_tile_cancels(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	for (i = 0; i < 5; i++)
		entitiesUpdate(&reg, 0.05f);
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 1));	/* == current tile */
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->tileX);
	TEST_ASSERT_EQUAL_INT(1, e->tileZ);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 1.5f, e->z);
	destroyVoxmap(map);
}

/* An entity created without a map accepts no move or path. */
static void test_entity_no_map(void)
{
	EntityRegistry bare;
	EntityHandle h;
	Entity *e;
	static const int step[1][2] = { { 0, 0 } };

	entitiesInit(&bare, NULL);
	h = entityCreate(&bare);
	e = entityGet(&bare, h);
	TEST_ASSERT_FALSE(entityMoveTo(e, 0, 0));
	TEST_ASSERT_FALSE(entityWalkPath(e, step, 1));
}

/* One-step paths drain immediately; a path appends to an in-flight queue or a
 * bare segment; n < 0 is rejected. */
static void test_path_append_variants(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int oneStep[1][2] = { { 0, 0 } };
	static const int twoStep[2][2] = { { 0, 0 }, { 1, 0 } };
	static const int moreStep[1][2] = { { 2, 0 } };
	static const int adjStep[1][2] = { { 1, 0 } };

	entitiesInit(&reg, map);

	/* A one-step path drains immediately: head and count reset to 0. */
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityWalkPath(e, oneStep, 1));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_EQUAL_INT(0, e->pathHead);
	TEST_ASSERT_EQUAL_INT(0, e->toX);
	TEST_ASSERT_EQUAL_INT(0, e->toZ);
	TEST_ASSERT_FALSE(entityWalkPath(e, oneStep, -1));

	/* Append to an existing queue while moving: the reference tile is the
	 * last queued waypoint. */
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityWalkPath(e, twoStep, 2));
	TEST_ASSERT_EQUAL_INT(1, e->pathCount);
	TEST_ASSERT_TRUE(entityWalkPath(e, moreStep, 1));
	TEST_ASSERT_EQUAL_INT(2, e->pathCount);
	/* The origin is retained when appending to an active path. */
	TEST_ASSERT_EQUAL_INT(0, e->pathStartX);
	TEST_ASSERT_EQUAL_INT(1, e->pathStartZ);

	/* Append to a bare in-flight segment (empty queue): the reference tile
	 * is the segment target. */
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_TRUE(entityWalkPath(e, adjStep, 1));	/* from (0,0) */
	TEST_ASSERT_EQUAL_INT(1, e->pathCount);
	destroyVoxmap(map);
}

/* ---- T20 path-debug markers ------------------------------------------- */

/* Mid-segment with queue [A,B,C]: START = the path origin, PATH = the
 * current target then each remaining waypoint, DEST = the final waypoint
 * (which is the same tile as the last PATH marker). Order and count pinned. */
static void test_debug_markers_mid_segment(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int path[3][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
	EntityDebugMarker m[8];
	int n;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_TRUE(entityWalkPath(e, path, 3));
	/* Mid-segment: (0,0) active, (1,0) and (2,0) still queued. */
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->toX);
	TEST_ASSERT_EQUAL_INT(0, e->toZ);
	TEST_ASSERT_EQUAL_INT(2, e->pathCount);

	n = entitiesDebugMarkers(&reg, m, 8);
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[0].kind);
	TEST_ASSERT_EQUAL_INT(0, m[0].x);	/* origin (0,1) */
	TEST_ASSERT_EQUAL_INT(1, m[0].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[1].kind);
	TEST_ASSERT_EQUAL_INT(0, m[1].x);
	TEST_ASSERT_EQUAL_INT(0, m[1].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[2].kind);
	TEST_ASSERT_EQUAL_INT(1, m[2].x);
	TEST_ASSERT_EQUAL_INT(0, m[2].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[3].kind);
	TEST_ASSERT_EQUAL_INT(2, m[3].x);
	TEST_ASSERT_EQUAL_INT(0, m[3].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_DEST, m[4].kind);
	TEST_ASSERT_EQUAL_INT(2, m[4].x);	/* final waypoint */
	TEST_ASSERT_EQUAL_INT(0, m[4].z);
	destroyVoxmap(map);
}

/* Idle (no path) and just-completed both yield zero markers. */
static void test_debug_markers_idle_and_completed(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int one[1][2] = { { 0, 0 } };
	EntityDebugMarker m[4];

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));

	/* Idle: no markers. */
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(&reg, m, 4));

	/* A one-step path: start, the target, and the destination. */
	e->speed = 10.0f;	/* duration 0.1 s */
	TEST_ASSERT_TRUE(entityWalkPath(e, one, 1));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(3, entitiesDebugMarkers(&reg, m, 4));

	entitiesUpdate(&reg, 0.1f);
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(&reg, m, 4));
	destroyVoxmap(map);
}

/* Two entities with different paths stay independent, emitted in registry
 * slot order (A then B). */
static void test_debug_markers_two_entities(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *a;
	Entity *b;
	static const int pathA[1][2] = { { 0, 0 } };
	static const int pathB[1][2] = { { 1, 0 } };
	EntityDebugMarker m[8];
	int n;

	entitiesInit(&reg, map);
	a = entityGet(&reg, makeWalker(&reg, 0, 1));
	b = entityGet(&reg, makeWalker(&reg, 1, 1));
	TEST_ASSERT_TRUE(entityWalkPath(a, pathA, 1));
	TEST_ASSERT_TRUE(entityWalkPath(b, pathB, 1));

	n = entitiesDebugMarkers(&reg, m, 8);
	TEST_ASSERT_EQUAL_INT(6, n);
	/* A: start(0,1), path(0,0), dest(0,0). */
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[0].kind);
	TEST_ASSERT_EQUAL_INT(0, m[0].x);
	TEST_ASSERT_EQUAL_INT(1, m[0].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_DEST, m[2].kind);
	TEST_ASSERT_EQUAL_INT(0, m[2].x);
	/* B: start(1,1), path(1,0), dest(1,0). */
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[3].kind);
	TEST_ASSERT_EQUAL_INT(1, m[3].x);
	TEST_ASSERT_EQUAL_INT(1, m[3].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[4].kind);
	TEST_ASSERT_EQUAL_INT(1, m[4].x);
	TEST_ASSERT_EQUAL_INT(0, m[4].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_DEST, m[5].kind);
	TEST_ASSERT_EQUAL_INT(1, m[5].x);
	destroyVoxmap(map);
}

/* Capacity: a short buffer truncates the tail safely (never overflows), and
 * the guard args return zero. */
static void test_debug_markers_overflow(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	static const int path[3][2] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
	EntityDebugMarker m[5];

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityWalkPath(e, path, 3));
	/* Five markers total; every cap yields exactly min(cap, 5). */
	TEST_ASSERT_EQUAL_INT(5, entitiesDebugMarkers(&reg, m, 5));
	TEST_ASSERT_EQUAL_INT(4, entitiesDebugMarkers(&reg, m, 4));
	TEST_ASSERT_EQUAL_INT(3, entitiesDebugMarkers(&reg, m, 3));
	TEST_ASSERT_EQUAL_INT(2, entitiesDebugMarkers(&reg, m, 2));
	TEST_ASSERT_EQUAL_INT(1, entitiesDebugMarkers(&reg, m, 1));
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(&reg, m, 0));
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(&reg, m, -1));
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(NULL, m, 5));
	TEST_ASSERT_EQUAL_INT(0, entitiesDebugMarkers(&reg, NULL, 5));
	destroyVoxmap(map);
}

/* When the buffer fills on an earlier entity, a later entity's markers are
 * dropped wholesale (deterministic prefix, no partial interleave). */
static void test_debug_markers_truncates_later_entities(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *a;
	Entity *b;
	static const int pathA[1][2] = { { 0, 0 } };
	static const int pathB[1][2] = { { 1, 0 } };
	EntityDebugMarker m[1];

	entitiesInit(&reg, map);
	a = entityGet(&reg, makeWalker(&reg, 0, 1));
	b = entityGet(&reg, makeWalker(&reg, 1, 1));
	TEST_ASSERT_TRUE(entityWalkPath(a, pathA, 1));
	TEST_ASSERT_TRUE(entityWalkPath(b, pathB, 1));

	/* A's start fills the single slot; B's markers are dropped. */
	TEST_ASSERT_EQUAL_INT(1, entitiesDebugMarkers(&reg, m, 1));
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[0].kind);
	TEST_ASSERT_EQUAL_INT(0, m[0].x);
	TEST_ASSERT_EQUAL_INT(1, m[0].z);
	destroyVoxmap(map);
}

/* A bare move (segment, no queue) still overlays start/target/dest; a queue
 * with no active segment (constructible via the public fields, not through
 * the movement API) enumerates the queue without a current target. */
static void test_debug_markers_move_to_and_queued_idle(void)
{
	Voxmap *map = stepMap();
	EntityRegistry reg;
	Entity *e;
	EntityDebugMarker m[6];
	int n;

	entitiesInit(&reg, map);

	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	TEST_ASSERT_TRUE(entityMoveTo(e, 0, 0));
	n = entitiesDebugMarkers(&reg, m, 6);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[0].kind);
	TEST_ASSERT_EQUAL_INT(0, m[0].x);
	TEST_ASSERT_EQUAL_INT(1, m[0].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[1].kind);
	TEST_ASSERT_EQUAL_INT(0, m[1].x);
	TEST_ASSERT_EQUAL_INT(0, m[1].z);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_DEST, m[2].kind);
	TEST_ASSERT_EQUAL_INT(0, m[2].x);
	TEST_ASSERT_EQUAL_INT(0, m[2].z);

	/* Queued but idle (hand-built state) — fresh registry so the previous
	 * entity does not contribute. */
	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 1));
	e->moving = false;
	e->pathStartX = 0;
	e->pathStartZ = 1;
	e->pathHead = 0;
	e->pathCount = 2;
	e->path[0][0] = 0;
	e->path[0][1] = 0;
	e->path[1][0] = 1;
	e->path[1][1] = 0;
	n = entitiesDebugMarkers(&reg, m, 6);
	TEST_ASSERT_EQUAL_INT(4, n);	/* start, two path, dest */
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_START, m[0].kind);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[1].kind);
	TEST_ASSERT_EQUAL_INT(0, m[1].x);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_PATH, m[2].kind);
	TEST_ASSERT_EQUAL_INT(1, m[2].x);
	TEST_ASSERT_EQUAL_INT(ENTITY_DEBUG_DEST, m[3].kind);
	TEST_ASSERT_EQUAL_INT(1, m[3].x);
	TEST_ASSERT_EQUAL_INT(0, m[3].z);
	destroyVoxmap(map);
}

/* Kind values, tints and widths are the pinned overlay contract. */
static void test_debug_marker_look_constants(void)
{
	TEST_ASSERT_EQUAL_INT(0, ENTITY_DEBUG_START);
	TEST_ASSERT_EQUAL_INT(1, ENTITY_DEBUG_PATH);
	TEST_ASSERT_EQUAL_INT(2, ENTITY_DEBUG_DEST);

	/* cyan path, green start, red dest. */
	TEST_ASSERT_EQUAL_UINT(0x00DCDCFFu, ENTITY_DEBUG_TINT_PATH);
	TEST_ASSERT_EQUAL_UINT(0x3CDC50FFu, ENTITY_DEBUG_TINT_START);
	TEST_ASSERT_EQUAL_UINT(0xE63232FFu, ENTITY_DEBUG_TINT_DEST);

	TEST_ASSERT_EQUAL_UINT(ENTITY_DEBUG_TINT_PATH,
			       entityDebugMarkerTint(ENTITY_DEBUG_PATH));
	TEST_ASSERT_EQUAL_UINT(ENTITY_DEBUG_TINT_START,
			       entityDebugMarkerTint(ENTITY_DEBUG_START));
	TEST_ASSERT_EQUAL_UINT(ENTITY_DEBUG_TINT_DEST,
			       entityDebugMarkerTint(ENTITY_DEBUG_DEST));
	TEST_ASSERT_EQUAL_UINT(ENTITY_DEBUG_TINT_PATH,
			       entityDebugMarkerTint(99));	/* fallback */

	TEST_ASSERT_FLOAT_WITHIN(EPS, ENTITY_DEBUG_MARKER_PATH_WIDTH,
				 entityDebugMarkerWidth(ENTITY_DEBUG_PATH));
	TEST_ASSERT_FLOAT_WITHIN(EPS, ENTITY_DEBUG_MARKER_START_WIDTH,
				 entityDebugMarkerWidth(ENTITY_DEBUG_START));
	TEST_ASSERT_FLOAT_WITHIN(EPS, ENTITY_DEBUG_MARKER_DEST_WIDTH,
				 entityDebugMarkerWidth(ENTITY_DEBUG_DEST));
	TEST_ASSERT_FLOAT_WITHIN(EPS, ENTITY_DEBUG_MARKER_PATH_WIDTH,
				 entityDebugMarkerWidth(99));	/* fallback */
	TEST_ASSERT_TRUE(ENTITY_DEBUG_MARKER_START_WIDTH >
			 ENTITY_DEBUG_MARKER_PATH_WIDTH);
	TEST_ASSERT_TRUE(ENTITY_DEBUG_MARKER_PATH_WIDTH >
			 ENTITY_DEBUG_MARKER_DEST_WIDTH);
	TEST_ASSERT_TRUE(ENTITY_DEBUG_MARKER_LIFT > 0.0f);
}

void run_test_entities(void);

void run_test_entities(void)
{
	RUN_TEST(test_registry_create_capacity);
	RUN_TEST(test_registry_destroy_and_reuse);
	RUN_TEST(test_place_and_sprite);
	RUN_TEST(test_walkable_step_table);
	RUN_TEST(test_walkable_step_shapes);
	RUN_TEST(test_move_tween_positions);
	RUN_TEST(test_move_easing_curve);
	RUN_TEST(test_move_snap_mode);
	RUN_TEST(test_move_speed_to_duration);
	RUN_TEST(test_move_y_follows_surface);
	RUN_TEST(test_move_to_same_tile);
	RUN_TEST(test_move_rejects_invalid);
	RUN_TEST(test_move_far_and_speed_zero);
	RUN_TEST(test_move_same_tile_cancels);
	RUN_TEST(test_entity_no_map);
	RUN_TEST(test_path_queue_steps);
	RUN_TEST(test_path_rejection_atomic);
	RUN_TEST(test_path_append_variants);
	RUN_TEST(test_path_overflow);
	RUN_TEST(test_update_dt_clamp);
	RUN_TEST(test_determinism);
	RUN_TEST(test_debug_markers_mid_segment);
	RUN_TEST(test_debug_markers_idle_and_completed);
	RUN_TEST(test_debug_markers_two_entities);
	RUN_TEST(test_debug_markers_overflow);
	RUN_TEST(test_debug_markers_truncates_later_entities);
	RUN_TEST(test_debug_markers_move_to_and_queued_idle);
	RUN_TEST(test_debug_marker_look_constants);
}
