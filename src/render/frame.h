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
 * - Voxel faces are emitted with the frame tint scaled by each face's sampled
 *   light factor (`lights`, from lightgrid.h); a NULL `lights` keeps them at
 *   full brightness. `options` selects smooth per-corner lighting + AO (the
 *   default the level passes) or the flat T14 path, and the light-only debug
 *   view; NULL `options` is the flat path with no debug (backward-compatible).
 * - Each sprite's tint is scaled by the flat light factor at its base cell
 *   (`lights`, NULL = unchanged) before it is appended, so a billboard in a
 *   dark spot reads dark.
 * - Returns true once the list has been rebuilt (even if it ends up empty).
 */
#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/materials.h"
#include "render/sprites.h"
#include "render/voxmap.h"

#include <stdbool.h>
#include <stddef.h>

/* Voxel face tint. Slightly below white so voxmap's per-face shade and the
 * 1.06 checkerboard boost have headroom to brighten without clamping (with a
 * white tint, 255 * 1.06 clamps to 255 and the checkerboard is invisible on
 * the flat ground tops). The atlas texel still carries the base colour. */
#define FRAME_VOXEL_TINT DRAW_TINT(240, 240, 240, 255)

/* Per-frame render options. NULL (passed to buildFrameDrawList) is the flat
 * T14 path with no debug view. `smooth` selects per-corner light + AO;
 * `lightDebug` the light-only view; `debugUV` is the 4-corner UV of a white
 * atlas texel for that view (NULL falls back to the built-in spare region). */
typedef struct FrameOptions {
	bool smooth;
	bool lightDebug;
	const float (*debugUV)[2];
} FrameOptions;

bool buildFrameDrawList(const Voxmap *map, const MaterialTable *materials,
			const LightGrid *lights, const SpriteEntity *sprites,
			size_t count, const Camera3D *camera, DrawList *list,
			const FrameOptions *options);

#endif /* ISOMATA_RENDER_FRAME_H */
