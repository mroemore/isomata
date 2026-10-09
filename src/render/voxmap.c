/*
 * Voxmap (see voxmap.h for the model, format and query contract). Pure: file
 * I/O and a small malloc, no SDL. Diagnostics go to stderr (a pure module
 * cannot use SDL_Log).
 *
 * Storage is an occupancy + per-voxel material grid: solid[] is one byte per
 * voxel, materials[] one int16 per voxel and shapes[] one packed byte per
 * voxel (shape in bits 0-1, ramp dir in bits 2-3; index order x fastest, then
 * y, then z: index = ((z * levels) + y) * width + x, matching lightgrid.c). A
 * parallel per-column ground[] byte marks the single-section height-0
 * degenerate (a solid flat ground tile with no voxel). Single-section files
 * assign the column's material and shape to every voxel; slice files assign
 * per voxel.
 *
 * Parsing: a legend pass collects `@` lines, a light pass collects `$` lines
 * and a separator scan flags multi-section files. A file with no `---`
 * separator is a heightmap (the original format); one with a separator is a
 * stack of slices. Both never assume NUL termination — every scan is bounded
 * by the caller's length.
 */

#include "render/voxmap.h"
#include "render/lightgrid.h"
#include "render/textures.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

/* Char -> legend table size (ASCII). */
#define LEGEND_CHARS 128
/* Longest legend line we accept (short by construction). */
#define LEGEND_LINE_MAX 256
/* -2 = char not a valid cell, -1 = void, 0..9 = column height. */
#define LEGEND_INVALID (-2)

struct Voxmap {
	int width;
	int depth;
	int levels;		/* y dimension; >= 1 */
	uint8_t *solid;		/* width * depth * levels; 1 = solid voxel */
	int16_t *materials;	/* width * depth * levels; -1 = air */
	uint8_t *shapes;	/* width * depth * levels; packed shape|dir<<2 */
	uint8_t *ground;	/* width * depth; 1 = height-0 ground tile */
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount;
};

/* x fastest, then y, then z. */
static size_t voxelIndex(const Voxmap *map, int x, int y, int z)
{
	return ((size_t)z * (size_t)map->levels + (size_t)y) *
	       (size_t)map->width + (size_t)x;
}

