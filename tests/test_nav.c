/*
 * Navigation glue tests (CTOL rung 1: unit + boundary).
 *
 * entityFollowPath (queue a validated route); entityPathTo (compute + replace,
 * rejecting cleanly with no path); the pinned re-path semantics (remaining
 * queue dropped, in-flight segment kept, new route from the segment target);
 * following a computed path to the end under a fake dt, and stopping on
 * arrival.
 *
 * Pure: links ai/nav.c + ai/pathfind.c + entities.c + easing.c + voxmap.c +
 * the Unity subset. Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_nav().
 */

#include "unity.h"

#include "ai/nav.h"
#include "ai/pathfind.h"
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

/* A walker at (x, z): linear easing, animated, speed 1 (1s per segment). */
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

/* entityFollowPath is a pass-through: it queues a valid route and rejects an
 * invalid one cleanly. */
static void test_follow_path(void)
{
	Voxmap *map = parseText("11111\n");
	EntityRegistry reg;
	Entity *e;
	static const int good[3][2] = { { 1, 0 }, { 2, 0 }, { 3, 0 } };
	static const int bad[1][2] = { { 2, 2 } };	/* non-adjacent / OOB */

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 0));
	TEST_ASSERT_NOT_NULL(e);

	TEST_ASSERT_TRUE(entityFollowPath(e, good, 3));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	TEST_ASSERT_EQUAL_INT(2, e->pathCount);

	/* entityPlace resets, then an invalid route is rejected with nothing
	 * queued. */
	TEST_ASSERT_TRUE(entityPlace(e, 0, 0));
	TEST_ASSERT_FALSE(entityFollowPath(e, bad, 1));
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_FALSE(entityFollowPath(NULL, good, 3));
	TEST_ASSERT_FALSE(entityFollowPath(e, good, -1));
	destroyVoxmap(map);
}

/* entityPathTo computes a route, queues it and returns its length; following
 * it under a fake dt reaches the goal and stops. */
static void test_path_to_and_follow_to_end(void)
{
	Voxmap *map = parseText("11111\n");
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 0));
	TEST_ASSERT_EQUAL_INT(4, entityPathTo(e, 4, 0));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	TEST_ASSERT_EQUAL_INT(3, e->pathCount);
	TEST_ASSERT_EQUAL_INT(0, e->pathStartX);	/* origin retained */
	TEST_ASSERT_EQUAL_INT(0, e->pathStartZ);

	/* 4 segments of 1s each (dt is clamped to 0.1s): step 0.1s at a time. */
	for (i = 0; i < 40; i++)
		entitiesUpdate(&reg, 0.1f);
	TEST_ASSERT_TRUE(e->arrived);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(4, e->tileX);
	TEST_ASSERT_EQUAL_INT(0, e->tileZ);
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 4.5f, e->x);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, e->z);

	/* Idle at the goal: another path to the same tile is a no-op that
	 * reports 0. */
	TEST_ASSERT_EQUAL_INT(0, entityPathTo(e, 4, 0));
	TEST_ASSERT_FALSE(e->moving);
	destroyVoxmap(map);
}

/* Re-path mid-walk: the remaining queue is dropped, the in-flight segment is
 * kept, and the new route is computed from that segment's target tile (the
 * entity never snaps backwards). */
