#ifndef ISOMATA_SCENES_PAUSE_SCENE_H
#define ISOMATA_SCENES_PAUSE_SCENE_H

/*
 * Pause scene: a retained overlay over the frozen level. Resume pops back to
 * the level; Quit to Menu replaces the whole stack with the menu (unloading
 * the level). The level beneath stays loaded and is still DRAWN (dimmed) via
 * drawSceneStackAll.
 */
#include "scene.h"

Scene *pauseSceneCreate(void);

#endif /* ISOMATA_SCENES_PAUSE_SCENE_H */