/* Read the whole file into a NUL-terminated malloc buffer, or NULL. */
static char *readWholeFile(const char *path, size_t *outSize)
{
	FILE *file;
	long length;
	char *data;

	*outSize = 0;
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	data = malloc((size_t)length + 1);
	if (data == NULL) {
		fclose(file);
		return NULL;
	}
	if (length > 0 && fread(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	data[length] = '\0';
	*outSize = (size_t)length;
	return data;
}

/* Length of the line starting at data[start], excluding a trailing '\r' and
 * trailing spaces/tabs. `end` is the first '\n' (or the buffer end). */
static size_t trimmedLength(const char *data, size_t start, size_t end)
{
	while (end > start && (data[end - 1] == '\r' || data[end - 1] == ' ' ||
			       data[end - 1] == '\t'))
		end--;
	return end - start;
}

/* Slice-mode row length: strip only a trailing '\r'. A trailing space is a
 * real air cell in slice mode, so it must not be trimmed. */
static size_t sliceRowLength(const char *data, size_t start, size_t end)
{
	while (end > start && data[end - 1] == '\r')
		end--;
	return end - start;
}

/* True when the line's first non-space/tab character is '@'. */
static bool isLegendLine(const char *data, size_t start, size_t len)
{
	size_t i = start;
	size_t end = start + len;

	while (i < end && (data[i] == ' ' || data[i] == '\t'))
		i++;
	return i < end && data[i] == '@';
}

/* True when the line's first non-space/tab character is '$'. */
static bool isLightLine(const char *data, size_t start, size_t len)
{
	size_t i = start;
	size_t end = start + len;

	while (i < end && (data[i] == ' ' || data[i] == '\t'))
		i++;
	return i < end && data[i] == '$';
}

/* True for any directive line (legend or light): such a line is never a map
 * row and is skipped when pinning width / counting depth. */
static bool isDirectiveLine(const char *data, size_t start, size_t len)
{
	return isLegendLine(data, start, len) || isLightLine(data, start, len);
}

/* True when the line's trimmed content is exactly "---" (a section
 * separator). Surrounding spaces/tabs/\r are ignored. */
static bool isSeparatorLine(const char *data, size_t start, size_t len)
{
	size_t i = start;
	size_t end = start + len;

	while (i < end && (data[i] == ' ' || data[i] == '\t' ||
			   data[i] == '\r'))
		i++;
	while (end > i && (data[end - 1] == ' ' || data[end - 1] == '\t' ||
			   data[end - 1] == '\r'))
		end--;
	return end - i == 3 && data[i] == '-' && data[i + 1] == '-' &&
	       data[i + 2] == '-';
}

/* Split a bounded line copy on spaces/tabs. Returns the token count. */
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

/* The built-in legend: digits 0..9 are solid heights (0 = a ground-level
 * cell), '.' is void; legend chars can override any of them. */
static void legendDefaults(int8_t h[LEGEND_CHARS], int16_t m[LEGEND_CHARS],
			   uint8_t shape[LEGEND_CHARS], int16_t defaultMat)
{
	int c;

	for (c = 0; c < LEGEND_CHARS; c++) {
		h[c] = LEGEND_INVALID;
		m[c] = defaultMat;
		shape[c] = VOXMAP_SHAPE_PACK(VOXMAP_SHAPE_FULL, 0);
	}
	h[(unsigned char)'.'] = -1;
	for (c = '0'; c <= '9'; c++)
		h[c] = (int8_t)(c - '0');
}

/* Parse a shape token value ("full", "half", "ramp", "half-ramp"); false on an
 * unknown token. */
static bool parseShapeToken(const char *tok, int *out)
{
	if (strcmp(tok, "full") == 0)
		*out = VOXMAP_SHAPE_FULL;
	else if (strcmp(tok, "half") == 0)
		*out = VOXMAP_SHAPE_HALF;
	else if (strcmp(tok, "ramp") == 0)
		*out = VOXMAP_SHAPE_RAMP;
	else if (strcmp(tok, "half-ramp") == 0)
		*out = VOXMAP_SHAPE_HALF_RAMP;
	else
		return false;
	return true;
}

/* Parse a direction token value ("north", "south", "east", "west"); false on an
 * unknown token. */
static bool parseDirToken(const char *tok, int *out)
{
	if (strcmp(tok, "north") == 0)
		*out = VOXMAP_DIR_NORTH;
	else if (strcmp(tok, "east") == 0)
		*out = VOXMAP_DIR_EAST;
	else if (strcmp(tok, "south") == 0)
		*out = VOXMAP_DIR_SOUTH;
	else if (strcmp(tok, "west") == 0)
		*out = VOXMAP_DIR_WEST;
	else
		return false;
	return true;
}

int voxmapParseShapeAttrs(char *const *tokens, int count, const char *prefix,
			  uint8_t *outPacked)
{
	int shape = VOXMAP_SHAPE_FULL;
	int dir = VOXMAP_DIR_NORTH;
	bool dirSeen = false;
	int i;

	for (i = 0; i < count; i++) {
		const char *t = tokens[i];
		int v;

		if (strncmp(t, "shape=", 6) == 0) {
			if (!parseShapeToken(t + 6, &v)) {
				fprintf(stderr,
					"%s unknown shape '%s'; entry skipped\n",
					prefix, t + 6);
				return VOXMAP_ATTR_SKIP;
			}
			shape = v;
		} else if (strncmp(t, "dir=", 4) == 0) {
			if (!parseDirToken(t + 4, &v)) {
				fprintf(stderr,
					"%s unknown dir '%s'; entry skipped\n",
					prefix, t + 4);
				return VOXMAP_ATTR_SKIP;
			}
			dir = v;
			dirSeen = true;
		} else {
			/* Not a shape/dir attribute: a malformed legend line
			 * (the caller rejects it, preserving the old strict
			 * "too many / junk tokens fails" behaviour). */
			return VOXMAP_ATTR_BAD;
		}
	}
	if (dirSeen && shape != VOXMAP_SHAPE_RAMP &&
	    shape != VOXMAP_SHAPE_HALF_RAMP) {
		fprintf(stderr, "%s dir= on a non-ramp shape; ignored\n", prefix);
		dir = VOXMAP_DIR_NORTH;
	}
	if (!dirSeen && (shape == VOXMAP_SHAPE_RAMP ||
			 shape == VOXMAP_SHAPE_HALF_RAMP)) {
		fprintf(stderr, "%s ramp without dir=; defaulting to north\n",
			prefix);
		dir = VOXMAP_DIR_NORTH;
	}
	*outPacked = VOXMAP_SHAPE_PACK(shape, dir);
	return VOXMAP_ATTR_OK;
}

/* Parse one `@ <char> <height> <material>` line into the legend table. */
static bool parseLegend(const char *data, size_t start, size_t len,
			const char *label, int8_t h[LEGEND_CHARS],
			int16_t m[LEGEND_CHARS], uint8_t shape[LEGEND_CHARS],
			const MaterialTable *materials, int16_t defaultMat)
{
	char line[LEGEND_LINE_MAX];
	char *tokens[8];
	int ntok;
	unsigned char ch;
	int hIdx;
	int mIdx;
	int height;
	int id;

	if (len >= sizeof(line))
		return false;
	memcpy(line, data + start, len);
	line[len] = '\0';
	ntok = tokenize(line, tokens, 8);
	if (ntok > 8 || ntok < 3)
		return false;
	if (strcmp(tokens[0], "@") == 0) {
		if (ntok < 4 || strlen(tokens[1]) != 1)
			return false;
		ch = (unsigned char)tokens[1][0];
		hIdx = 2;
		mIdx = 3;
	} else if (tokens[0][0] == '@' && strlen(tokens[0]) == 2) {
		ch = (unsigned char)tokens[0][1];
		hIdx = 1;
		mIdx = 2;
	} else {
		return false;
	}
	/* A legend char must be a representable ASCII cell; a high byte cannot
	 * index the legend table (and can never match a map cell, which the
	 * validation pass rejects). Skip the entry and keep parsing: the line
	 * is unusable, but it must not corrupt the rest of the map. */
	if (ch >= LEGEND_CHARS) {
		fprintf(stderr,
			"voxmap: '%s' legend char 0x%02x is not a valid cell (>= %d); entry skipped\n",
			label, (unsigned)ch, LEGEND_CHARS);
		return true;
	}
	if (strlen(tokens[hIdx]) != 1 || tokens[hIdx][0] < '0' ||
	    tokens[hIdx][0] > '9') {
		fprintf(stderr, "voxmap: '%s' bad legend height '%s'\n", label,
			tokens[hIdx]);
		return false;
	}
	height = tokens[hIdx][0] - '0';
	id = defaultMat;
	if (materials != NULL) {
		int found = materialIdByName(materials, tokens[mIdx]);

		if (found < 0) {
			fprintf(stderr,
				"voxmap: '%s' legend char '%c' unknown material '%s'; using default\n",
				label, ch, tokens[mIdx]);
		} else {
			id = found;
		}
	}
	/* Optional shape=/dir= attributes (after the material). An unknown shape
	 * or dir value skips the entry (keep parsing); a non-attribute token is
	 * a malformed legend line (load fails), preserving the old strict
	 * "too many junk tokens fails" behaviour. */
	{
		char prefix[LEGEND_LINE_MAX];
		uint8_t packed;
		int rc;

		snprintf(prefix, sizeof(prefix),
			 "voxmap: '%s' legend char '%c'", label, ch);
		rc = voxmapParseShapeAttrs(&tokens[mIdx + 1], ntok - (mIdx + 1),
					   prefix, &packed);
		if (rc == VOXMAP_ATTR_BAD)
			return false;
		if (rc == VOXMAP_ATTR_SKIP)
			return true;	/* entry skipped, load continues */
		shape[ch] = packed;
	}
	h[ch] = (int8_t)height;
	m[ch] = (int16_t)id;
	return true;
}

/* Parse a decimal float token in full; reject empty / trailing junk / a
 * non-finite value. */
static bool parseFloatToken(const char *tok, float *out)
{
	char *end;
	float v;

	if (tok == NULL || tok[0] == '\0')
		return false;
	v = strtof(tok, &end);
	if (end == tok || *end != '\0' || !isfinite(v))
		return false;
	*out = v;
	return true;
}

/* Clamp a channel to 0..255 (a negative or NaN value becomes 0). */
static float clampChannel(float v)
{
	if (!(v > 0.0f))
		return 0.0f;
	if (v > 255.0f)
		return 255.0f;
	return v;
}

static void addLight(VoxmapLight *lights, int *count, const VoxmapLight *l)
{
	if (*count >= VOXMAP_MAX_LIGHTS) {
		fprintf(stderr, "voxmap: more than %d lights; line skipped\n",
			VOXMAP_MAX_LIGHTS);
		return;
	}
	lights[*count] = *l;
	(*count)++;
}

/* Parse one `$` light line into the emitter array. Malformed lines are skipped
 * with a diagnostic (never a load failure). */
static void parseLightLine(VoxmapLight *lights, int *count, const char *data,
			   size_t start, size_t len, const char *label)
{
	char line[LEGEND_LINE_MAX];
	char *tokens[16];
	int ntok;
	VoxmapLight l;

	if (len >= sizeof(line)) {
		fprintf(stderr, "voxmap: '%s' light line too long; skipped\n",
			label);
		return;
	}
	memcpy(line, data + start, len);
	line[len] = '\0';
	ntok = tokenize(line, tokens, 16);
	if (ntok < 2) {
		fprintf(stderr, "voxmap: '%s' empty light line; skipped\n",
			label);
		return;
	}
	if (strcmp(tokens[0], "$") != 0) {
		fprintf(stderr,
			"voxmap: '%s' not a light line ('%s'); skipped\n",
			label, tokens[0]);
		return;
	}
	memset(&l, 0, sizeof(l));
	if (strcmp(tokens[1], "point") == 0) {
		if (ntok != 8 && ntok != 9) {
			fprintf(stderr,
				"voxmap: '%s' point light needs 8 or 9 tokens (%d); skipped\n",
				label, ntok);
			return;
		}
		l.kind = VOXMAP_LIGHT_POINT;
		if (!parseFloatToken(tokens[2], &l.x) ||
		    !parseFloatToken(tokens[3], &l.y) ||
		    !parseFloatToken(tokens[4], &l.z) ||
		    !parseFloatToken(tokens[5], &l.r) ||
		    !parseFloatToken(tokens[6], &l.g) ||
		    !parseFloatToken(tokens[7], &l.b)) {
			fprintf(stderr,
				"voxmap: '%s' point light has a bad number; skipped\n",
				label);
			return;
		}
		if (ntok == 9) {
			if (!parseFloatToken(tokens[8], &l.radius)) {
				fprintf(stderr,
					"voxmap: '%s' point light has a bad radius; skipped\n",
					label);
				return;
			}
		}
	} else if (strcmp(tokens[1], "spot") == 0) {
		float dx;
		float dy;
		float dz;
		float lenDir;

		if (ntok != 12 && ntok != 13) {
			fprintf(stderr,
				"voxmap: '%s' spot light needs 12 or 13 tokens (%d); skipped\n",
				label, ntok);
			return;
		}
		l.kind = VOXMAP_LIGHT_SPOT;
		if (!parseFloatToken(tokens[2], &l.x) ||
		    !parseFloatToken(tokens[3], &l.y) ||
		    !parseFloatToken(tokens[4], &l.z) ||
		    !parseFloatToken(tokens[5], &l.r) ||
		    !parseFloatToken(tokens[6], &l.g) ||
		    !parseFloatToken(tokens[7], &l.b) ||
		    !parseFloatToken(tokens[8], &dx) ||
		    !parseFloatToken(tokens[9], &dy) ||
		    !parseFloatToken(tokens[10], &dz) ||
		    !parseFloatToken(tokens[11], &l.halfAngleDeg)) {
			fprintf(stderr,
				"voxmap: '%s' spot light has a bad number; skipped\n",
				label);
			return;
		}
		lenDir = sqrtf(dx * dx + dy * dy + dz * dz);
		if (!(lenDir > 0.0f) || !isfinite(lenDir)) {
			fprintf(stderr,
				"voxmap: '%s' spot light has a zero direction; skipped\n",
				label);
			return;
		}
		l.dir[0] = dx / lenDir;
		l.dir[1] = dy / lenDir;
		l.dir[2] = dz / lenDir;
		if (!(l.halfAngleDeg > 0.0f)) {
			fprintf(stderr,
				"voxmap: '%s' spot light has a non-positive angle; skipped\n",
				label);
			return;
		}
		if (ntok == 13) {
			if (!parseFloatToken(tokens[12], &l.radius)) {
				fprintf(stderr,
					"voxmap: '%s' spot light has a bad radius; skipped\n",
					label);
				return;
			}
		} else {
			l.radius = VOXMAP_LIGHT_DEFAULT_RADIUS;
		}
	} else {
		fprintf(stderr, "voxmap: '%s' unknown light kind '%s'; skipped\n",
			label, tokens[1]);
		return;
	}
	l.r = clampChannel(l.r);
	l.g = clampChannel(l.g);
	l.b = clampChannel(l.b);
	addLight(lights, count, &l);
}

/* Parsed state shared by the heightmap and slice fillers. */
typedef struct ParseCtx {
	const char *label;
	const char *text;
	size_t length;
	int8_t legendH[LEGEND_CHARS];
	int16_t legendM[LEGEND_CHARS];
	uint8_t legendShape[LEGEND_CHARS];
	int16_t defaultMat;
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount;
} ParseCtx;

/* Allocate a zeroed map of the given dimensions with every voxel air
 * (material -1, shape FULL). NULL on an over-large volume or OOM. Precondition (both
 * callers validate it): width, depth, levels are all > 0 and width/depth are
 * <= VOXMAP_MAX_DIM. */
static Voxmap *allocVoxmap(int width, int depth, int levels)
{
	Voxmap *map;
	size_t n;

	if (levels > VOXMAP_MAX_DIM)
		return NULL;
	if ((size_t)width * (size_t)depth * (size_t)levels > VOXMAP_MAX_CELLS)
		return NULL;
	n = (size_t)width * (size_t)depth * (size_t)levels;
	map = calloc(1, sizeof(*map));
	if (map == NULL)
		return NULL;
	map->solid = malloc(n);
	map->materials = malloc(n * sizeof(*map->materials));
	map->shapes = malloc(n);
	map->ground = malloc((size_t)width * (size_t)depth);
	if (map->solid == NULL || map->materials == NULL || map->shapes == NULL ||
	    map->ground == NULL) {
		free(map->solid);
		free(map->materials);
		free(map->shapes);
		free(map->ground);
		free(map);
		return NULL;
	}
	memset(map->solid, 0, n);
	memset(map->materials, 0xFF, n * sizeof(*map->materials));	/* -1 */
	memset(map->shapes, 0, n);	/* every voxel FULL (0) by default */
	memset(map->ground, 0, (size_t)width * (size_t)depth);
	map->width = width;
	map->depth = depth;
	map->levels = levels;
	return map;
}

/* Copy the parsed lights onto the map. */
static void attachLights(Voxmap *map, const ParseCtx *ctx)
{
	if (ctx->lightCount > 0)
		memcpy(map->lights, ctx->lights,
		       (size_t)ctx->lightCount * sizeof(*ctx->lights));
	map->lightCount = ctx->lightCount;
}

/* Heightmap (single-section) fill. A height-h column is solid voxels 0..h-1;
 * a height-0 cell sets the ground tile flag (no voxel) and still carries its
 * material at level 0 for the top-face query. */
static Voxmap *fillHeightmap(ParseCtx *ctx)
{
	Voxmap *map;
	int width = -1;
	int depth = 0;
	int maxH = 0;
	size_t pos;
	int row;
	int levels;

	/* Pass 1: validate rows, pin width, count depth, find max height. */
	pos = 0;
	while (pos < ctx->length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		size_t i;

		while (pos < ctx->length && ctx->text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < ctx->length)
			pos++;
		rowLen = trimmedLength(ctx->text, start, end);
		if (rowLen == 0 ||
		    isDirectiveLine(ctx->text, start, rowLen))
			continue;
		if (width < 0) {
			if (rowLen > VOXMAP_MAX_DIM) {
				fprintf(stderr,
					"voxmap: '%s' row too wide (%zu > %d)\n",
					ctx->label, rowLen, VOXMAP_MAX_DIM);
				return NULL;
			}
			width = (int)rowLen;
		} else if ((int)rowLen != width) {
			fprintf(stderr,
				"voxmap: '%s' ragged row %d (%zu chars, expected %d)\n",
				ctx->label, depth + 1, rowLen, width);
			return NULL;
		}
		for (i = start; i < start + rowLen; i++) {
			unsigned char c = (unsigned char)ctx->text[i];
			int8_t h;

			if (c >= LEGEND_CHARS ||
			    (h = ctx->legendH[c]) == LEGEND_INVALID) {
				fprintf(stderr,
					"voxmap: '%s' invalid cell '%c' at row %d\n",
					ctx->label,
					(c >= 32 && c < 127) ? c : '?',
					depth + 1);
				return NULL;
			}
			if (h > maxH)
				maxH = h;
		}
		depth++;
		if (depth > VOXMAP_MAX_DIM) {
			fprintf(stderr, "voxmap: '%s' too many rows (> %d)\n",
				ctx->label, VOXMAP_MAX_DIM);
			return NULL;
		}
	}
	if (width <= 0 || depth <= 0) {
		fprintf(stderr, "voxmap: '%s' is empty\n", ctx->label);
		return NULL;
	}
	levels = maxH > 0 ? maxH : 1;
	map = allocVoxmap(width, depth, levels);
	if (map == NULL) {
		fprintf(stderr,
			"voxmap: '%s' map too large (%dx%dx%d cells)\n",
			ctx->label, width, depth, levels);
		return NULL;
	}

	/* Pass 2: fill the occupancy grid and its per-voxel materials. */
	pos = 0;
	row = 0;
	while (pos < ctx->length && row < depth) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		int col;

		while (pos < ctx->length && ctx->text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < ctx->length)
			pos++;
		rowLen = trimmedLength(ctx->text, start, end);
		if (rowLen == 0 ||
		    isDirectiveLine(ctx->text, start, rowLen))
			continue;
		for (col = 0; col < width; col++) {
			unsigned char c =
				(unsigned char)ctx->text[start + (size_t)col];
			int8_t h = ctx->legendH[c];
			int16_t id = ctx->legendM[c];
			uint8_t shp = ctx->legendShape[c];
			int y;

			if (h < 0)
				continue;	/* void */
			if (h == 0) {
				/* No voxel to shape: the ground tile stays a
				 * full flat top (the shape is documented as
				 * ignored on a height-0 entry). */
				map->ground[(size_t)row * width + col] = 1;
				map->materials[voxelIndex(map, col, 0, row)] = id;
				continue;
			}
			for (y = 0; y < h; y++) {
				size_t idx = voxelIndex(map, col, y, row);

				map->solid[idx] = 1;
				map->materials[idx] = id;
				map->shapes[idx] = shp;
			}
		}
		row++;
	}
	return map;
}

/* Slice (multi-section) fill. Section s is level y = s; a cell char is solid
 * with its legend material unless it is air ('.', a void legend char, or a
 * space). All sections must share one width/depth. */
static Voxmap *fillSlices(ParseCtx *ctx)
{
	Voxmap *map;
	int width = -1;
	int depth = -1;
	int levels = 0;
	int curDepth = 0;
	size_t pos;
	int section;

	/* Pass 1: validate sections, pin width/depth/levels. */
	pos = 0;
	while (pos < ctx->length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		size_t i;

		while (pos < ctx->length && ctx->text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < ctx->length)
			pos++;
		rowLen = sliceRowLength(ctx->text, start, end);
		if (rowLen == 0)
			continue;	/* empty line */
		if (isDirectiveLine(ctx->text, start, rowLen))
			continue;
		if (isSeparatorLine(ctx->text, start, end - start)) {
			if (curDepth == 0) {
				fprintf(stderr,
					"voxmap: '%s' empty map section (level %d)\n",
					ctx->label, levels);
				return NULL;
			}
			if (depth < 0)
				depth = curDepth;
			else if (curDepth != depth) {
				fprintf(stderr,
					"voxmap: '%s' sections differ in depth (%d vs %d)\n",
					ctx->label, curDepth, depth);
				return NULL;
			}
			levels++;
			curDepth = 0;
			continue;
		}
		if (width < 0) {
			if (rowLen > VOXMAP_MAX_DIM) {
				fprintf(stderr,
					"voxmap: '%s' section row too wide (%zu > %d)\n",
					ctx->label, rowLen, VOXMAP_MAX_DIM);
				return NULL;
			}
			width = (int)rowLen;
		} else if ((int)rowLen != width) {
			fprintf(stderr,
				"voxmap: '%s' ragged section row %d (%zu chars, expected %d)\n",
				ctx->label, curDepth + 1, rowLen, width);
			return NULL;
		}
		for (i = start; i < start + rowLen; i++) {
			unsigned char c = (unsigned char)ctx->text[i];

			if (c == ' ')
				continue;	/* air */
			if (c >= LEGEND_CHARS ||
			    ctx->legendH[c] == LEGEND_INVALID) {
				fprintf(stderr,
					"voxmap: '%s' invalid slice cell '%c' at level %d row %d\n",
					ctx->label,
					(c >= 32 && c < 127) ? c : '?',
					levels, curDepth + 1);
				return NULL;
			}
		}
		curDepth++;
		if (curDepth > VOXMAP_MAX_DIM) {
			fprintf(stderr,
				"voxmap: '%s' section too deep (> %d)\n",
				ctx->label, VOXMAP_MAX_DIM);
			return NULL;
		}
	}
	if (curDepth == 0) {
		fprintf(stderr,
			"voxmap: '%s' empty trailing map section\n", ctx->label);
		return NULL;
	}
	/* depth is set by the first separator (fillSlices only runs when one
	 * exists), so the last section only needs the equality check. */
	if (curDepth != depth) {
		fprintf(stderr,
			"voxmap: '%s' sections differ in depth (%d vs %d)\n",
			ctx->label, curDepth, depth);
		return NULL;
	}
	levels++;
	map = allocVoxmap(width, depth, levels);
	if (map == NULL) {
		fprintf(stderr,
			"voxmap: '%s' map too large (%dx%dx%d cells)\n",
			ctx->label, width, depth, levels);
		return NULL;
	}

	/* Pass 2: fill per level. */
	pos = 0;
	section = 0;
	{
		int row = 0;

		while (pos < ctx->length && section < levels) {
			size_t start = pos;
			size_t end;
			size_t rowLen;
			int col;

			while (pos < ctx->length && ctx->text[pos] != '\n')
				pos++;
			end = pos;
			if (pos < ctx->length)
				pos++;
			rowLen = sliceRowLength(ctx->text, start, end);
			if (rowLen == 0)
				continue;
			if (isDirectiveLine(ctx->text, start, rowLen))
				continue;
			if (isSeparatorLine(ctx->text, start, end - start)) {
				section++;
				row = 0;
				continue;
			}
			for (col = 0; col < width; col++) {
				unsigned char c =
					(unsigned char)ctx->text[start +
								(size_t)col];
				size_t idx = voxelIndex(map, col, section, row);

				if (c == ' ' || ctx->legendH[c] < 0) {
					map->solid[idx] = 0;
					map->materials[idx] = -1;
					map->shapes[idx] = VOXMAP_SHAPE_PACK(
						VOXMAP_SHAPE_FULL, 0);
				} else {
					map->solid[idx] = 1;
					map->materials[idx] =
						ctx->legendM[c];
					map->shapes[idx] =
						ctx->legendShape[c];
				}
			}
			row++;
		}
	}
	return map;
}

/* Parse `length` bytes of voxel-map text (see voxmap.h). */
static Voxmap *parseVoxmap(const char *text, size_t length, const char *label,
			   const MaterialTable *materials)
{
	ParseCtx ctx;
	Voxmap *map;
	size_t pos;
	bool hasSeparator = false;

	if (text == NULL)
		return NULL;
	memset(&ctx, 0, sizeof(ctx));
	ctx.label = label;
	ctx.text = text;
	ctx.length = length;
	ctx.defaultMat = 0;
	if (materials != NULL) {
		int d = materialIdByName(materials, "default");

		if (d >= 0)
			ctx.defaultMat = (int16_t)d;
	}
	legendDefaults(ctx.legendH, ctx.legendM, ctx.legendShape,
		       ctx.defaultMat);

	/* Pass 0: collect legend + light lines and detect any separator. */
	pos = 0;
	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;			/* skip '\n' */
		rowLen = trimmedLength(text, start, end);
		if (rowLen == 0)
			continue;
		if (isLegendLine(text, start, rowLen)) {
			if (!parseLegend(text, start, rowLen, label,
					 ctx.legendH, ctx.legendM,
					 ctx.legendShape, materials,
					 ctx.defaultMat))
				return NULL;
		} else if (isLightLine(text, start, rowLen)) {
			parseLightLine(ctx.lights, &ctx.lightCount, text, start,
				       rowLen, label);
		} else if (isSeparatorLine(text, start, end - start)) {
			hasSeparator = true;
		}
	}

	map = hasSeparator ? fillSlices(&ctx) : fillHeightmap(&ctx);
	if (map == NULL)
		return NULL;
	attachLights(map, &ctx);
	return map;
}

