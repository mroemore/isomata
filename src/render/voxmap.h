#ifndef ISOMATA_RENDER_VOXMAP_H
#define ISOMATA_RENDER_VOXMAP_H

/*
 * ASCII voxel map: an occupancy + per-voxel material grid. Pure: file I/O
 * only, no SDL.
 *
 * MODEL. The map is a w (x) * levels (y) * d (z) grid. A cell is either air
 * or a solid voxel with a material id. Level y is the voxel occupying world
 * [y, y + 1), so a voxel's top surface is at world y + 1 and its bottom at
 * world y. This is the standard bottom-surface convention: two stacked solid
 * levels are a two-unit-tall column.
 *
 * SINGLE-SECTION (heightmap) FORMAT (one row per line, rows top-to-bottom):
 *   '0'..'9'  a column of that many unit blocks (height 0..9). A height-h
 *             column is solid voxels y 0..h-1. A height-0 cell is the one
 *             degenerate case: it has no voxel, but it is a SOLID flat ground
 *             tile that still emits a top face at y = 0 (a void column, '.',
 *             emits nothing).
 *   '.'       void: no column at all (a hole; emits nothing)
 *   a char declared by a legend line (below) with height 0..9
 *   anything else is a load error (diagnostic to stderr, load returns NULL)
 * Blank lines are ignored; a trailing CR (CRLF files) and trailing spaces or
 * tabs on a line are stripped. Every non-blank, non-legend, non-light,
 * non-separator line must be the same length (the map width). An empty map, an
 * over-large map, or a ragged map is a load error. VOXMAP_MAX_DIM bounds the
 * width, depth and level count; VOXMAP_MAX_CELLS bounds w*d*levels so a bad
 * file cannot force a huge allocation.
 *
 * MULTI-SECTION (slice) FORMAT. A file containing at least one separator line
 * is a 3D map. A separator is a line whose trimmed content is exactly `---`.
 * Each block of rows between separators is one horizontal slice; slice index s
 * is level y = s (ascending: the FIRST section is y 0, the bottom). All
 * sections must have the same width and depth, and a section may not be empty
 * (a leading, trailing or doubled separator is a load error). In slice mode a
 * cell char is:
 *   - a legend char whose height is 0..9 -> a SOLID voxel at that level with
 *     that char's material (the legend HEIGHT is ignored; material only);
 *   - '.' or a legend char whose height is -1 (the built-in void char), or a
 *     space -> AIR;
 *   - anything else -> a load error.
 * In slice mode only a trailing CR is stripped (a trailing space is a real air
 * cell), so rows keep their width; use '.' for air at a line end. Blank
 * (empty) lines are still ignored everywhere — but note that a line of ONLY
 * spaces is not blank: it is a full row of air cells (and participates in the
 * width check).
 *
 * Legend lines (anywhere in the file; skipped when counting map rows):
 *   @ <char> <height> <material>
 * map a single character to a height (0..9; 0 = a solid ground-level cell) and
 * a material name. The '@' may be attached to the char (`@g 2 grass`). A legend
 * char overrides the built-in default for that char. Digits 0..9 default to
 * height = digit with the "default" material; '.' defaults to void. A legend
 * naming a material absent from the table logs a diagnostic and falls back to
 * the "default" material. A legend line whose char is not a single ASCII byte
 * (< 128) is skipped with a diagnostic (it cannot index the legend table).
 *
 * Light lines (anywhere in the file; also skipped when counting map rows):
 *   $ point x y z r g b [radius]
 *   $ spot x y z r g b dx dy dz angle [radius]
 * (x, y, z) is a WORLD position in the same space lightGridSeedPoint /
 * lightGridSeedSpot consume: the seeded cell is (floor(x), floor(y), floor(z)),
 * so a lamp at the centre of cell (cx, cy, cz) is written (cx+0.5, cy+0.5,
 * cz+0.5). r/g/b are 0..255 (clamped). `radius` is a world-unit range: for a
 * point it caps the seeded value (min(channel, radius * LIGHT_ATTEN)); for a
 * spot it is the cone length; omitted, a point defaults to 0 (full channel,
 * global attenuation only) and a spot to VOXMAP_LIGHT_DEFAULT_RADIUS. The spot
 * direction need not be unit length; it is normalised here, and a zero-length
 * direction is malformed. A malformed light line is SKIPPED with a stderr
 * diagnostic (it never fails the load and never crashes): wrong token count, a
 * non-numeric token, a non-finite value, a zero direction, or a non-positive
 * angle. At most VOXMAP_MAX_LIGHTS lines are kept; extras are skipped.
 *
 * Two entry points share the parser: parseVoxmapText() takes an in-memory
 * buffer (used by the SDL tier for Android APK assets, which are not
 * filesystem files) and loadVoxmap() reads a file. Both take the material
 * table used to resolve legend names (NULL is allowed: every material
 * resolves to id 0). The parser is length-bounded and never assumes NUL
 * termination.
 *
 * QUERY SEMANTICS (pinned by test_voxmap):
 *   voxmapHeightAt returns the world height of the topmost solid voxel's top
 *   surface (topmost solid level + 1), 0 for a height-0 ground tile, and -1
 *   for a void cell, an out-of-bounds cell, or a NULL map.
 *   voxmapIsVoid is exactly "height < 0" (void, out of bounds, or NULL).
 *   voxmapMaterialAt returns the material id of the topmost solid voxel (or of
 *   the height-0 ground tile), and -1 for void / out of bounds / NULL.
 *   voxmapLevels returns the y dimension (>= 1), 0 for a NULL map.
 *   voxmapSolidAt returns whether the voxel at (x, y, z) is solid; false for
 *   air / out of bounds / NULL.
 *   voxmapMaterialAtVoxel returns the voxel's material id, -1 for air / out of
 *   bounds / NULL.
 * Note the historical (x, y) argument names of voxmapHeightAt / voxmapIsVoid /
 * voxmapMaterialAt: their second argument is the Z column coordinate. The new
 * 3D queries take (x, y, z) in world order.
 *
 * FACE GENERATION. Emission culls against the six neighbours:
 *   - TOP: a solid voxel whose above-neighbour is air emits a 1x1 top face at
 *     world y + 1. A height-0 ground tile emits its top at y = 0.
 *   - BOTTOM: a solid voxel at level y >= 1 whose below-neighbour is air emits
 *     a bottom face at world y. (Level 0's underside is the world floor, so no
 *     bottom is emitted there.) This is the new 3D face.
 *   - SIDES: per (column, direction), the vertical runs where the voxel is
 *     solid AND the neighbour voxel at that level is air emit ONE quad per
 *     run, spanning world [runStart, runEnd + 1). A void / out-of-bounds
 *     neighbour is air at every level. A side with outward normal n is CULLED
 *     when dot(n, toCameraGround) <= CAMERA_CULL_EPS, where toCameraGround is
 *     the unit ground-plane direction from the target toward the camera (the
 *     negative of the camera's ground forward). The test is continuous: at an
 *     axis-aligned yaw exactly one side emits, at 45 degrees two sides emit,
 *     and a tween moves smoothly between them.
 * For a single-section heightmap the run logic degenerates to the old
 * single-run-per-side behaviour, so emission is byte-identical to the
 * heightmap engine (the strongest backward-compat pin; pinned by
 * test_voxmap).
 *
 * Each face samples its voxel material's UV rect for that face (materials.h):
 * the top face the top slot, the bottom face the bottom slot, each side
 * direction its own slot. With a NULL material table the built-in fallback
 * regions (textures.h) are used, so the emitters stay usable without a
 * manifest.
 *
 * LIGHTING: each face multiplies a per-channel brightness factor into its
 * tint. Two paths share one composition (material tint x directional shade x
 * checkerboard x light factor x AO):
 *   - FLAT (VoxmapEmitOptions.smooth false) is exactly the T14 path: one
 *     factor per face, no AO, byte-identical tints. The factor comes from the
 *     air cell the face looks across (lightgrid.h::lightGridFactorAt):
 *       . a top face samples the cell directly above the voxel top,
 *         (x, y + 1, z);
 *       . a bottom face samples the cell directly below the voxel,
 *         (x, y - 1, z);
 *       . a side face samples the air cell immediately across the neighbour
 *         voxel at the run's bottom level, (x + dx, runStart, z + dz).
 *   - SMOOTH (the default, T15) samples a per-corner average and a per-corner
 *     AO multiplier, written as the DrawItem's 4 corner tints so the GPU
 *     interpolates a gradient across the face. Top and bottom faces average
 *     the 2x2 block of columns at the face's air level; side faces keep the
 *     SINGLE multi-level span (splitting it would reintroduce the T12 base-line
 *     sort bug) and sample the corner's own air cell plus its in-plane
 *     neighbours at the bottom level for the two bottom corners and the top
 *     level for the two top corners.
 * A NULL `lights` keeps every face at full brightness (factor 255) on both
 * paths, so the unlit emission path is unchanged.
 *
 * DEBUG (VoxmapEmitOptions.lightDebug) renders the light only: every face uses
 * a fully-white atlas UV (`debugUV`; NULL falls back to the built-in spare
 * region) and the light factor itself as the corner tint — no material tint,
 * shade, checkerboard or AO. Corner-interpolated on the smooth path.
 */

