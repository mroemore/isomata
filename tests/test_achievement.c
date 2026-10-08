/*
 * Achievement system tests (CTOL rung 1: unit + boundary).
 *
 * Covers: the eight-step threshold (no unlock at 7), the unlock payload
 * (id/title), exactly-once idempotence on further turns, the deferred
 * (snapshot) delivery of the unlock event, ignoring other/malformed events,
 * NULL safety, and unsubscribe-on-destroy.
 *
 * Pure: links only achievement.c + events.c plus the Unity subset; no SDL.
 *
 * Harness convention: no main()/setUp()/tearDown(); exposes
 * run_test_achievement().
 */

#include "unity.h"

#include "achievement.h"
#include "events.h"
#include "scenes/level_scene.h"

#include <stdio.h>
#include <string.h>

/* Records EV_ACHIEVEMENT_UNLOCKED deliveries. */
static int g_unlocks;
static char g_id[32];
static char g_title[32];

static void reset_recorder(void)
{
	g_unlocks = 0;
	g_id[0] = '\0';
	g_title[0] = '\0';
}

static void onUnlock(void *ctx, const Event *event)
{
	const AchievementUnlocked *u;

	(void)ctx;
	if (event->topic != EV_TOPIC_ACHIEVEMENT ||
	    event->type != EV_ACHIEVEMENT_UNLOCKED)
		return;
	g_unlocks++;
	if (event->payload != NULL &&
	    event->payloadSize >= sizeof(AchievementUnlocked)) {
		u = event->payload;
		snprintf(g_id, sizeof(g_id), "%s", u->id);
		snprintf(g_title, sizeof(g_title), "%s", u->title);
	}
}

static void publish_turn(EventBus *bus, int direction, int step)
{
	GameplayCameraTurn turn = { direction, step };

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY,
				      EV_GAMEPLAY_CAMERA_TURNED, &turn,
				      sizeof(turn)));
}

/* --- tests ------------------------------------------------------------ */

/* The threshold fires on the 8th step, never before; the unlock event is
 * queued (snapshot) and delivered on the next dispatch. */
static void test_threshold_fires_on_eighth_step(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;
	int i;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ACHIEVEMENT, onUnlock,
					NULL));
	reset_recorder();

	for (i = 1; i <= 8; i++) {
		publish_turn(bus, 1, i);
		dispatchEvents(bus);
	}

	/* Unlocked, but the unlock event was published from inside the 8th
	 * dispatch, so it lands on the next one. */
	TEST_ASSERT_TRUE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));
	TEST_ASSERT_EQUAL_INT(0, g_unlocks);

	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_unlocks);
	TEST_ASSERT_EQUAL_STRING(ACHIEVEMENT_ORIENTEER_ID, g_id);
	TEST_ASSERT_EQUAL_STRING(ACHIEVEMENT_ORIENTEER_TITLE, g_title);

	destroyAchievementSystem(sys);
	destroyEventBus(bus);
}

/* Seven steps do not unlock. */
static void test_no_unlock_before_eight(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;
	int i;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ACHIEVEMENT, onUnlock,
					NULL));
	reset_recorder();

	for (i = 1; i <= 7; i++) {
		publish_turn(bus, -1, i);
		dispatchEvents(bus);
	}
	dispatchEvents(bus);	/* nothing extra to deliver */

	TEST_ASSERT_FALSE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));
	TEST_ASSERT_EQUAL_INT(0, g_unlocks);

	destroyAchievementSystem(sys);
	destroyEventBus(bus);
}

/* Once unlocked, further steps never re-publish. */
static void test_unlock_is_exactly_once(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;
	int i;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ACHIEVEMENT, onUnlock,
					NULL));
	reset_recorder();

	for (i = 1; i <= 8; i++) {
		publish_turn(bus, 1, i);
		dispatchEvents(bus);
	}
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_unlocks);

	for (i = 9; i <= 13; i++) {
		publish_turn(bus, 1, i);
		dispatchEvents(bus);
	}
	dispatchEvents(bus);
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_unlocks);
	TEST_ASSERT_TRUE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));

	destroyAchievementSystem(sys);
	destroyEventBus(bus);
}

/* Other event types, NULL/short payloads, and other topics are ignored. */
static void test_ignores_other_and_malformed_events(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;
	GameplayCameraTurn turn = { 1, 1 };
	int i;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);

	/* Wrong type on the gameplay topic. */
	for (i = 0; i < 5; i++) {
		TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 7u, &turn,
					      sizeof(turn)));
		dispatchEvents(bus);
	}
	TEST_ASSERT_FALSE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));

	/* Correct type but NULL / too-small payload. */
	for (i = 0; i < 5; i++) {
		TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY,
					      EV_GAMEPLAY_CAMERA_TURNED, NULL,
					      0));
		TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY,
					      EV_GAMEPLAY_CAMERA_TURNED, &turn,
					      sizeof(turn) - 1));
		dispatchEvents(bus);
	}
	TEST_ASSERT_FALSE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));

	/* Correct type but the wrong topic: the system is not subscribed. */
	for (i = 0; i < 5; i++) {
		TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE,
					      EV_GAMEPLAY_CAMERA_TURNED, &turn,
					      sizeof(turn)));
		dispatchEvents(bus);
	}
	TEST_ASSERT_FALSE(achievementUnlocked(sys, ACHIEVEMENT_ORIENTEER_ID));

	destroyAchievementSystem(sys);
	destroyEventBus(bus);
}

/* Unknown ids and NULL queries are false. */
static void test_unlock_query(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);

	TEST_ASSERT_FALSE(achievementUnlocked(sys, "nope"));
	TEST_ASSERT_FALSE(achievementUnlocked(sys, NULL));
	TEST_ASSERT_FALSE(achievementUnlocked(NULL, ACHIEVEMENT_ORIENTEER_ID));

	destroyAchievementSystem(sys);
	destroyEventBus(bus);
}

/* NULL safety and rejected creation. */
static void test_null_and_edge_arguments(void)
{
	EventBus *bus = createEventBus(4);
	AchievementSystem *sys;

	TEST_ASSERT_NOT_NULL(bus);
	TEST_ASSERT_NULL(createAchievementSystem(NULL));
	destroyAchievementSystem(NULL);	/* harmless */

	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);
	destroyAchievementSystem(sys);

	destroyEventBus(bus);
}

/* destroy unsubscribes: after destroy, gameplay turns produce no unlock. */
static void test_destroy_unsubscribes(void)
{
	EventBus *bus = createEventBus(16);
	AchievementSystem *sys;
	int i;

	TEST_ASSERT_NOT_NULL(bus);
	sys = createAchievementSystem(bus);
	TEST_ASSERT_NOT_NULL(sys);
	destroyAchievementSystem(sys);

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ACHIEVEMENT, onUnlock,
					NULL));
	reset_recorder();

	for (i = 1; i <= 8; i++) {
		publish_turn(bus, 1, i);
		dispatchEvents(bus);
	}
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(0, g_unlocks);

	destroyEventBus(bus);
}

/* --- runner ----------------------------------------------------------- */

void run_test_achievement(void);

void run_test_achievement(void)
{
	RUN_TEST(test_threshold_fires_on_eighth_step);
	RUN_TEST(test_no_unlock_before_eight);
	RUN_TEST(test_unlock_is_exactly_once);
	RUN_TEST(test_ignores_other_and_malformed_events);
	RUN_TEST(test_unlock_query);
	RUN_TEST(test_null_and_edge_arguments);
	RUN_TEST(test_destroy_unsubscribes);
}
