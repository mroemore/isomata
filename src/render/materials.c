/*
 * Material model + manifest parser (see materials.h). Pure: no SDL.
 */

#include "render/materials.h"

#include <string.h>

#define LINE_MAX 256

static bool copyBounded(char *dst, size_t cap, const char *src, size_t len)
{
	if (len >= cap)
		return false;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return true;
}

/* Split `line` (modified in place) on spaces/tabs into up to `max` tokens.
 * Returns the token count. */
static int tokenize(char *line, char **tokens, int max)
{
	int n = 0;
	char *p = line;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (*p == '\0')
			break;
		if (n < max)
			tokens[n] = p;
		n++;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
	}
	return n;
}

static bool parseAlpha(const char *value, AlphaMode *out)
{
	if (strcmp(value, "opaque") == 0) {
		*out = ALPHA_OPAQUE;
		return true;
	}
	if (strcmp(value, "blend") == 0) {
		*out = ALPHA_BLEND;
		return true;
	}
	if (strcmp(value, "cutout") == 0) {
		*out = ALPHA_CUTOUT;
		return true;
	}
	return false;
}

/* Face key -> FaceId. Returns -1 for an unknown key. "side" is handled by the
 * caller (it expands to the four sides). */
static int faceKeyId(const char *key, size_t len)
{
	static const struct {
		const char *name;
		int id;
	} keys[] = {
		{ "top", FACE_TOP }, { "bottom", FACE_BOTTOM },
		{ "north", FACE_NORTH }, { "south", FACE_SOUTH },
		{ "east", FACE_EAST }, { "west", FACE_WEST },
	};
	size_t i;

	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (strlen(keys[i].name) == len &&
		    memcmp(keys[i].name, key, len) == 0)
			return keys[i].id;
	return -1;
}

/* Fill every unset face of `def` from its primary file (the default file when
 * present, else the first set face). Returns false when no face has a file. */
static bool fillFaces(MaterialDef *def)
{
	int primary = -1;
	int i;

	for (i = 0; i < 6; i++) {
		if (def->file[i][0] != '\0') {
			primary = i;
			break;
		}
	}
	if (primary < 0)
		return false;
	for (i = 0; i < 6; i++) {
		if (def->file[i][0] == '\0') {
			copyBounded(def->file[i], MATERIAL_PATH_MAX,
				    def->file[primary],
				    strlen(def->file[primary]));
		}
	}
	return true;
}

bool materialManifestParse(const char *text, size_t length,
			   MaterialManifest *out)
{
	size_t pos = 0;

	if (text == NULL || out == NULL)
		return false;
	out->count = 0;

	while (pos < length) {
		size_t start = pos;
		size_t end;
		char line[LINE_MAX];
		char *tokens[16];
		int ntok;
		MaterialDef def;
		char defaultFile[MATERIAL_PATH_MAX];
		int i;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;
		if (end > start && text[end - 1] == '\r')
			end--;
		if (!copyBounded(line, sizeof(line), text + start, end - start))
			return false;
		ntok = tokenize(line, tokens, 16);
		if (ntok == 0 || tokens[0][0] == '#')
			continue;
		if (ntok > 16 || strcmp(tokens[0], "material") != 0)
			return false;
		if (ntok < 3)
			return false;

		memset(&def, 0, sizeof(def));
		def.alpha = ALPHA_OPAQUE;
		defaultFile[0] = '\0';
		if (!copyBounded(def.name, MATERIAL_NAME_MAX, tokens[1],
				 strlen(tokens[1])))
			return false;

		for (i = 2; i < ntok; i++) {
			char *tok = tokens[i];
			char *eq = strchr(tok, '=');

			if (strcmp(tok, "face") == 0)
				continue;	/* optional keyword */
			if (eq == NULL) {
				/* Bare token: the default file for all faces. */
				if (defaultFile[0] != '\0' ||
				    !copyBounded(defaultFile, MATERIAL_PATH_MAX,
						 tok, strlen(tok)))
					return false;
			} else {
				const char *key = tok;
				size_t keyLen = (size_t)(eq - tok);
				const char *val = eq + 1;

				if (keyLen == 5 && memcmp(key, "alpha", 5) == 0) {
					if (!parseAlpha(val, &def.alpha))
						return false;
				} else if (keyLen == 4 &&
					   memcmp(key, "side", 4) == 0) {
					if (!copyBounded(def.file[FACE_NORTH],
							 MATERIAL_PATH_MAX, val,
							 strlen(val)) ||
					    !copyBounded(def.file[FACE_SOUTH],
							 MATERIAL_PATH_MAX, val,
							 strlen(val)) ||
					    !copyBounded(def.file[FACE_EAST],
							 MATERIAL_PATH_MAX, val,
							 strlen(val)) ||
					    !copyBounded(def.file[FACE_WEST],
							 MATERIAL_PATH_MAX, val,
							 strlen(val)))
						return false;
				} else {
					int id = faceKeyId(key, keyLen);

					if (id < 0 ||
					    !copyBounded(def.file[id],
							 MATERIAL_PATH_MAX, val,
							 strlen(val)))
						return false;
				}
			}
		}
		if (defaultFile[0] != '\0') {
			/* One file fills all six faces; overrides already set
			 * individual faces and win, so only empty faces take
			 * the default. */
			for (i = 0; i < 6; i++)
				if (def.file[i][0] == '\0')
					copyBounded(def.file[i],
						    MATERIAL_PATH_MAX,
						    defaultFile,
						    strlen(defaultFile));
		}
		if (!fillFaces(&def))
			return false;
		if (out->count >= MATERIAL_MAX)
			return false;
		out->defs[out->count++] = def;
	}
	return true;
}

