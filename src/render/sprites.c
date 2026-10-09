/*
 * Sprites (see sprites.h for the contract). Pure: math3d + camera3d only.
 *
 * The billboard basis is read straight from cameraView: its row 0 is the
 * camera's right vector and row 1 its up vector (both unit length from the
 * look-at basis), so the quad always faces the camera without duplicating any
 * yaw/pitch math here. Column-major storage means row 0 is
 * (m[0], m[4], m[8]) and row 1 is (m[1], m[5], m[9]).
 */

#include "render/sprites.h"
#include "render/textures.h"

#include <string.h>

static const float kFallbackUV[4][2] = ATLAS_UV_SPRITE;

void buildSpriteQuad(const SpriteEntity *sprite, const Camera3D *camera,
		     float worldQuad[4][3])
{
	Mat4 view;
	float rx;
	float ry;
	float rz;
	float ux;
	float uy;
	float uz;
	float hw;
	float h;
	int i;

	if (worldQuad == NULL)
		return;
	if (sprite == NULL) {
		for (i = 0; i < 4; i++) {
			worldQuad[i][0] = 0.0f;
			worldQuad[i][1] = 0.0f;
			worldQuad[i][2] = 0.0f;
		}
		return;
	}

	view = cameraView(camera);
	rx = view.m[0];
	ry = view.m[4];
	rz = view.m[8];
	ux = view.m[1];
	uy = view.m[5];
	uz = view.m[9];
	hw = sprite->width * 0.5f;
	h = sprite->height;

	/* bottom-left, bottom-right, top-right, top-left */
	worldQuad[0][0] = sprite->x - rx * hw;
	worldQuad[0][1] = sprite->y - ry * hw;
	worldQuad[0][2] = sprite->z - rz * hw;
	worldQuad[1][0] = sprite->x + rx * hw;
	worldQuad[1][1] = sprite->y + ry * hw;
	worldQuad[1][2] = sprite->z + rz * hw;
	worldQuad[2][0] = sprite->x + rx * hw + ux * h;
	worldQuad[2][1] = sprite->y + ry * hw + uy * h;
	worldQuad[2][2] = sprite->z + rz * hw + uz * h;
	worldQuad[3][0] = sprite->x - rx * hw + ux * h;
	worldQuad[3][1] = sprite->y - ry * hw + uy * h;
	worldQuad[3][2] = sprite->z - rz * hw + uz * h;
}

bool appendSprite(DrawList *list, const SpriteEntity *sprite,
		  const Camera3D *camera, const MaterialTable *materials)
{
	DrawItem item;

	if (sprite == NULL)
		return false;
	buildSpriteQuad(sprite, camera, item.worldQuad);
	item.alphaMode = (uint8_t)ALPHA_BLEND;
	if (materials != NULL && sprite->material >= 0 &&
	    (size_t)sprite->material < materials->count) {
		const Material *m = &materials->items[sprite->material];

		materialFaceUV(&m->rect[FACE_SOUTH], FACE_SOUTH, item.uv);
		item.alphaMode = (uint8_t)m->alpha;
	} else {
		memcpy(item.uv, kFallbackUV, sizeof(item.uv));
	}
	item.tint = sprite->tint;
	item.kind = DRAW_KIND_SPRITE;
	return appendDrawItem(list, &item);
}
