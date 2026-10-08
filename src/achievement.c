/*
 * Achievement system (see achievement.h). Pure: it consumes gameplay events
 * from the event bus and publishes unlock events back onto it; no SDL.
 *
 * The gameplay vocabulary (EV_GAMEPLAY_CAMERA_TURNED + GameplayCameraTurn)
 * is owned by the level scene, so this module includes that header purely
 * for the constants — level_scene.h is SDL-free (it includes only scene.h).
 */

#include "achievement.h"

#include "scenes/level_scene.h"

#include <stdlib.h>
#include <string.h>

struct AchievementSystem {
	EventBus *bus;
	int turns;	/* counted EV_GAMEPLAY_CAMERA_TURNED events */
	bool unlocked;
};

static void achievementOnGameplay(void *ctx, const Event *event)
{
	AchievementSystem *sys = ctx;
	AchievementUnlocked payload;

	/* The system is only subscribed to EV_TOPIC_GAMEPLAY, but the type
	 * check keeps it correct if the topic gains other events. */
	if (event->topic != EV_TOPIC_GAMEPLAY ||
	    event->type != EV_GAMEPLAY_CAMERA_TURNED)
		return;
	if (event->payload == NULL ||
	    event->payloadSize < sizeof(GameplayCameraTurn))
		return;		/* malformed: not a counted turn */

	sys->turns++;
	if (sys->unlocked || sys->turns < ACHIEVEMENT_TURN_THRESHOLD)
		return;

	sys->unlocked = true;
	payload.id = ACHIEVEMENT_ORIENTEER_ID;
	payload.title = ACHIEVEMENT_ORIENTEER_TITLE;
	/* Snapshot semantics: this queues for the NEXT dispatch. */
	publishEvent(sys->bus, EV_TOPIC_ACHIEVEMENT, EV_ACHIEVEMENT_UNLOCKED,
		     &payload, sizeof(payload));
}

AchievementSystem *createAchievementSystem(EventBus *bus)
{
	AchievementSystem *sys;

	if (bus == NULL)
		return NULL;
	sys = calloc(1, sizeof(*sys));
	if (sys == NULL)
		return NULL;
	sys->bus = bus;
	if (!subscribeEvent(bus, EV_TOPIC_GAMEPLAY, achievementOnGameplay,
			    sys)) {
		free(sys);
		return NULL;
	}
	return sys;
}

void destroyAchievementSystem(AchievementSystem *system)
{
	if (system == NULL)
		return;
	unsubscribeEvent(system->bus, EV_TOPIC_GAMEPLAY, achievementOnGameplay,
			 system);
	free(system);
}

bool achievementUnlocked(const AchievementSystem *system, const char *id)
{
	if (system == NULL || id == NULL || !system->unlocked)
		return false;
	return strcmp(id, ACHIEVEMENT_ORIENTEER_ID) == 0;
}
