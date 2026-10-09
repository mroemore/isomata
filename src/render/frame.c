/*
 * Per-frame draw-list construction (see frame.h). Thin composition of the
 * existing pure primitives; kept here so app.c owns only the loop.
 */

#include "render/frame.h"

bool buildFrameDrawList(const Voxmap *map, const MaterialTable *materials,
			const LightGrid *lights, const SpriteEntity *sprites,
			size_t count, const Camera3D *camera, DrawList *list)
{
	size_t i;

	if (list == NULL)
		return false;

	clearDrawList(list);
	voxmapEmitFaces(map, materials, lights, list, camera, FRAME_VOXEL_TINT);
	if (sprites != NULL)
		for (i = 0; i < count; i++)
			appendSprite(list, &sprites[i], camera, materials);
	sortDrawList(list, camera);
	return true;
}
