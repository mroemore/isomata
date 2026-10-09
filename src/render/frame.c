/*
 * Per-frame draw-list construction (see frame.h). Thin composition of the
 * existing pure primitives; kept here so app.c owns only the loop.
 */

#include "render/frame.h"

bool buildFrameDrawList(const Voxmap *map, const MaterialTable *materials,
			const LightGrid *lights, const SpriteEntity *sprites,
			size_t count, const Camera3D *camera, DrawList *list,
			const FrameOptions *options)
{
	VoxmapEmitOptions emit = { FRAME_VOXEL_TINT, false, false, NULL };
	size_t i;

	if (list == NULL)
		return false;
	if (options != NULL) {
		emit.smooth = options->smooth;
		emit.lightDebug = options->lightDebug;
		emit.debugUV = options->debugUV;
	}

	clearDrawList(list);
	voxmapEmitFacesOpt(map, materials, lights, list, camera, &emit);
	if (sprites != NULL)
		for (i = 0; i < count; i++) {
			/* Sprites get the flat light factor at their base
			 * cell; a NULL grid leaves the tint unchanged. */
			SpriteEntity lit = sprites[i];

			lit.tint = spriteApplyLight(&sprites[i], lights);
			appendSprite(list, &lit, camera, materials);
		}
	sortDrawList(list, camera);
	return true;
}