#include "render/camera3d.h"
#include "render/drawlist.h"
#include "render/materials.h"

#include <stdbool.h>
#include <stdint.h>

#define VOXMAP_MAX_DIM 256
/* Upper bound on w * d * levels: a bad multi-section file must fail cleanly
 * rather than force a huge allocation. A 256x256 heightmap (10 levels) is
 * ~655k cells, comfortably under this. */
#define VOXMAP_MAX_CELLS (1u << 20)
/* A side at dot <= this faces away from the camera. Non-zero so float noise
 * at an exactly edge-on (dot 0) side still culls it. */
#define CAMERA_CULL_EPS 1e-4f

/* At most this many `$` light lines are kept per map. */
#define VOXMAP_MAX_LIGHTS 64
/* Cone length for a spot line whose optional radius is omitted. */
#define VOXMAP_LIGHT_DEFAULT_RADIUS 16.0f

#define VOXMAP_LIGHT_POINT 0
#define VOXMAP_LIGHT_SPOT 1

/* One parsed `$` light. Coordinates are world space (the seeded cell is the
 * floor); r/g/b are 0..255; dir is unit length for a spot. */
typedef struct VoxmapLight {
	int kind;		/* VOXMAP_LIGHT_POINT / VOXMAP_LIGHT_SPOT */
	float x;
	float y;
	float z;
	float r;
	float g;
	float b;
	float radius;		/* <= 0 = full channel (point) */
	float dir[3];		/* spot axis, unit length */
	float halfAngleDeg;	/* spot half-angle, > 0 */
} VoxmapLight;

