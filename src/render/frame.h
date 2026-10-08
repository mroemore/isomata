#ifndef ISOMATA_RENDER_FRAME_H
#define ISOMATA_RENDER_FRAME_H

/*
 * Per-frame draw-list construction. Pure: math3d/camera3d/voxmap/drawlist/
 * sprites only, no SDL.
 *
 * This is the smallest piece of the frame the app used to inline (Task 8
 * scaffolding in app.c): clear the list, emit every visible voxel face of
 * `map`, append `count` billboard sprites, then painter-sort the result for
 * `camera`. Extracting it here makes the construction headless-testable
 * (Task 9 controller ruling #1) and keeps app.c owning only the loop.
 *
 * Contract:
 * - `list` must already be initialized (initDrawList). A NULL list returns
 *   false and touches nothing.
 * - A NULL `map` emits no voxel faces (sprites are still appended); a NULL
 *   `sprites` with count > 0 appends nothing; a NULL `camera` sorts against
 *   the identity view (drawlist.h's rule).
 * - Voxel faces are emitted opaque white (the atlas texel carries the
 *   colour); sprites keep their own tints.
 * - Returns true once the list has been rebuilt (even if it ends up empty).
 */
#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/sprites.h"
#include "render/voxmap.h"

#include <stdbool.h>
#include <stddef.h>

/* Opaque white: the placeholder atlas region supplies the actual colour. */
#define FRAME_VOXEL_TINT DRAW_TINT(255, 255, 255, 255)

bool buildFrameDrawList(const Voxmap *map, const SpriteEntity *sprites,
			size_t count, const Camera3D *camera, DrawList *list);

#endif /* ISOMATA_RENDER_FRAME_H */