Voxmap *parseVoxmapText(const char *text, size_t length,
			const MaterialTable *materials)
{
	return parseVoxmap(text, length, "<memory>", materials);
}

Voxmap *loadVoxmap(const char *path, const MaterialTable *materials)
{
	size_t size = 0;
	char *data;
	Voxmap *map;

	if (path == NULL)
		return NULL;
	data = readWholeFile(path, &size);
	if (data == NULL) {
		fprintf(stderr, "voxmap: cannot read '%s'\n", path);
		return NULL;
	}
	map = parseVoxmap(data, size, path, materials);
	free(data);
	return map;
}

void destroyVoxmap(Voxmap *map)
{
	if (map == NULL)
		return;
	free(map->solid);
	free(map->materials);
	free(map->shapes);
	free(map->ground);
	free(map);
}

bool voxmapParseLightLine(const char *text, size_t length, VoxmapLight *out)
{
	VoxmapLight lights[1];
	int count = 0;

	if (text == NULL || out == NULL)
		return false;
	parseLightLine(lights, &count, text, 0, length, "<legend>");
	if (count == 0)
		return false;
	*out = lights[0];
	return true;
}

Voxmap *voxmapBuildRawShaped(int width, int depth, int levels,
			     const uint8_t *solid, const int16_t *materials,
			     const uint8_t *shapes,
			     const VoxmapLight *lights, int lightCount)
{
	Voxmap *map;
	size_t n;

	if (width < 1 || depth < 1 || levels < 1 ||
	    width > VOXMAP_MAX_DIM || depth > VOXMAP_MAX_DIM ||
	    levels > VOXMAP_MAX_DIM)
		return NULL;
	map = allocVoxmap(width, depth, levels);
	if (map == NULL)
		return NULL;
	n = (size_t)width * (size_t)depth * (size_t)levels;
	if (solid != NULL)
		memcpy(map->solid, solid, n);
	if (materials != NULL)
		memcpy(map->materials, materials, n * sizeof(*map->materials));
	if (shapes != NULL)
		memcpy(map->shapes, shapes, n);
	if (lights != NULL && lightCount > 0) {
		int keep = lightCount < VOXMAP_MAX_LIGHTS ? lightCount
							  : VOXMAP_MAX_LIGHTS;

		memcpy(map->lights, lights, (size_t)keep * sizeof(*lights));
		map->lightCount = keep;
	}
	return map;
}