static void test_repath_mid_walk(void)
{
	Voxmap *map = parseText("11111\n11111\n11111\n");
	EntityRegistry reg;
	Entity *e;
	int i;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 0));
	TEST_ASSERT_EQUAL_INT(4, entityPathTo(e, 4, 0));
	/* 25 * 0.1s: two segments done, five steps into the third, so the
	 * entity is mid-segment from (2,0) toward (3,0). */
	for (i = 0; i < 25; i++)
		entitiesUpdate(&reg, 0.1f);
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(3, e->toX);	/* in-flight target */
	TEST_ASSERT_EQUAL_INT(2, e->tileX);
	TEST_ASSERT_EQUAL_INT(1, e->pathCount);	/* (4,0) still queued */

	/* Re-path to (4,2): computed from (3,0) (the segment target). */
	TEST_ASSERT_EQUAL_INT(3, entityPathTo(e, 4, 2));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(3, e->toX);	/* unchanged in-flight */
	TEST_ASSERT_EQUAL_INT(3, e->pathCount);	/* the old route is gone */
	TEST_ASSERT_EQUAL_INT(4, e->path[e->pathHead + 2][0]);
	TEST_ASSERT_EQUAL_INT(2, e->path[e->pathHead + 2][1]);
	TEST_ASSERT_EQUAL_INT(0, e->pathStartX);	/* origin retained */

	/* Walk it out and land on (4,2). */
	for (i = 0; i < 45; i++)
		entitiesUpdate(&reg, 0.1f);
	TEST_ASSERT_FALSE(e->moving);
	TEST_ASSERT_EQUAL_INT(4, e->tileX);
	TEST_ASSERT_EQUAL_INT(2, e->tileZ);
	destroyVoxmap(map);
}

/* Re-path to the current in-flight target is an empty route: it clears the
 * queue but keeps the entity moving to the segment target. */
static void test_repath_to_segment_target(void)
{
	Voxmap *map = parseText("111111\n");
	EntityRegistry reg;
	Entity *e;

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 0));
	TEST_ASSERT_EQUAL_INT(5, entityPathTo(e, 5, 0));
	entitiesUpdate(&reg, 0.1f);	/* mid-segment toward (1,0) */
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	TEST_ASSERT_TRUE(e->moving);

	TEST_ASSERT_EQUAL_INT(0, entityPathTo(e, 1, 0));	/* == segment target */
	TEST_ASSERT_EQUAL_INT(0, e->pathCount);
	TEST_ASSERT_TRUE(e->moving);
	destroyVoxmap(map);
}

/* A failed compute leaves the entity untouched: its existing route is intact. */
static void test_path_to_failure_is_atomic(void)
{
	Voxmap *map = parseText("11111\n11.11\n1.1.1\n11.11\n11111\n");
	EntityRegistry reg;
	Entity *e;
	static const int route[1][2] = { { 1, 4 } };

	entitiesInit(&reg, map);
	e = entityGet(&reg, makeWalker(&reg, 0, 4));
	TEST_ASSERT_TRUE(entityFollowPath(e, route, 1));
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	TEST_ASSERT_TRUE(e->moving);

	/* (2,2) is enclosed by void: no path -> -1, route unchanged. */
	TEST_ASSERT_EQUAL_INT(-1, entityPathTo(e, 2, 2));
	TEST_ASSERT_TRUE(e->moving);
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	TEST_ASSERT_EQUAL_INT(4, e->toZ);
	/* A void target is rejected the same way. */
	TEST_ASSERT_EQUAL_INT(-1, entityPathTo(e, 1, 2));
	TEST_ASSERT_EQUAL_INT(1, e->toX);
	destroyVoxmap(map);
}

/* Guards: NULL entity, no map. */
static void test_path_to_guards(void)
{
	Voxmap *map = parseText("111\n");
	EntityRegistry bare;
	EntityHandle h;
	Entity *e;

	TEST_ASSERT_EQUAL_INT(-1, entityPathTo(NULL, 1, 0));
	entitiesInit(&bare, NULL);
	h = entityCreate(&bare);
	e = entityGet(&bare, h);
	TEST_ASSERT_EQUAL_INT(-1, entityPathTo(e, 1, 0));
	destroyVoxmap(map);
}

void run_test_nav(void);

void run_test_nav(void)
{
	RUN_TEST(test_follow_path);
	RUN_TEST(test_path_to_and_follow_to_end);
	RUN_TEST(test_repath_mid_walk);
	RUN_TEST(test_repath_to_segment_target);
	RUN_TEST(test_path_to_failure_is_atomic);
	RUN_TEST(test_path_to_guards);
}
