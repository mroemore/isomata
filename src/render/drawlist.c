/*
 * Draw list (see drawlist.h for the contract). Pure: camera3d + math3d +
 * stdlib only, no SDL.
 *
 * Storage is one malloc at init; append never allocates. The sort is a stable
 * insertion sort — O(n^2) but n is a bounded map's visible face count, and
 * stability is exactly the "insertion index" tie-break the header pins, so no
 * extra key array is needed. Depth is recomputed per comparison (a cheap
 * 4-point centre + one mat4 transform); at these sizes that is far below a
 * frame's budget.
 */

#include "render/drawlist.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float kTopUV[4][2] = ATLAS_UV_TOP;
static const float kSideUV[4][2] = ATLAS_UV_SIDE;

/* How far a sprite is pulled toward the camera for the painter sort. A
 * billboard's quad centre = base + up*h/2 and the camera up axis is
 * perpendicular to the view direction, so the sprite's centre has EXACTLY
 * its anchor's view depth: a sprite standing on a tile centre ties with that
 * tile's top face and float noise decides the comparison. The bias is far
 * above the noise (~1e-6) and far below any real occlusion separation
 * (>= ~0.29 world units for the demo's configurations). */
#define DRAW_SPRITE_DEPTH_BIAS 0.02f

void initDrawList(DrawList *list, size_t capacity)
{
	if (list == NULL)
		return;
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
	if (capacity == 0)
		return;
	list->items = malloc(capacity * sizeof(*list->items));
	if (list->items == NULL)
		return;		/* stays empty; append will refuse */
	list->capacity = capacity;
}

void destroyDrawList(DrawList *list)
{
	if (list == NULL)
		return;
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

void clearDrawList(DrawList *list)
{
	if (list == NULL)
		return;
	list->count = 0;
}

size_t drawListCount(const DrawList *list)
{
	if (list == NULL)
		return 0;
	return list->count;
}

const DrawItem *drawListItem(const DrawList *list, size_t index)
{
	if (list == NULL || index >= list->count)
		return NULL;
	return &list->items[index];
}

bool appendDrawItem(DrawList *list, const DrawItem *item)
{
	if (list == NULL || item == NULL || list->items == NULL)
		return false;
	if (list->count >= list->capacity)
		return false;
	list->items[list->count++] = *item;
	return true;
}

bool appendVoxelFace(DrawList *list, const float worldQuad[4][3],
		     DrawFace face, uint32_t tint)
{
	DrawItem item;

	if (worldQuad == NULL)
		return false;
	memcpy(item.worldQuad, worldQuad, sizeof(item.worldQuad));
	if (face == DRAW_FACE_SIDE)
		memcpy(item.uv, kSideUV, sizeof(item.uv));
	else
		memcpy(item.uv, kTopUV, sizeof(item.uv));
	item.tint = tint;
	item.kind = DRAW_KIND_VOXEL;
	return appendDrawItem(list, &item);
}

/* --- painter sort ------------------------------------------------------ */

static void itemCentre(const DrawItem *item, float out[3])
{
	out[0] = (item->worldQuad[0][0] + item->worldQuad[1][0] +
		  item->worldQuad[2][0] + item->worldQuad[3][0]) * 0.25f;
	out[1] = (item->worldQuad[0][1] + item->worldQuad[1][1] +
		  item->worldQuad[2][1] + item->worldQuad[3][1]) * 0.25f;
	out[2] = (item->worldQuad[0][2] + item->worldQuad[1][2] +
		  item->worldQuad[2][2] + item->worldQuad[3][2]) * 0.25f;
}

static float itemDepth(const DrawItem *item, const Mat4 *view)
{
	float c[3];
	Vec4 v;
	float depth;

	itemCentre(item, c);
	v = mat4TransformPoint(view, (Vec3){ c[0], c[1], c[2] });
	depth = v.z;
	/* View-space z grows toward the camera (the camera looks down -Z), so
	 * adding the bias moves a sprite toward the camera — it draws after a
	 * coplanar face and never flickers against it. */
	if (item->kind == DRAW_KIND_SPRITE)
		depth += DRAW_SPRITE_DEPTH_BIAS;
	return depth;
}

/* True when `a` must be drawn AFTER `b` (painter order). Equal keys return
 * false, which is what makes the insertion sort stable. */
static bool comesAfter(const DrawItem *a, const DrawItem *b, const Mat4 *view)
{
	float da = itemDepth(a, view);
	float db = itemDepth(b, view);
	float ca[3];
	float cb[3];

	if (da != db)
		return da > db;		/* farther (more negative view z) first */
	if (a->kind != b->kind)
		return a->kind > b->kind;	/* VOXEL before SPRITE */
	itemCentre(a, ca);
	itemCentre(b, cb);
	if (ca[0] != cb[0])
		return ca[0] > cb[0];
	if (ca[1] != cb[1])
		return ca[1] > cb[1];
	return ca[2] > cb[2];
}

void sortDrawList(DrawList *list, const Camera3D *camera)
{
	Mat4 view;
	size_t i;
	size_t j;

	if (list == NULL || list->items == NULL)
		return;
	view = cameraView(camera);
	for (i = 1; i < list->count; i++) {
		DrawItem key = list->items[i];

		j = i;
		while (j > 0 && comesAfter(&list->items[j - 1], &key, &view)) {
			list->items[j] = list->items[j - 1];
			j--;
		}
		list->items[j] = key;
	}
}