Voxmap *voxmapBuildRaw(int width, int depth, int levels,
		       const uint8_t *solid, const int16_t *materials,
		       const VoxmapLight *lights, int lightCount)
{
	return voxmapBuildRawShaped(width, depth, levels, solid, materials,
				    NULL, lights, lightCount);
}

int voxmapWidth(const Voxmap *map)
{
	return map == NULL ? 0 : map->width;
}

int voxmapDepth(const Voxmap *map)
{
	return map == NULL ? 0 : map->depth;
}

int voxmapLevels(const Voxmap *map)
{
	return map == NULL ? 0 : map->levels;
}

int voxmapHeightAt(const Voxmap *map, int x, int z)
{
	int y;

	if (map == NULL || x < 0 || z < 0 || x >= map->width ||
	    z >= map->depth)
		return -1;
	for (y = map->levels - 1; y >= 0; y--) {
		if (map->solid[voxelIndex(map, x, y, z)])
			return y + 1;
	}
	if (map->ground[(size_t)z * map->width + x])
		return 0;
	return -1;
}

bool voxmapIsVoid(const Voxmap *map, int x, int z)
{
	return voxmapHeightAt(map, x, z) < 0;
}

int voxmapMaterialAt(const Voxmap *map, int x, int z)
{
	int y;

	if (map == NULL || x < 0 || z < 0 || x >= map->width ||
	    z >= map->depth)
		return -1;
	for (y = map->levels - 1; y >= 0; y--) {
		size_t idx = voxelIndex(map, x, y, z);

		if (map->solid[idx])
			return map->materials[idx];
	}
	if (map->ground[(size_t)z * map->width + x])
		return map->materials[voxelIndex(map, x, 0, z)];
	return -1;
}

