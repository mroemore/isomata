/*
 * Draw list (see drawlist.h for the contract). Pure: camera3d + math3d +
 * stdlib only, no SDL.
 *
 * Storage is one malloc at init; append never allocates. The sort is a stable
 * insertion sort — O(n^2) but n is a bounded map's visible face count, and
 * stability is exactly the "insertion index" tie-break the header pins, so no
 * extra key array is needed. Depth is recomputed per comparison (a cheap key
 * point — the base line for a vertical face, else the 4-point centre — plus
 * one mat4 transform); at these sizes that is far below a frame's budget.
 */

#include "render/drawlist.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How far a sprite is pulled toward the camera for the painter sort. A
 * billboard's quad centre = base + up*h/2 and the camera up axis is
 * perpendicular to the view direction, so the sprite's centre has EXACTLY
 * its anchor's view depth: a sprite standing on a tile centre ties with that
 * tile's top face and float noise decides the comparison. The bias is far
 * above the noise (~1e-6) and far below any real occlusion separation
 * (>= ~0.29 world units for the demo's configurations). */
#define DRAW_SPRITE_DEPTH_BIAS 0.02f

/* A voxel face is treated as VERTICAL when its corner heights differ by more
 * than this. Real faces are axis-aligned with integer-ish corners, so a side
 * face spans >= 1.0 and a top face exactly 0.0; the epsilon only rejects
 * float noise, never a real face. */
#define DRAW_FACE_VERTICAL_EPS 1e-4f

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
		     const float uv[4][2], uint8_t alphaMode, uint32_t tint)
{
	uint32_t cornerTint[4] = { tint, tint, tint, tint };

	return appendVoxelFaceShaded(list, worldQuad, uv, alphaMode,
				     cornerTint);
}

bool appendVoxelFaceShaded(DrawList *list, const float worldQuad[4][3],
			   const float uv[4][2], uint8_t alphaMode,
			   const uint32_t cornerTint[4])
{
	DrawItem item;

	if (worldQuad == NULL || uv == NULL || cornerTint == NULL)
		return false;
	memcpy(item.worldQuad, worldQuad, sizeof(item.worldQuad));
	memcpy(item.uv, uv, sizeof(item.uv));
	memcpy(item.cornerTint, cornerTint, sizeof(item.cornerTint));
	item.tint = cornerTint[0];
	item.kind = DRAW_KIND_VOXEL;
	item.alphaMode = alphaMode;
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

/* True when the quad's corners span a height range: a side face, not a top. */
static bool faceIsVertical(const DrawItem *item)
{
	float minY = item->worldQuad[0][1];
	float maxY = item->worldQuad[0][1];
	int i;

	for (i = 1; i < 4; i++) {
		float y = item->worldQuad[i][1];

		if (y < minY)
			minY = y;
		if (y > maxY)
			maxY = y;
	}
	return (maxY - minY) > DRAW_FACE_VERTICAL_EPS;
}

/* Midpoint of the quad's TWO LOWEST corners — the base line, where a vertical
 * face meets the surface it stands on. */
static void itemBaseLine(const DrawItem *item, float out[3])
{
	int lo0 = 0;
	int lo1 = 1;
	float y0;
	float y1;
	int i;

	if (item->worldQuad[lo1][1] < item->worldQuad[lo0][1]) {
		lo0 = 1;
		lo1 = 0;
	}
	y0 = item->worldQuad[lo0][1];
	y1 = item->worldQuad[lo1][1];
	for (i = 2; i < 4; i++) {
		float y = item->worldQuad[i][1];

		if (y < y0) {
			lo1 = lo0;
			y1 = y0;
			lo0 = i;
			y0 = y;
		} else if (y < y1) {
			lo1 = i;
			y1 = y;
		}
	}
	out[0] = (item->worldQuad[lo0][0] + item->worldQuad[lo1][0]) * 0.5f;
	out[1] = (item->worldQuad[lo0][1] + item->worldQuad[lo1][1]) * 0.5f;
	out[2] = (item->worldQuad[lo0][2] + item->worldQuad[lo1][2]) * 0.5f;
}

/* The painter key point: the point whose camera-space depth is the item's
 * sort key. A vertical VOXEL face keys on its base line (a tall face's centre
 * depth is dominated by its height, which does not interact with anything
 * standing at ground level — the base line is the depth at which the face
 * meets the world). Everything else (top faces, sprites) keys on the quad
 * centre. Sprites are billboards whose centre already has their anchor's
 * depth, so their bias (below) is applied to this same point. */
static void itemKeyPoint(const DrawItem *item, float out[3])
{
	if (item->kind == DRAW_KIND_VOXEL && faceIsVertical(item))
		itemBaseLine(item, out);
	else
		itemCentre(item, out);
}

static float itemDepth(const DrawItem *item, const Mat4 *view)
{
	float c[3];
	Vec4 v;
	float depth;

	itemKeyPoint(item, c);
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
	itemKeyPoint(a, ca);
	itemKeyPoint(b, cb);
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