int materialTableAdd(MaterialTable *table, const char *name, AlphaMode alpha)
{
	Material *m;
	int id;

	if (table == NULL || name == NULL || table->count >= MATERIAL_MAX)
		return -1;
	id = (int)table->count;
	m = &table->items[id];
	memset(m, 0, sizeof(*m));
	copyBounded(m->name, MATERIAL_NAME_MAX, name, strlen(name));
	m->alpha = alpha;
	table->count++;
	return id;
}

bool materialTableSetRect(MaterialTable *table, int id, FaceId face,
			  AtlasRect rect)
{
	if (table == NULL || id < 0 || (size_t)id >= table->count)
		return false;
	if ((int)face < 0 || face > FACE_WEST)
		return false;
	table->items[id].rect[face] = rect;
	return true;
}

int materialIdByName(const MaterialTable *table, const char *name)
{
	size_t i;

	if (table == NULL || name == NULL)
		return -1;
	for (i = 0; i < table->count; i++)
		if (strcmp(table->items[i].name, name) == 0)
			return (int)i;
	return -1;
}

int materialFileSlot(char (*fileNames)[MATERIAL_PATH_MAX], int fileCount,
		     const char *name)
{
	int k;

	if (fileNames == NULL || name == NULL)
		return -1;
	for (k = 0; k < fileCount; k++)
		if (strcmp(fileNames[k], name) == 0)
			return k;
	return -1;
}

int materialTableBuild(MaterialTable *table, const MaterialManifest *manifest,
		       char (*fileNames)[MATERIAL_PATH_MAX], int fileCount,
		       const AtlasLayout *layout, const int *fileSizes)
{
	size_t i;

	if (table == NULL || manifest == NULL || fileNames == NULL ||
	    layout == NULL || fileSizes == NULL)
		return -1;
	table->count = 0;
	for (i = 0; i < manifest->count; i++) {
		const MaterialDef *def = &manifest->defs[i];
		int id = materialTableAdd(table, def->name, def->alpha);
		int f;

		if (id < 0)
			return -1;
		for (f = 0; f < 6; f++) {
			int slot = materialFileSlot(fileNames, fileCount,
						    def->file[f]);
			AtlasRect r;

			if (slot < 0)
				continue;
			if (atlasSlotRect(layout, slot, fileSizes[slot], &r))
				materialTableSetRect(table, id, (FaceId)f, r);
		}
	}
	return (int)table->count;
}

FaceId materialFaceForSideDir(int dir)
{
	switch (dir) {
	case 1:
		return FACE_EAST;
	case 2:
		return FACE_NORTH;
	case 3:
		return FACE_WEST;
	default:
		return FACE_SOUTH;	/* dir 0 = +Z */
	}
}

void materialFaceUV(const AtlasRect *rect, FaceId face, float uv[4][2])
{
	if (rect == NULL || uv == NULL)
		return;
	switch (face) {
	case FACE_EAST:
	case FACE_WEST:
		/* Quad BL, BR, TR, TL. Upright (texture top at the top
		 * corners) and horizontally mirrored so the east/west faces are
		 * not reversed when viewed from outside the cube. */
		uv[0][0] = rect->u1;	uv[0][1] = rect->v1;
		uv[1][0] = rect->u0;	uv[1][1] = rect->v1;
		uv[2][0] = rect->u0;	uv[2][1] = rect->v0;
		uv[3][0] = rect->u1;	uv[3][1] = rect->v0;
		break;
	case FACE_NORTH:
	case FACE_SOUTH:
		/* Quad BL, BR, TR, TL. Upright: the bottom corners sample the
		 * texture bottom (v1), the top corners the texture top (v0). The
		 * -Z emitter already reverses x in its corner order, so north and
		 * south share this order. */
		uv[0][0] = rect->u0;	uv[0][1] = rect->v1;
		uv[1][0] = rect->u1;	uv[1][1] = rect->v1;
		uv[2][0] = rect->u1;	uv[2][1] = rect->v0;
		uv[3][0] = rect->u0;	uv[3][1] = rect->v0;
		break;
	case FACE_BOTTOM:
		/* Underside: mirrored vertically against the top. */
		uv[0][0] = rect->u0;	uv[0][1] = rect->v1;
		uv[1][0] = rect->u1;	uv[1][1] = rect->v1;
		uv[2][0] = rect->u1;	uv[2][1] = rect->v0;
		uv[3][0] = rect->u0;	uv[3][1] = rect->v0;
		break;
	case FACE_TOP:
	default:
		/* Quad (x,z), (x+1,z), (x+1,z+1), (x,z+1): texture top at the
		 * far (-Z) edge, texture left at the west edge. */
		uv[0][0] = rect->u0;	uv[0][1] = rect->v0;
		uv[1][0] = rect->u1;	uv[1][1] = rect->v0;
		uv[2][0] = rect->u1;	uv[2][1] = rect->v1;
		uv[3][0] = rect->u0;	uv[3][1] = rect->v1;
		break;
	}
}