bool voxmapSolidAt(const Voxmap *map, int x, int y, int z)
{
	if (map == NULL || x < 0 || y < 0 || z < 0 || x >= map->width ||
	    y >= map->levels || z >= map->depth)
		return false;
	return map->solid[voxelIndex(map, x, y, z)] != 0;
}

int voxmapMaterialAtVoxel(const Voxmap *map, int x, int y, int z)
{
	if (map == NULL || x < 0 || y < 0 || z < 0 || x >= map->width ||
	    y >= map->levels || z >= map->depth)
		return -1;
	return map->materials[voxelIndex(map, x, y, z)];
}

int voxmapShapeAt(const Voxmap *map, int x, int y, int z)
{
	if (map == NULL || x < 0 || y < 0 || z < 0 || x >= map->width ||
	    y >= map->levels || z >= map->depth)
		return VOXMAP_SHAPE_FULL;
	return VOXMAP_SHAPE_OF(map->shapes[voxelIndex(map, x, y, z)]);
}

int voxmapShapeDirAt(const Voxmap *map, int x, int y, int z)
{
	uint8_t packed;

	if (map == NULL || x < 0 || y < 0 || z < 0 || x >= map->width ||
	    y >= map->levels || z >= map->depth)
		return -1;
	packed = map->shapes[voxelIndex(map, x, y, z)];
	if (VOXMAP_SHAPE_OF(packed) != VOXMAP_SHAPE_RAMP &&
	    VOXMAP_SHAPE_OF(packed) != VOXMAP_SHAPE_HALF_RAMP)
		return -1;
	return VOXMAP_SHAPE_DIR_OF(packed);
}

bool voxmapFullAt(const Voxmap *map, int x, int y, int z)
{
	size_t idx;

	if (map == NULL || x < 0 || y < 0 || z < 0 || x >= map->width ||
	    y >= map->levels || z >= map->depth)
		return false;
	idx = voxelIndex(map, x, y, z);
	return map->solid[idx] &&
	       VOXMAP_SHAPE_OF(map->shapes[idx]) == VOXMAP_SHAPE_FULL;
}

int voxmapLightCount(const Voxmap *map)
{
	return map == NULL ? 0 : map->lightCount;
}

const VoxmapLight *voxmapLightAt(const Voxmap *map, int index)
{
	if (map == NULL || index < 0 || index >= map->lightCount)
		return NULL;
	return &map->lights[index];
}

/* --- face generation --------------------------------------------------- */

/* Side directions: 0 = +Z, 1 = +X, 2 = -Z, 3 = -X. */
static const int kSideDx[4] = { 0, 1, 0, -1 };
static const int kSideDz[4] = { 1, 0, -1, 0 };

/* A ramp direction (VOXMAP_DIR_*) mapped to its side index: EAST -> +X (1),
 * SOUTH -> +Z (0), WEST -> -X (3), NORTH -> -Z (2). Indexed by VOXMAP_DIR_*. */
static const int kDirToSide[4] = { 2, 1, 0, 3 };

/* Built-in fallback regions (used with a NULL material table). */
static const float kFallbackTop[4][2] = ATLAS_UV_TOP;
static const float kFallbackSide[4][2] = ATLAS_UV_SIDE;
static const float kFallbackBottom[4][2] = ATLAS_UV_SPARE;

/* One-time overflow diagnostic: appendVoxelFace returns false when the draw
 * list is full, and the face is silently dropped by design (a bounded list
 * must not grow mid-frame). Report the first drop so vanishing geometry is
 * diagnosable; later drops stay quiet to avoid per-face log spam. */
static bool g_faceOverflowReported = false;

/* Fill `uv` with the material's oriented UV quad for `face`, or the built-in
 * fallback region when the table/id is unusable. A negative id casts to a
 * huge size_t, so the range check alone covers it. */
static void materialUV(const MaterialTable *materials, int id, FaceId face,
		       float uv[4][2])
{
	if (materials != NULL && (size_t)id < materials->count) {
		materialFaceUV(&materials->items[id].rect[face], face, uv);
		return;
	}
	if (face == FACE_TOP)
		memcpy(uv, kFallbackTop, sizeof(kFallbackTop));
	else if (face == FACE_BOTTOM)
		memcpy(uv, kFallbackBottom, sizeof(kFallbackBottom));
	else
		memcpy(uv, kFallbackSide, sizeof(kFallbackTop));
}

static uint8_t materialAlpha(const MaterialTable *materials, int id)
{
	if (materials != NULL && (size_t)id < materials->count)
		return (uint8_t)materials->items[id].alpha;
	return (uint8_t)ALPHA_BLEND;
}

static void emitFace(DrawList *list, const float quad[4][3],
		     const float uv[4][2], uint8_t alphaMode,
		     const uint32_t cornerTint[4])
{
	if (!appendVoxelFaceShaded(list, quad, uv, alphaMode, cornerTint) &&
	    !g_faceOverflowReported) {
		g_faceOverflowReported = true;
		fprintf(stderr,
			"voxmap: draw list full (%zu items); faces dropped (reported once)\n",
			drawListCount(list));
	}
}

/* Multiply one 0..255 channel by `factor`, clamped, rounded to nearest. */
static uint8_t shadeChannel(uint8_t c, float factor)
{
	float v = (float)c * factor;

	if (v <= 0.0f)
		return 0;
	if (v >= 255.0f)
		return 255;
	return (uint8_t)(v + 0.5f);
}

/* Apply the face shade, the per-column checker boost, the sampled light factor
 * and an AO percent to the caller tint: RGB multiplied and clamped per channel,
 * alpha preserved. `x`/`z` are the column's grid coordinates (odd tiles are
 * brightened); `light` is the 0..255 per-channel factor (255 = full);
 * `aoPercent` is the per-corner occlusion multiplier (100 = none). The flat
 * path passes 100, an exact x1.0, so its tints are byte-identical to T14. */
static uint32_t shadeTint(uint32_t tint, float faceShade, int x, int z,
			  const uint8_t light[3], int aoPercent)
{
	float base = faceShade *
		     (((x + z) & 1) ? VOXMAP_CHECKER_BOOST : 1.0f) *
		     ((float)aoPercent / 100.0f);
	uint8_t r = shadeChannel((uint8_t)((tint >> 24) & 0xffu),
				 base * (float)light[0] / 255.0f);
	uint8_t g = shadeChannel((uint8_t)((tint >> 16) & 0xffu),
				 base * (float)light[1] / 255.0f);
	uint8_t b = shadeChannel((uint8_t)((tint >> 8) & 0xffu),
				 base * (float)light[2] / 255.0f);
	uint8_t a = (uint8_t)(tint & 0xffu);

	return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
	       ((uint32_t)b << 8) | (uint32_t)a;
}

/* Build the 4 corner tints for a face (canonical corner order). `light` holds
 * the per-corner factors and `ao` the per-corner occupancy counts (both only
 * meaningful when `smooth`); `light[0]` is the flat factor. In the debug view
 * the tint IS the light colour (no material tint, shade, checker or AO). */
static void faceCornerTints(uint32_t tint, float faceShade, int x, int z,
			    const uint8_t light[4][3], const int ao[4],
			    bool smooth, bool lightDebug, uint32_t out[4])
{
	int k;

	if (lightDebug) {
		for (k = 0; k < 4; k++) {
			const uint8_t *f = smooth ? light[k] : light[0];

			out[k] = DRAW_TINT(f[0], f[1], f[2], 255);
		}
		return;
	}
	if (smooth) {
		for (k = 0; k < 4; k++)
			out[k] = shadeTint(tint, faceShade, x, z, light[k],
					   lightGridAoPercent(ao[k]));
		return;
	}
	for (k = 0; k < 4; k++)
		out[k] = shadeTint(tint, faceShade, x, z, light[0], 100);
}

/* Corner cells for a horizontal face's corner `k` (0..3, canonical order):
 * the 2x2 block of columns around the corner at the face's air level `h`. The
 * face's own air cell (x, h, z) is one of the four; *ownIndex points at it.
 * Used by top faces (h = top level) and bottom faces (h = below level). */
