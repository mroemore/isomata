#ifndef ISOMATA_SCENES_LEVEL_SCENE_H
#define ISOMATA_SCENES_LEVEL_SCENE_H

/*
 * Level scene: owns the demo voxmap, its billboards and a Camera3D. Each
 * frame it builds the painter-sorted draw list (pure render/frame.c) and
 * draws it through the SDL_gpu backend, then applies the non-UI commands
 * (rotate / zoom / pan) and pushes the pause overlay on CMD_BACK. The level
 * has no UI yet, so uiBridgeDispatch runs with a NULL root.
 */
#include "scene.h"

Scene *levelSceneCreate(void);

#endif /* ISOMATA_SCENES_LEVEL_SCENE_H */
