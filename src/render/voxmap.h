#ifndef ISOMATA_RENDER_VOXMAP_H
#define ISOMATA_RENDER_VOXMAP_H

/*
 * ASCII heightmap voxel map. Pure: file I/O only, no SDL.
 *
 * Format (one row per line, rows top-to-bottom):
 *   '0'..'9'  a column of that many unit blocks (height 0..9); '0' is a valid
 *             ground-level cell that still emits a top face at y = 0
 *   '.'       void: no column at all (a hole; emits nothing)
 *   anything else is a load error (diagnostic to stderr, loadVoxmap returns
 *             NULL)
 * Blank lines are ignored; a trailing CR (CRLF files) and trailing spaces or
 * tabs on a line are stripped. Every non-blank line must be the same length
 * (the map width). An empty map, an over-large map, or a ragged map is a load
 * error. VOXMAP_MAX_DIM bounds both dimensions so a bad file cannot force a
 * huge allocation.
 *
 * Two entry points share the parser: parseVoxmapText() takes an in-memory
 * buffer (used by the SDL tier for Android APK assets, which are not
 * filesystem files) and loadVoxmap() reads a file. The parser is
 * length-bounded and never assumes NUL termination.
 *
 * Height query semantics (pinned by test_voxmap):
 *   voxmapHeightAt returns the column height 0..9 for an in-bounds cell, and
 *   -1 for a void cell, an out-of-bounds cell, or a NULL map.
 *   voxmapIsVoid is exactly "height < 0" (void, out of bounds, or NULL).
 *
 * Face generation: for each non-void column, the top face at y = height plus
 * the exposed side faces (a side is exposed when the neighbour's height is
 * lower; a void or out-of-bounds neighbour counts as height 0). At an
 * axis-aligned camera yaw (0/90/180/270) only the one side facing the camera
 * is emitted; during a tween (any other yaw) all exposed sides are emitted.
 */

#include "render/camera3d.h"
#include "render/drawlist.h"

#include <stdbool.h>
#include <stdint.h>

#define VOXMAP_MAX_DIM 256

typedef struct Voxmap Voxmap;

/* Load a map from a file. Returns NULL with a clear stderr diagnostic on a
 * missing file, a malformed cell, an empty map, or a ragged/over-large map. */
Voxmap *loadVoxmap(const char *path);

/* Parse a map from `length` bytes of in-memory text. The buffer need NOT be
 * NUL-terminated and is never read past text + length, so a slice of a larger
 * buffer is safe. Same format/validation and NULL-on-error contract as
 * loadVoxmap (diagnostics are labelled "<memory>"). */
Voxmap *parseVoxmapText(const char *text, size_t length);

/* Release a map. NULL is a no-op. */
void destroyVoxmap(Voxmap *map);

/* Grid dimensions (0 for a NULL map). */
int voxmapWidth(const Voxmap *map);
int voxmapDepth(const Voxmap *map);

/* Column height at (x, y), or -1 for void / out of bounds / NULL (see the
 * header note). */
int voxmapHeightAt(const Voxmap *map, int x, int y);

/* True when (x, y) is void, out of bounds, or the map is NULL. */
bool voxmapIsVoid(const Voxmap *map, int x, int y);

/* Append every visible face of the map to `list` with the given tint. Emits
 * nothing for a NULL map/list. */
void voxmapEmitFaces(const Voxmap *map, DrawList *list, const Camera3D *camera,
		     uint32_t tint);

#endif /* ISOMATA_RENDER_VOXMAP_H */