static void topCornerCells(int x, int z, int h, int k, int cells[4][3],
			   int *ownIndex)
{
	int cx = x + ((k == 1 || k == 2) ? 1 : 0);
	int cz = z + ((k == 2 || k == 3) ? 1 : 0);
	int ox = cx - x;
	int oz = cz - z;

	cells[0][0] = cx - 1;	cells[0][1] = h;	cells[0][2] = cz - 1;
	cells[1][0] = cx;	cells[1][1] = h;	cells[1][2] = cz - 1;
	cells[2][0] = cx - 1;	cells[2][1] = h;	cells[2][2] = cz;
	cells[3][0] = cx;	cells[3][1] = h;	cells[3][2] = cz;
	*ownIndex = (1 - oz) * 2 + (1 - ox);
}

/* Corner cells for a side face's corner: the 2x2 block in the face plane on
 * the neighbour side. `a` is the corner's coordinate along the face's
 * horizontal axis and `y` its level; (nx, nz) is the neighbour column the face
 * looks across. `axisX` selects x as the along axis (dirs 0/2), else z
 * (dirs 1/3). The corner's own air cell is the block entry at (a, y) = index 3,
 * so a solid cell there (a taller neighbour) is skipped in favour of the in-
 * plane air neighbours. */
static void sideCornerCells(int a, int y, int nx, int nz, bool axisX,
			    int cells[4][3])
{
	int i = 0;
	int dy;
	int da;

	for (dy = -1; dy <= 0; dy++) {
		for (da = -1; da <= 0; da++) {
			if (axisX) {
				cells[i][0] = a + da;
				cells[i][1] = y + dy;
				cells[i][2] = nz;
			} else {
				cells[i][0] = nx;
				cells[i][1] = y + dy;
				cells[i][2] = a + da;
			}
			i++;
		}
	}
}

/* Fill the 4 per-corner light factors and AO counts for a horizontal face at
 * air level `h`. */
static void topCorners(const LightGrid *lights, int x, int z, int h,
		       uint8_t light[4][3], int ao[4])
{
	int k;

	for (k = 0; k < 4; k++) {
		int cells[4][3];
		int own;

		topCornerCells(x, z, h, k, cells, &own);
		lightGridCornerAverage(lights, cells, own, light[k]);
		ao[k] = lightGridCornerOcclusion(lights, cells, own);
	}
}

/* Fill the 4 per-corner light factors and AO counts for a side face. The
 * corner order matches the quad: bottom-start, bottom-end, top-end, top-start.
 * The two bottom corners sample at y0 (the run bottom), the two top corners
 * at y1 (the run top), so the GPU interpolates the vertical gradient. */
static void sideCorners(const LightGrid *lights, int x, int z, int dir,
			int y0, int y1, uint8_t light[4][3], int ao[4])
{
	int nx = x + kSideDx[dir];
	int nz = z + kSideDz[dir];
	bool axisX = (dir == 0 || dir == 2);
	int base = (axisX ? x : z) + ((dir >= 2) ? 1 : 0);
	int step = (dir >= 2) ? -1 : 1;
	int as[4] = { base, base + step, base + step, base };
	int ys[4] = { y0, y0, y1, y1 };
	int k;

	for (k = 0; k < 4; k++) {
		int cells[4][3];

		sideCornerCells(as[k], ys[k], nx, nz, axisX, cells);
		lightGridCornerAverage(lights, cells, 3, light[k]);
		ao[k] = lightGridCornerOcclusion(lights, cells, 3);
	}
}

/* The per-channel brightness factor at a face's adjacent air cell; a NULL
 * light grid keeps the face at full brightness. */
static void sampleLight(const LightGrid *lights, int x, int y, int z,
			uint8_t out[3])
{
	if (lights == NULL) {
		out[0] = 255;
		out[1] = 255;
		out[2] = 255;
		return;
	}
	lightGridFactorAt(lights, x, y, z, out);
}

/* Per-direction side shade (dir 0..3 = +Z, +X, -Z, -X). */
static const float kSideShade[4] = {
	VOXMAP_SHADE_SIDE_PZ, VOXMAP_SHADE_SIDE_PX,
	VOXMAP_SHADE_SIDE_NZ, VOXMAP_SHADE_SIDE_NX,
};

/* Unit ground-plane direction from the target toward the camera, for yaw
 * degrees. The camera sits at target + (sin yaw, cos yaw) * cos(pitch) *
 * CAMERA_DISTANCE (see cameraView), so the horizontal direction is
 * (sin yaw, cos yaw). */
static void cameraGroundDir(float yawDeg, float *outX, float *outZ)
{
	float yawRad = yawDeg * (PI_F / 180.0f);

	*outX = sinf(yawRad);
	*outZ = cosf(yawRad);
}

/* True when the side with outward normal (kSideDx[dir], kSideDz[dir]) faces
 * away from the camera and is culled: dot(n, toCameraGround) <=
 * CAMERA_CULL_EPS. An edge-on side has dot exactly 0, so it is culled. */
static bool sideCulled(int dir, float toCamX, float toCamZ)
{
	float dot = (float)kSideDx[dir] * toCamX +
		    (float)kSideDz[dir] * toCamZ;

	return dot <= CAMERA_CULL_EPS;
}

/* A horizontal (or tilted) face: a 4-corner quad sampling the 2x2 light block
 * at `lightLevel` (the air level the face looks across), shaded by a single
 * face constant. Tops, bottoms and ramp slope quads share this. */
