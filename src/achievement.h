#ifndef ISOMATA_ACHIEVEMENT_H
#define ISOMATA_ACHIEVEMENT_H

/*
 * Achievement system: a pure event-bus consumer that watches gameplay events
 * and publishes unlock events. Headless: no SDL, no GPU, no window.
 *
 * Invariants:
 * - createAchievementSystem(bus) subscribes the new system to
 *   EV_TOPIC_GAMEPLAY on the given bus (it owns the subscription). A NULL
 *   bus, an allocation failure, or a failed subscribe returns NULL (the
 *   partial state is released; no leak).
 * - The system counts EV_GAMEPLAY_CAMERA_TURNED events (payload
 *   GameplayCameraTurn, declared with the event in level_scene.h). Events of
 *   any other topic/type, and camera-turn events whose payload is NULL or
 *   smaller than GameplayCameraTurn, are ignored.
 * - At exactly the ACHIEVEMENT_TURN_THRESHOLD-th counted turn it publishes
 *   EV_ACHIEVEMENT_UNLOCKED once, with an AchievementUnlocked payload naming
 *   the "orienteer" achievement. It NEVER publishes it again: further turns
 *   are counted but the unlock is idempotent. No event is published before
 *   the threshold.
 * - destroyAchievementSystem unsubscribes from the bus (using the exact
 *   callback+ctx the system subscribed with) and frees it; it tolerates NULL.
 *   The bus must outlive the system.
 *
 * Event vocabulary (topic EV_TOPIC_ACHIEVEMENT, per-topic types start at 0):
 * - EV_ACHIEVEMENT_UNLOCKED: payload AchievementUnlocked. The two `const
 *   char *` fields point at string literals owned by this module, so they
 *   have static lifetime and stay valid for as long as the payload copy
 *   exists (the bus copies the struct at publish time; the pointed-to text
 *   is not copied, which is fine for static literals).
 */

#include "events.h"

#include <stdbool.h>

/* The achievement unlocked on the 8th counted rotation step (a full circle
 * at 45-degree steps). */
#define ACHIEVEMENT_TURN_THRESHOLD 8
#define ACHIEVEMENT_ORIENTEER_ID "orienteer"
#define ACHIEVEMENT_ORIENTEER_TITLE "ORIENTEER"

/* Event type (topic EV_TOPIC_ACHIEVEMENT). */
enum {
	EV_ACHIEVEMENT_UNLOCKED = 0,
};

/* Payload for EV_ACHIEVEMENT_UNLOCKED. Both strings are static-lifetime. */
typedef struct AchievementUnlocked {
	const char *id;
	const char *title;
} AchievementUnlocked;

typedef struct AchievementSystem AchievementSystem;

/* Subscribe a new system to EV_TOPIC_GAMEPLAY on `bus`. NULL on NULL bus,
 * allocation failure, or failed subscribe. */
AchievementSystem *createAchievementSystem(EventBus *bus);

/* Unsubscribe and free. Tolerates NULL. */
void destroyAchievementSystem(AchievementSystem *system);

/* True when `id` names an achievement the system has unlocked. NULL system,
 * NULL id, and unknown ids are false. */
bool achievementUnlocked(const AchievementSystem *system, const char *id);

#endif /* ISOMATA_ACHIEVEMENT_H */
