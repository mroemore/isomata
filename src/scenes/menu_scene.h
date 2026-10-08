#ifndef ISOMATA_SCENES_MENU_SCENE_H
#define ISOMATA_SCENES_MENU_SCENE_H

/*
 * Start menu scene: title, a Start/Settings/Quit menu, and a hint line.
 * SDL-tier (engine_sources): it builds a UI tree and dispatches input
 * through the pure ui_bridge. Start replaces the stack with the level,
 * Settings pushes the retained settings overlay, Quit asks the app to stop.
 */
#include "scene.h"

Scene *menuSceneCreate(void);

#endif /* ISOMATA_SCENES_MENU_SCENE_H */