static void emitHorizontalShaded(DrawList *list, const float quad[4][3],
				 int x, int z, int lightLevel, float faceShade,
				 const float uv[4][2], uint8_t alphaMode,
				 uint32_t tint, const LightGrid *lights,
				 const VoxmapEmitOptions *opts,
				 const float debugUV[4][2])
{
	uint8_t light[4][3];
	int ao[4] = { 0, 0, 0, 0 };
	uint32_t cornerTint[4];

	if (opts->smooth)
		topCorners(lights, x, z, lightLevel, light, ao);
	else
		sampleLight(lights, x, lightLevel, z, light[0]);
	faceCornerTints(tint, faceShade, x, z, light, ao, opts->smooth,
			opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
}

/* Top face of a solid voxel: 1x1 at `topY` (y + 1 for FULL, y + 0.5 for HALF),
 * sampling the air cell above the voxel at `lightLevel`. */
static void emitTop(DrawList *list, int x, int z, float topY, int lightLevel,
		    const float uv[4][2], uint8_t alphaMode, uint32_t tint,
		    const LightGrid *lights, const VoxmapEmitOptions *opts,
		    const float debugUV[4][2])
{
	float quad[4][3] = {
		{ (float)x, topY, (float)z },
		{ (float)(x + 1), topY, (float)z },
		{ (float)(x + 1), topY, (float)(z + 1) },
		{ (float)x, topY, (float)(z + 1) },
	};
	emitHorizontalShaded(list, quad, x, z, lightLevel, VOXMAP_SHADE_TOP, uv,
			     alphaMode, tint, lights, opts, debugUV);
}

/* Bottom face of the solid voxel at level `y`: the same x/z quad as the top, at
 * world `bottomY`, sampling the air cell below (lightLevel = y - 1). Shade is
 * the darkest face (VOXMAP_SHADE_BOTTOM). */
static void emitBottom(DrawList *list, int x, int z, float bottomY,
		       int lightLevel, const float uv[4][2], uint8_t alphaMode,
		       uint32_t tint, const LightGrid *lights,
		       const VoxmapEmitOptions *opts, const float debugUV[4][2])
{
	float quad[4][3] = {
		{ (float)x, bottomY, (float)z },
		{ (float)(x + 1), bottomY, (float)z },
		{ (float)(x + 1), bottomY, (float)(z + 1) },
		{ (float)x, bottomY, (float)(z + 1) },
	};
	emitHorizontalShaded(list, quad, x, z, lightLevel, VOXMAP_SHADE_BOTTOM,
			     uv, alphaMode, tint, lights, opts, debugUV);
}

/* A ramp's slope quad (TOP slot): light samples the cell above the voxel
 * (lightLevel = the ramp's level + 1), like a top face. */
static void emitSlope(DrawList *list, const float quad[4][3], int x, int z,
		      int lightLevel, const float uv[4][2], uint8_t alphaMode,
		      uint32_t tint, const LightGrid *lights,
		      const VoxmapEmitOptions *opts, const float debugUV[4][2])
{
	emitHorizontalShaded(list, quad, x, z, lightLevel, VOXMAP_SHADE_TOP, uv,
			     alphaMode, tint, lights, opts, debugUV);
}

/* A ramp's triangular side, emitted as the degenerate quad [A, B, C, C]. The
 * light is a single flat sample of the side neighbour at the voxel's level,
 * copied to every corner (the brief's "triangles sample the side neighbour");
 * the shade is that direction's directional shade. */
static void emitTri(DrawList *list, const float quad[4][3], int x, int y, int z,
		    int sideDir, const float uv[4][2], uint8_t alphaMode,
		    uint32_t tint, const LightGrid *lights,
		    const VoxmapEmitOptions *opts, const float debugUV[4][2])
{
	uint8_t light[4][3];
	int ao[4] = { 0, 0, 0, 0 };
	uint32_t cornerTint[4];
	int k;

	sampleLight(lights, x + kSideDx[sideDir], y, z + kSideDz[sideDir],
		    light[0]);
	for (k = 1; k < 4; k++) {
		light[k][0] = light[0][0];
		light[k][1] = light[0][1];
		light[k][2] = light[0][2];
	}
	faceCornerTints(tint, kSideShade[sideDir], x, z, light, ao, opts->smooth,
			opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
}

/* A vertical side face spanning world [y0, y1] (floats: a HALF side is 0.5 high,
 * a ramp back face is `rise` high). Light samples the side neighbour at the
 * floor of each height (a 0.5-high face samples one level, a full face keeps
 * the two-level span). */
static void emitSide(DrawList *list, int x, int z, int dir, float y0, float y1,
		     const float uv[4][2], uint8_t alphaMode, uint32_t tint,
		     const LightGrid *lights, const VoxmapEmitOptions *opts,
		     const float debugUV[4][2])
{
	float fx = (float)x;
	float fz = (float)z;
	float quad[4][3];
	uint8_t light[4][3];
	int ao[4] = { 0, 0, 0, 0 };
	uint32_t cornerTint[4];
	int ly0 = (int)floorf(y0);
	int ly1 = (int)floorf(y1);

	switch (dir) {
	case 0:	/* +Z face at z + 1 */
		quad[0][0] = fx;	quad[0][1] = y0;	quad[0][2] = fz + 1.0f;
		quad[1][0] = fx + 1.0f;	quad[1][1] = y0;	quad[1][2] = fz + 1.0f;
		quad[2][0] = fx + 1.0f;	quad[2][1] = y1;	quad[2][2] = fz + 1.0f;
		quad[3][0] = fx;	quad[3][1] = y1;	quad[3][2] = fz + 1.0f;
		break;
	case 1:	/* +X face at x + 1 */
		quad[0][0] = fx + 1.0f;	quad[0][1] = y0;	quad[0][2] = fz;
		quad[1][0] = fx + 1.0f;	quad[1][1] = y0;	quad[1][2] = fz + 1.0f;
		quad[2][0] = fx + 1.0f;	quad[2][1] = y1;	quad[2][2] = fz + 1.0f;
		quad[3][0] = fx + 1.0f;	quad[3][1] = y1;	quad[3][2] = fz;
		break;
	case 2:	/* -Z face at z */
		quad[0][0] = fx + 1.0f;	quad[0][1] = y0;	quad[0][2] = fz;
		quad[1][0] = fx;	quad[1][1] = y0;	quad[1][2] = fz;
		quad[2][0] = fx;	quad[2][1] = y1;	quad[2][2] = fz;
		quad[3][0] = fx + 1.0f;	quad[3][1] = y1;	quad[3][2] = fz;
		break;
	default: /* 3: -X face at x */
		quad[0][0] = fx;	quad[0][1] = y0;	quad[0][2] = fz + 1.0f;
		quad[1][0] = fx;	quad[1][1] = y0;	quad[1][2] = fz;
		quad[2][0] = fx;	quad[2][1] = y1;	quad[2][2] = fz;
		quad[3][0] = fx;	quad[3][1] = y1;	quad[3][2] = fz + 1.0f;
		break;
	}
	if (opts->smooth)
		sideCorners(lights, x, z, dir, ly0, ly1, light, ao);
	else
		sampleLight(lights, x + kSideDx[dir], ly0, z + kSideDz[dir],
			    light[0]);
	faceCornerTints(tint, kSideShade[dir], x, z, light, ao, opts->smooth,
			opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
}

/* One ramp triangle's geometry: the emitted degenerate quad is [a, b, c, c]. */
typedef struct RampTri {
	int sideDir;	/* VOXMAP side index (0 +Z, 1 +X, 2 -Z, 3 -X) */
	float a[3];
	float b[3];
	float c[3];
} RampTri;

/* The two triangular side faces of a ramp: the planes parallel to its rise.
 * `dir` is the rise direction, `rise` the tall-edge height above y. */
static void rampTriangles(int x, int y, int z, int dir, float rise,
			  RampTri out[2])
{
	float fx = (float)x;
	float fy = (float)y;
	float fz = (float)z;
	float h = fy + rise;

	switch (dir) {
	case VOXMAP_DIR_NORTH:	/* tall at z: triangles at x0 / x1 */
		out[0] = (RampTri){ 3, { fx, fy, fz }, { fx, h, fz },
				    { fx, fy, fz + 1.0f } };
		out[1] = (RampTri){ 1, { fx + 1.0f, fy, fz },
				    { fx + 1.0f, h, fz },
				    { fx + 1.0f, fy, fz + 1.0f } };
		break;
	case VOXMAP_DIR_SOUTH:	/* tall at z+1 */
		out[0] = (RampTri){ 3, { fx, fy, fz + 1.0f },
				    { fx, h, fz + 1.0f }, { fx, fy, fz } };
		out[1] = (RampTri){ 1, { fx + 1.0f, fy, fz + 1.0f },
				    { fx + 1.0f, h, fz + 1.0f },
				    { fx + 1.0f, fy, fz } };
		break;
	case VOXMAP_DIR_WEST:	/* tall at x0: triangles at z0 / z1 */
		out[0] = (RampTri){ 2, { fx, fy, fz }, { fx, h, fz },
				    { fx + 1.0f, fy, fz } };
		out[1] = (RampTri){ 0, { fx, fy, fz + 1.0f },
				    { fx, h, fz + 1.0f },
				    { fx + 1.0f, fy, fz + 1.0f } };
		break;
	default:		/* EAST: tall at x+1 */
		out[0] = (RampTri){ 2, { fx + 1.0f, fy, fz },
				    { fx + 1.0f, h, fz }, { fx, fy, fz } };
		out[1] = (RampTri){ 0, { fx + 1.0f, fy, fz + 1.0f },
				    { fx + 1.0f, h, fz + 1.0f },
				    { fx, fy, fz + 1.0f } };
		break;
	}
}

/* The slope quad (TOP slot): walk the tall edge's two corners, then the low
 * edge's two (see the header's exact per-direction table). */
static void rampSlopeQuad(int x, int y, int z, int dir, float rise,
			  float q[4][3])
{
	float x0 = (float)x;
	float x1 = (float)(x + 1);
	float z0 = (float)z;
	float z1 = (float)(z + 1);
	float yl = (float)y;
	float yh = yl + rise;

	switch (dir) {
	case VOXMAP_DIR_NORTH:	/* tall at z0, low at z1 */
		q[0][0] = x0;	q[0][1] = yh;	q[0][2] = z0;
		q[1][0] = x1;	q[1][1] = yh;	q[1][2] = z0;
		q[2][0] = x1;	q[2][1] = yl;	q[2][2] = z1;
		q[3][0] = x0;	q[3][1] = yl;	q[3][2] = z1;
		break;
	case VOXMAP_DIR_SOUTH:	/* tall at z1 */
		q[0][0] = x0;	q[0][1] = yl;	q[0][2] = z0;
		q[1][0] = x1;	q[1][1] = yl;	q[1][2] = z0;
		q[2][0] = x1;	q[2][1] = yh;	q[2][2] = z1;
		q[3][0] = x0;	q[3][1] = yh;	q[3][2] = z1;
		break;
	case VOXMAP_DIR_WEST:	/* tall at x0 */
		q[0][0] = x0;	q[0][1] = yh;	q[0][2] = z0;
		q[1][0] = x0;	q[1][1] = yh;	q[1][2] = z1;
		q[2][0] = x1;	q[2][1] = yl;	q[2][2] = z1;
		q[3][0] = x1;	q[3][1] = yl;	q[3][2] = z0;
		break;
	default:		/* EAST: tall at x1 */
		q[0][0] = x1;	q[0][1] = yh;	q[0][2] = z0;
		q[1][0] = x1;	q[1][1] = yh;	q[1][2] = z1;
		q[2][0] = x0;	q[2][1] = yl;	q[2][2] = z1;
		q[3][0] = x0;	q[3][1] = yl;	q[3][2] = z0;
		break;
	}
}

/* Emit the non-FULL shape's faces for the voxel (x, y, z) of material `id`.
 * Culling and the UV convention are documented in the header. */
static void emitShapeFaces(const Voxmap *map, DrawList *list, int x, int y,
			   int z, int id, int shape, int dir,
			   const MaterialTable *materials,
			   const LightGrid *lights,
			   const VoxmapEmitOptions *opts,
			   const float debugUV[4][2], float toCamX, float toCamZ)
{
	uint8_t alpha = materialAlpha(materials, id);
	float fy = (float)y;
	float uv[4][2];

	if (shape == VOXMAP_SHAPE_HALF) {
		int d;

		/* The top is at y + 0.5, so a FULL voxel above (base y + 1)
		 * leaves a 0.5 air gap: the top is always visible from a low
		 * side angle and is NEVER culled by the neighbour above (see
		 * the header; only the from-above overdraw is possible). */
		materialUV(materials, id, FACE_TOP, uv);
		emitTop(list, x, z, fy + 0.5f, y + 1, uv, alpha, opts->tint,
			lights, opts, debugUV);
		if (y > 0 && !voxmapFullAt(map, x, y - 1, z)) {
			materialUV(materials, id, FACE_BOTTOM, uv);
			emitBottom(list, x, z, fy, y - 1, uv, alpha, opts->tint,
				   lights, opts, debugUV);
		}
		for (d = 0; d < 4; d++) {
			if (sideCulled(d, toCamX, toCamZ) ||
			    voxmapFullAt(map, x + kSideDx[d], y,
					 z + kSideDz[d]))
				continue;
			materialUV(materials, id, materialFaceForSideDir(d), uv);
			emitSide(list, x, z, d, fy, fy + 0.5f, uv, alpha,
				 opts->tint, lights, opts, debugUV);
		}
		return;
	}

	/* RAMP / HALF_RAMP. */
	{
		float rise = (shape == VOXMAP_SHAPE_RAMP) ? 1.0f : 0.5f;
		int backSide = kDirToSide[dir];
		RampTri tri[2];
		int k;

		if (y > 0 && !voxmapFullAt(map, x, y - 1, z)) {
			materialUV(materials, id, FACE_BOTTOM, uv);
			emitBottom(list, x, z, fy, y - 1, uv, alpha, opts->tint,
				   lights, opts, debugUV);
		}
		if (!voxmapFullAt(map, x + kSideDx[backSide], y,
				  z + kSideDz[backSide]) &&
		    !sideCulled(backSide, toCamX, toCamZ)) {
			materialUV(materials, id, materialFaceForSideDir(backSide),
				   uv);
			emitSide(list, x, z, backSide, fy, fy + rise, uv, alpha,
				 opts->tint, lights, opts, debugUV);
		}
		/* A HALF_RAMP's slope tops out at y + 0.5, so a FULL above
		 * leaves a gap and the slope is never culled by it. A full RAMP
		 * reaches y + 1, meeting the neighbour's base, so it keeps the
		 * cull (see the header). */
		if (shape == VOXMAP_SHAPE_HALF_RAMP ||
		    !voxmapFullAt(map, x, y + 1, z)) {
			float q[4][3];

			rampSlopeQuad(x, y, z, dir, rise, q);
			materialUV(materials, id, FACE_TOP, uv);
			emitSlope(list, q, x, z, y + 1, uv, alpha, opts->tint,
				  lights, opts, debugUV);
		}
		rampTriangles(x, y, z, dir, rise, tri);
		for (k = 0; k < 2; k++) {
			float q[4][3];

			if (sideCulled(tri[k].sideDir, toCamX, toCamZ) ||
			    voxmapFullAt(map, x + kSideDx[tri[k].sideDir], y,
					 z + kSideDz[tri[k].sideDir]))
				continue;
			memcpy(q[0], tri[k].a, sizeof(q[0]));
			memcpy(q[1], tri[k].b, sizeof(q[1]));
			memcpy(q[2], tri[k].c, sizeof(q[2]));
			memcpy(q[3], tri[k].c, sizeof(q[3]));
			materialUV(materials, id,
				   materialFaceForSideDir(tri[k].sideDir), uv);
			/* The degenerate corner mirrors the real third corner so
			 * the zero-area half contributes no UV area. */
			uv[3][0] = uv[2][0];
			uv[3][1] = uv[2][1];
			emitTri(list, q, x, y, z, tri[k].sideDir, uv, alpha,
				opts->tint, lights, opts, debugUV);
		}
	}
}

void voxmapEmitFacesOpt(const Voxmap *map, const MaterialTable *materials,
			const LightGrid *lights, DrawList *list,
			const Camera3D *camera,
			const VoxmapEmitOptions *options)
{
	static const float kFallbackDebug[4][2] = ATLAS_UV_SPARE;
	const float (*debugUV)[2];
	float toCamX;
	float toCamZ;
	int x;
	int z;

	if (map == NULL || list == NULL || options == NULL)
		return;
	debugUV = options->debugUV != NULL ? options->debugUV : kFallbackDebug;
	cameraGroundDir(cameraYawDeg(camera), &toCamX, &toCamZ);
	for (z = 0; z < map->depth; z++) {
		for (x = 0; x < map->width; x++) {
			int y;
			int dir;

			/* The height-0 ground tile has no voxel but still
			 * emits its top face at y = 0. */
			if (map->ground[(size_t)z * map->width + x]) {
				int id = map->materials[voxelIndex(map, x, 0, z)];
				float uv[4][2];

				materialUV(materials, id, FACE_TOP, uv);
				emitTop(list, x, z, 0.0f, 0, uv,
					materialAlpha(materials, id), options->tint,
					lights, options, debugUV);
			}
			/* Tops + bottoms + shape faces, ascending level. */
			for (y = 0; y < map->levels; y++) {
				size_t idx = voxelIndex(map, x, y, z);
				int id;
				int shape;
				float uv[4][2];

				if (!map->solid[idx])
					continue;
				id = map->materials[idx];
				shape = VOXMAP_SHAPE_OF(map->shapes[idx]);
				if (shape != VOXMAP_SHAPE_FULL) {
					emitShapeFaces(map, list, x, y, z, id,
						       shape,
						       VOXMAP_SHAPE_DIR_OF(
							       map->shapes[idx]),
						       materials, lights,
						       options, debugUV,
						       toCamX, toCamZ);
					continue;
				}
				if (!voxmapFullAt(map, x, y + 1, z)) {
					materialUV(materials, id, FACE_TOP, uv);
					emitTop(list, x, z, (float)(y + 1),
						y + 1, uv,
						materialAlpha(materials, id),
						options->tint, lights, options,
						debugUV);
				}
				if (y > 0 && !voxmapFullAt(map, x, y - 1, z)) {
					materialUV(materials, id, FACE_BOTTOM, uv);
					emitBottom(list, x, z, (float)y, y - 1,
						   uv, materialAlpha(materials, id),
						   options->tint, lights, options,
						   debugUV);
				}
			}
			/* Sides: one quad per exposed run of consecutive FULL
			 * voxels (a shape breaks a run; see the header). */
			for (dir = 0; dir < 4; dir++) {
				if (sideCulled(dir, toCamX, toCamZ))
					continue;
				y = 0;
				while (y < map->levels) {
					int start;
					int id;
					float uv[4][2];

					if (!voxmapFullAt(map, x, y, z) ||
					    voxmapFullAt(map,
							  x + kSideDx[dir], y,
							  z + kSideDz[dir])) {
						y++;
						continue;
					}
					start = y;
					id = voxmapMaterialAtVoxel(map, x, y, z);
					while (y < map->levels &&
					       voxmapFullAt(map, x, y, z) &&
					       !voxmapFullAt(map,
							      x + kSideDx[dir], y,
							      z + kSideDz[dir]))
						y++;
					materialUV(materials, id,
						   materialFaceForSideDir(dir),
						   uv);
					emitSide(list, x, z, dir,
						 (float)start, (float)y, uv,
						 materialAlpha(materials, id),
						 options->tint, lights, options,
						 debugUV);
				}
			}
		}
	}
}

void voxmapEmitFaces(const Voxmap *map, const MaterialTable *materials,
		     const LightGrid *lights, DrawList *list,
		     const Camera3D *camera, uint32_t tint)
{
	VoxmapEmitOptions options = { tint, false, false, NULL };

	voxmapEmitFacesOpt(map, materials, lights, list, camera, &options);
}