/*
 * Per-face brightness multiplied onto the caller tint (RGB only; alpha is
 * preserved). The top is the brightest (1.0); the four side constants are
 * distinct and ordered so the two sides visible at a 45-degree yaw differ
 * noticeably: +Z bright, +X mid, -Z dark, -X darker. The bottom is the
 * darkest face of all (an underside reads as a dim, occluded plane).
 */
#define VOXMAP_SHADE_TOP 1.00f
#define VOXMAP_SHADE_SIDE_PZ 0.90f	/* dir 0: +Z */
#define VOXMAP_SHADE_SIDE_PX 0.80f	/* dir 1: +X */
#define VOXMAP_SHADE_SIDE_NZ 0.70f	/* dir 2: -Z */
#define VOXMAP_SHADE_SIDE_NX 0.62f	/* dir 3: -X */
#define VOXMAP_SHADE_BOTTOM 0.55f	/* -Y underside: the darkest face */

/* Checkerboard: odd tiles ((x + z) & 1) are brightened by this factor on top
 * of the face shade, for tops, bottoms and sides of that column. */
#define VOXMAP_CHECKER_BOOST 1.06f

typedef struct Voxmap Voxmap;
#ifndef ISOMATA_LIGHTGRID_TYPEDEF
#define ISOMATA_LIGHTGRID_TYPEDEF
typedef struct LightGrid LightGrid;
#endif

