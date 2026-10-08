#ifndef ISOMATA_SCENES_LEVEL_SCENE_H
#define ISOMATA_SCENES_LEVEL_SCENE_H

/*
 * Level scene: owns the demo voxmap, its billboards and a Camera3D. Each
 * frame it builds the painter-sorted draw list (pure render/frame.c) and
 * draws it through the SDL_gpu backend, then applies the non-UI commands
 * (rotate / zoom / pan) and pushes the pause overlay on CMD_BACK. Task 10
 * adds the achievement toast (a UI root) and the gameplay/audio events the
 * level publishes through the app's event bus.
 *
 * Event vocabulary (topic EV_TOPIC_GAMEPLAY, per-topic types start at 0):
 * the level owns the camera-turn event it emits on every successful rotation
 * step. The bus copies the GameplayCameraTurn payload at publish time.
 */

#include "scene.h"

/* Event type (topic EV_TOPIC_GAMEPLAY). */
enum {
	EV_GAMEPLAY_CAMERA_TURNED = 0,
};

/* Payload for EV_GAMEPLAY_CAMERA_TURNED. `direction` is +1 clockwise, -1
 * counter-clockwise (never 0: only a successful step is published); `step`
 * is the 1-based running count of 45-degree steps the level has applied
 * (monotonic). */
typedef struct GameplayCameraTurn {
	int direction;
	int step;
} GameplayCameraTurn;

Scene *levelSceneCreate(void);

#endif /* ISOMATA_SCENES_LEVEL_SCENE_H */
