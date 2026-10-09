#ifndef ISOMATA_RENDER_VOXMAP_H
#define ISOMATA_RENDER_VOXMAP_H

/*
 * ASCII heightmap voxel map with a per-cell material. Pure: file I/O only,
 * no SDL.
 *
 * Format (one row per line, rows top-to-bottom):
 *   '0'..'9'  a column of that many unit blocks (height 0..9); '0' is a solid
 *             ground-level cell that still emits a top face at y = 0
 *   '.'       void: no column at all (a hole; emits nothing)
 *   a char declared by a legend line (below) with height 0..9
 *   anything else is a load error (diagnostic to stderr, load returns NULL)
 * Blank lines are ignored; a trailing CR (CRLF files) and trailing spaces or
 * tabs on a line are stripped. Every non-blank, non-legend, non-light line must
 * be the same length (the map width). An empty map, an over-large map, or a
 * ragged map is a load error. VOXMAP_MAX_DIM bounds both dimensions so a bad
 * file cannot force a huge allocation.
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
 * Height query semantics (pinned by test_voxmap):
 *   voxmapHeightAt returns the column height 0..9 for an in-bounds cell, and
 *   -1 for a void cell, an out-of-bounds cell, or a NULL map.
 *   voxmapIsVoid is exactly "height < 0" (void, out of bounds, or NULL).
 *   voxmapMaterialAt returns the material id for an in-bounds non-void cell,
 *   and -1 for void / out of bounds / NULL.
 *
 * Face generation: for each non-void column, the top face at y = height plus
 * the exposed side faces (a side is exposed when the neighbour's height is
 * lower; a void or out-of-bounds neighbour counts as height 0). A side with
 * outward normal n is CULLED when dot(n, toCameraGround) <= CAMERA_CULL_EPS,
 * where toCameraGround is the unit ground-plane direction from the target
 * toward the camera (the negative of the camera's ground forward). The test
 * is continuous: at an axis-aligned yaw exactly one side emits (the two
 * edge-on sides have dot 0 and are culled), at 45 degrees two sides emit,
 * and a tween moves smoothly between them. Top faces always emit.
 *
 * Each face samples its cell material's UV rect for that face
 * (materials.h): the top face the top slot, each side direction its own
 * slot. With a NULL material table the built-in fallback regions
 * (textures.h) are used, so the emitters stay usable without a manifest.
 *
 * LIGHTING: each face multiplies a per-channel brightness factor into its
 * tint. Two paths share one composition (material tint x directional shade x
 * checkerboard x light factor x AO):
 *   - FLAT (VoxmapEmitOptions.smooth false) is exactly the T14 path: one
 *     factor per face, no AO, byte-identical tints. The factor comes from the
 *     air cell the face looks across (lightgrid.h::lightGridFactorAt):
 *       . a top face samples the cell directly above the column top,
 *         (x, height, z);
 *       . a side face samples the air cell immediately above the neighbour
 *         column, (x + dx, neighbourHeight, z + dz) — the cell the face's lower
 *         edge looks across (a void/OOB neighbour uses y = 0).
 *   - SMOOTH (the default, T15) samples a per-corner average and a per-corner
 *     AO multiplier, written as the DrawItem's 4 corner tints so the GPU
 *     interpolates a gradient across the face. Top faces average the 2x2 block
 *     of columns above each corner at the face's air level; side faces keep the
 *     SINGLE multi-level span (splitting it would reintroduce the T12 base-line
 *     sort bug) and sample the corner's own air cell plus its in-plane
 *     neighbours at the bottom level for the two bottom corners and the top
 *     level for the two top corners — the GPU then interpolates the vertical
 *     gradient across the quad.
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
 * noticeably: +Z bright, +X mid, -Z dark, -X darker.
 */
#define VOXMAP_SHADE_TOP 1.00f
#define VOXMAP_SHADE_SIDE_PZ 0.90f	/* dir 0: +Z */
#define VOXMAP_SHADE_SIDE_PX 0.80f	/* dir 1: +X */
#define VOXMAP_SHADE_SIDE_NZ 0.70f	/* dir 2: -Z */
#define VOXMAP_SHADE_SIDE_NX 0.62f	/* dir 3: -X */

/* Checkerboard: odd tiles ((x + z) & 1) are brightened by this factor on top
 * of the face shade, for both tops and sides of that column. */
#define VOXMAP_CHECKER_BOOST 1.06f

typedef struct Voxmap Voxmap;
#ifndef ISOMATA_LIGHTGRID_TYPEDEF
#define ISOMATA_LIGHTGRID_TYPEDEF
typedef struct LightGrid LightGrid;
#endif

/* Load a map from a file, resolving legend materials through `materials`
 * (NULL allowed). Returns NULL with a clear stderr diagnostic on a missing
 * file, a malformed cell/legend, an empty map, or a ragged/over-large map. */
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

/* Column height at (x, y), or -1 for void / out of bounds / NULL (see the
 * header note). */
int voxmapHeightAt(const Voxmap *map, int x, int y);

/* True when (x, y) is void, out of bounds, or the map is NULL. */
bool voxmapIsVoid(const Voxmap *map, int x, int y);

/* Material id at (x, y), or -1 for void / out of bounds / NULL. */
int voxmapMaterialAt(const Voxmap *map, int x, int y);

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