/* Load a map from a file, resolving legend materials through `materials`
 * (NULL allowed). Returns NULL with a clear stderr diagnostic on a missing
 * file, a malformed cell/legend/section, an empty map, or a ragged/over-large
 * map. */
Voxmap *loadVoxmap(const char *path, const MaterialTable *materials);

/* Parse a map from `length` bytes of in-memory text. The buffer need NOT be
 * NUL-terminated and is never read past text + length, so a slice of a larger
 * buffer is safe. Same format/validation and NULL-on-error contract as
 * loadVoxmap (diagnostics are labelled "<memory>"). */
Voxmap *parseVoxmapText(const char *text, size_t length,
			const MaterialTable *materials);

/* Release a map. NULL is a no-op. */
void destroyVoxmap(Voxmap *map);

/* Grid dimensions (0 for a NULL map). */
int voxmapWidth(const Voxmap *map);
int voxmapDepth(const Voxmap *map);
/* Y dimension (levels); >= 1 for a real map, 0 for a NULL map. */
int voxmapLevels(const Voxmap *map);

/* Column height at (x, z) — the world height of the topmost solid voxel's top
 * surface (topmost solid level + 1), 0 for a height-0 ground tile, or -1 for
 * void / out of bounds / NULL (see the header note). */
int voxmapHeightAt(const Voxmap *map, int x, int z);

/* True when (x, z) is void, out of bounds, or the map is NULL. */
bool voxmapIsVoid(const Voxmap *map, int x, int z);

/* Material id at (x, z) — the topmost solid voxel's material (or the height-0
 * ground tile's), or -1 for void / out of bounds / NULL. */
int voxmapMaterialAt(const Voxmap *map, int x, int z);

/* True when the voxel at world (x, y, z) is solid; false for air / out of
 * bounds / NULL. */
bool voxmapSolidAt(const Voxmap *map, int x, int y, int z);

/* Material id of the voxel at world (x, y, z); -1 for air / out of bounds /
 * NULL. */
int voxmapMaterialAtVoxel(const Voxmap *map, int x, int y, int z);

/* Parsed `$` light count (0 for a NULL map). */
int voxmapLightCount(const Voxmap *map);

/* Borrowed light at `index`, or NULL when out of range / the map is NULL. */
const VoxmapLight *voxmapLightAt(const Voxmap *map, int index);

/* Face-emission options (see the lighting/debug note). `tint` is the base
 * material tint; `smooth` selects the per-corner light + AO pass (false is
 * exactly the T14 flat path); `lightDebug` is the light-only view;
 * `debugUV` is the 4-corner UV of a white atlas texel used in that view
 * (NULL uses the built-in spare region). */
typedef struct VoxmapEmitOptions {
	uint32_t tint;
	bool smooth;
	bool lightDebug;
	const float (*debugUV)[2];
} VoxmapEmitOptions;

/* Append every visible face of the map to `list` with the given tint, sampling
 * `materials` (NULL uses the built-in fallback regions) and `lights` (NULL
 * emits full-brightness faces; see the lighting note). Emits nothing for a
 * NULL map/list. This is the flat T14 path; use voxmapEmitFacesOpt for smooth
 * lighting or the debug view. */
void voxmapEmitFaces(const Voxmap *map, const MaterialTable *materials,
		     const LightGrid *lights, DrawList *list,
		     const Camera3D *camera, uint32_t tint);

/* As voxmapEmitFaces, but with the full options (smooth lighting, debug view).
 * A NULL `options` emits nothing. */
void voxmapEmitFacesOpt(const Voxmap *map, const MaterialTable *materials,
			const LightGrid *lights, DrawList *list,
			const Camera3D *camera,
			const VoxmapEmitOptions *options);

#endif /* ISOMATA_RENDER_VOXMAP_H */
