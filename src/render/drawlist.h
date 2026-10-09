#ifndef ISOMATA_RENDER_DRAWLIST_H
#define ISOMATA_RENDER_DRAWLIST_H

/*
 * Painter-sorted draw list: the render tier's frame currency.
 *
 * Every visible quad (voxel face or sprite billboard) becomes one DrawItem in
 * world space with atlas UVs and an RGBA tint. The GPU tier uploads the list
 * as vertices and issues ONE draw call; there is no depth buffer, so
 * sortDrawList must put the items in painter's order (far to near) before the
 * draw.
 *
 * Invariants:
 * - Storage is preallocated at initDrawList and never grows during a frame:
 *   appendDrawItem is capacity-bounded and returns false when full (no
 *   reallocation, no per-frame allocation). initDrawList allocates the item
 *   array; destroyDrawList releases it.
 * - A DrawItem is plain data. worldQuad[4][3] and uv[4][2] share the SAME
 *   canonical corner order (index 0..3 = bottom-left, bottom-right,
 *   top-right, top-left for a billboard / the equivalent quad corners for a
 *   face), so a corner and its UV always line up.
 * - tint is packed RGBA: r in bits 24..31, g 16..23, b 8..15, a 0..7. Use
 *   DRAW_TINT.
 * - sortDrawList is painter's back-to-front by the camera-space depth of the
 *   quad centre (transform by cameraView; farther = more negative view z =
 *   drawn first). Sprites are biased toward the camera (their comparison
 *   depth is raised by DRAW_SPRITE_DEPTH_BIAS, see drawlist.c) so a sprite
 *   standing on a tile never flickers against that tile's top face: a
 *   billboard's centre is perpendicular to the view direction and would
 *   otherwise tie exactly with the anchor it stands on. Because the bias
 *   separates a sprite from a coplanar face, the kind tie-break below is
 *   only reached when two keys are exactly equal AFTER the bias (two
 *   coplanar faces, or a face and a sprite at the same biased depth); the
 *   common sprite-vs-face tie is resolved by the bias, not by kind. Ties
 *   break deterministically: depth, then kind (VOXEL before SPRITE, so a
 *   transparent billboard blends over opaque terrain at the same depth),
 *   then centre position lexicographically (x, then y, then z), then
 *   insertion order (the sort is a stable insertion sort, so equal keys keep
 *   the order they were appended in).
 */

#include "render/camera3d.h"
#include "render/textures.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pack an RGBA tint for DrawItem.tint. */
#define DRAW_TINT(r, g, b, a) \
	(((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | \
	 ((uint32_t)(b) << 8) | (uint32_t)(a))

typedef enum DrawKind {
	DRAW_KIND_VOXEL = 0,	/* opaque terrain face */
	DRAW_KIND_SPRITE = 1,	/* alpha-blended billboard */
} DrawKind;

/* Which atlas region a voxel face samples. */
typedef enum DrawFace {
	DRAW_FACE_TOP = 0,	/* ATLAS_UV_TOP */
	DRAW_FACE_SIDE = 1,	/* ATLAS_UV_SIDE */
} DrawFace;

typedef struct DrawItem {
	float worldQuad[4][3];	/* 4 corners, world space, canonical order */
	float uv[4][2];		/* matching atlas UVs */
	uint32_t tint;		/* RGBA (see DRAW_TINT) */
	uint8_t kind;		/* DRAW_KIND_* */
} DrawItem;

typedef struct DrawList {
	DrawItem *items;	/* capacity slots; private storage */
	size_t count;
	size_t capacity;
} DrawList;

/* Initialize an empty list with room for `capacity` items. Allocates the item
 * array once. A NULL list, or capacity 0, yields an empty (never-appendable)
 * list; an allocation failure also leaves the list empty. */
void initDrawList(DrawList *list, size_t capacity);

/* Release the item array and reset to empty. NULL is a no-op. */
void destroyDrawList(DrawList *list);

/* Drop all items but keep the storage (the per-frame reset). NULL is a
 * no-op. */
void clearDrawList(DrawList *list);

/* Item count (0 for a NULL list). */
size_t drawListCount(const DrawList *list);

/* Borrowed item at index, or NULL when out of range. */
const DrawItem *drawListItem(const DrawList *list, size_t index);

/* Core append: copy one item. Returns false on NULL args, an unallocated
 * list, or a full list. */
bool appendDrawItem(DrawList *list, const DrawItem *item);

/* Typed helper over the core: append a voxel face. worldQuad is 4 world-space
 * corners in canonical order; `face` selects the pinned atlas region
 * (top/side). */
bool appendVoxelFace(DrawList *list, const float worldQuad[4][3],
		     DrawFace face, uint32_t tint);

/* Sort back-to-front for the given camera (see the invariant block). A NULL
 * camera sorts against the identity view. NULL list is a no-op. */
void sortDrawList(DrawList *list, const Camera3D *camera);

#endif /* ISOMATA_RENDER_DRAWLIST_H */
