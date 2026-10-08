#ifndef ISOMATA_RENDER_SPRITES_H
#define ISOMATA_RENDER_SPRITES_H

/*
 * Camera-anchored billboard sprites. Pure: math3d + camera3d only.
 *
 * A SpriteEntity is an axis-free billboard anchored at a world position (its
 * bottom-centre) with a world-space size and an RGBA tint. Its quad is built
 * from the camera's right/up basis (taken from cameraView, so no yaw math is
 * duplicated here), which keeps it facing the camera at any yaw, including
 * mid-tween.
 *
 * Sprites sample the atlas sprite region (ATLAS_UV_SPRITE) and are appended
 * as DRAW_KIND_SPRITE items, so they join the same painter-sorted list as the
 * voxel faces and blend over terrain at equal depth.
 *
 * Note: the plan's sketch listed appendSprite(DrawList*, const SpriteEntity*).
 * Controller ruling #7 requires the billboard to be built from the camera's
 * right/up basis, so the camera is an explicit parameter here.
 */

#include "render/camera3d.h"
#include "render/drawlist.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct SpriteEntity {
	float x;	/* world anchor x (billboard bottom-centre) */
	float y;	/* world anchor y */
	float z;	/* world anchor z */
	float width;	/* world-space billboard width */
	float height;	/* world-space billboard height */
	uint32_t tint;	/* RGBA (see DRAW_TINT) */
} SpriteEntity;

/* Build the camera-facing billboard quad into worldQuad (4 corners, canonical
 * order: bottom-left, bottom-right, top-right, top-left). A NULL sprite writes
 * an all-zero quad; a NULL worldQuad is a no-op. */
void buildSpriteQuad(const SpriteEntity *sprite, const Camera3D *camera,
		     float worldQuad[4][3]);

/* Append the sprite as a DRAW_KIND_SPRITE item (ATLAS_UV_SPRITE). Returns
 * false on a NULL sprite, a NULL list, or a full list. */
bool appendSprite(DrawList *list, const SpriteEntity *sprite,
		  const Camera3D *camera);

#endif /* ISOMATA_RENDER_SPRITES_H */
