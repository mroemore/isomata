#ifndef ISOMATA_RENDER_MATERIALS_H
#define ISOMATA_RENDER_MATERIALS_H

/*
 * Material model and texture manifest (pure). No SDL.
 *
 * A material names a texture for each of the six cube faces (outside only,
 * no inside set yet) and an alpha mode. The common case is ONE file filling
 * all six faces; individual faces can be overridden by name. A runtime atlas
 * packs the referenced files; each material face stores its atlas UV rect
 * (see atlas.h), and materialFaceUV() turns a rect into the four-corner UV
 * quad an emitter needs, oriented so the texture reads upright and
 * unmirrored from outside the cube.
 *
 * Face axes: top = +Y, bottom = -Y, north = -Z, south = +Z, east = +X,
 * west = -X. The voxmap's four side directions (0=+Z, 1=+X, 2=-Z, 3=-X) map
 * through materialFaceForSideDir().
 *
 * Manifest grammar (one directive per line; '#' comments and blank lines
 * ignored):
 *   material <name> <file> [alpha=opaque|blend|cutout]
 *   material <name> face <key>=<file> ... [alpha=...]
 * <key> is one of top, bottom, north, south, east, west, or side (which fills
 * all four sides). The bare `face` keyword is optional. A bare <file> (no
 * '=') is the default for every face. Any face left unset after the default
 * and overrides inherits the material's primary file (the default file, else
 * the first override given). A material with no file at all is an error.
 *
 * Alpha modes: opaque and blend both composite through the pipeline's fixed
 * alpha blend (opaque textures carry a=255, so the blend is identity);
 * cutout discards fragments whose texture alpha is below a threshold in the
 * fragment shader (see shaders/world.frag.glsl).
 */

#include "render/atlas.h"

#include <stdbool.h>
#include <stddef.h>

#define MATERIAL_MAX 64
#define MATERIAL_NAME_MAX 32
#define MATERIAL_PATH_MAX 128

typedef enum AlphaMode {
	ALPHA_OPAQUE = 0,
	ALPHA_BLEND = 1,
	ALPHA_CUTOUT = 2,
} AlphaMode;

typedef enum FaceId {
	FACE_TOP = 0,
	FACE_BOTTOM = 1,
	FACE_NORTH = 2,	/* -Z */
	FACE_SOUTH = 3,	/* +Z */
	FACE_EAST = 4,	/* +X */
	FACE_WEST = 5,	/* -X */
} FaceId;

typedef struct Material {
	char name[MATERIAL_NAME_MAX];
	AlphaMode alpha;
	AtlasRect rect[6];	/* per FaceId */
} Material;

typedef struct MaterialTable {
	Material items[MATERIAL_MAX];
	size_t count;
} MaterialTable;

typedef struct MaterialDef {
	char name[MATERIAL_NAME_MAX];
	char file[6][MATERIAL_PATH_MAX];	/* per FaceId; always set */
	AlphaMode alpha;
} MaterialDef;

typedef struct MaterialManifest {
	MaterialDef defs[MATERIAL_MAX];
	size_t count;
} MaterialManifest;

/* Parse manifest text. Returns false (out untouched) on a NULL out, a
 * malformed directive, an unknown face key, an unknown alpha mode, an
 * over-long name/path, or a material with no file. `length` need not be NUL
 * terminated. */
bool materialManifestParse(const char *text, size_t length,
			   MaterialManifest *out);

/* Append a material (name + alpha) to the table. Returns its id, or -1 when
 * the table is full or the arguments are NULL. The name is copied and
 * truncated to MATERIAL_NAME_MAX-1. */
int materialTableAdd(MaterialTable *table, const char *name, AlphaMode alpha);

/* Set one face's atlas rect for `id`. False on a NULL table / out-of-range
 * id. */
bool materialTableSetRect(MaterialTable *table, int id, FaceId face,
			  AtlasRect rect);

/* Look up a material id by exact name; -1 when absent or on NULL args. */
int materialIdByName(const MaterialTable *table, const char *name);

/* FaceId for a voxmap side direction (0=+Z, 1=+X, 2=-Z, 3=-X). */
FaceId materialFaceForSideDir(int dir);

/* Build the four-corner UV quad (canonical quad corner order) for `rect` on
 * `face`. `uv` receives 4 (u, v) pairs in the SAME corner order the emitter's
 * world quad uses for that face, oriented so the texture is upright and not
 * mirrored when viewed from outside the cube:
 *   - top/bottom quads run (x,z), (x+1,z), (x+1,z+1), (x,z+1);
 *   - side quads run bottom-left, bottom-right, top-right, top-left;
 *   - sprites use the side order (they are billboards, front face = south).
 * NULL uv is a no-op. */
void materialFaceUV(const AtlasRect *rect, FaceId face, float uv[4][2]);

#endif /* ISOMATA_RENDER_MATERIALS_H */
