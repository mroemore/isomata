#ifndef ISOMATA_SCENES_SETTINGS_SCENE_H
#define ISOMATA_SCENES_SETTINGS_SCENE_H

/*
 * Settings scene: a placeholder overlay (dim backdrop + pane + label + a
 * Back item) pushed as a RETAINED overlay over the menu. The menu beneath
 * stays loaded and is drawn by drawSceneStackAll; Back or cancel pops this
 * scene and reveals it. No settings functionality (YAGNI).
 */
#include "scene.h"

Scene *settingsSceneCreate(void);

#endif /* ISOMATA_SCENES_SETTINGS_SCENE_H */
