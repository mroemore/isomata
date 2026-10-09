/*
 * Voxmap (see voxmap.h for the model, format and query contract). Pure: file
 * I/O and a small malloc, no SDL. Diagnostics go to stderr (a pure module
 * cannot use SDL_Log).
 *
 * Storage is an occupancy + per-voxel material grid: solid[] is one byte per
 * voxel, materials[] one int16 per voxel (index order x fastest, then y, then
 * z: index = ((z * levels) + y) * width + x, matching lightgrid.c). A parallel
 * per-column ground[] byte marks the single-section height-0 degenerate (a
 * solid flat ground tile with no voxel). Single-section files assign the
 * column's material to every voxel; slice files assign per voxel.
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
			   int16_t defaultMat)
{
	int c;

	for (c = 0; c < LEGEND_CHARS; c++) {
		h[c] = LEGEND_INVALID;
		m[c] = defaultMat;
	}
	h[(unsigned char)'.'] = -1;
	for (c = '0'; c <= '9'; c++)
		h[c] = (int8_t)(c - '0');
}

/* Parse one `@ <char> <height> <material>` line into the legend table. */
static bool parseLegend(const char *data, size_t start, size_t len,
			const char *label, int8_t h[LEGEND_CHARS],
			int16_t m[LEGEND_CHARS], const MaterialTable *materials,
			int16_t defaultMat)
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
	int16_t defaultMat;
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount;
} ParseCtx;

/* Allocate a zeroed map of the given dimensions with every voxel air
 * (material -1). NULL on an over-large volume or OOM. Precondition (both
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
	map->ground = malloc((size_t)width * (size_t)depth);
	if (map->solid == NULL || map->materials == NULL || map->ground == NULL) {
		free(map->solid);
		free(map->materials);
		free(map->ground);
		free(map);
		return NULL;
	}
	memset(map->solid, 0, n);
	memset(map->materials, 0xFF, n * sizeof(*map->materials));	/* -1 */
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
			int y;

			if (h < 0)
				continue;	/* void */
			if (h == 0) {
				map->ground[(size_t)row * width + col] = 1;
				map->materials[voxelIndex(map, col, 0, row)] = id;
				continue;
			}
			for (y = 0; y < h; y++) {
				size_t idx = voxelIndex(map, col, y, row);

				map->solid[idx] = 1;
				map->materials[idx] = id;
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
				} else {
					map->solid[idx] = 1;
					map->materials[idx] =
						ctx->legendM[c];
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
	legendDefaults(ctx.legendH, ctx.legendM, ctx.defaultMat);

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
					 ctx.legendH, ctx.legendM, materials,
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

Voxmap *voxmapBuildRaw(int width, int depth, int levels,
		       const uint8_t *solid, const int16_t *materials,
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
	if (lights != NULL && lightCount > 0) {
		int keep = lightCount < VOXMAP_MAX_LIGHTS ? lightCount
							  : VOXMAP_MAX_LIGHTS;

		memcpy(map->lights, lights, (size_t)keep * sizeof(*lights));
		map->lightCount = keep;
	}
	return map;
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

static void emitTop(DrawList *list, int x, int z, int height, const float uv[4][2],
		    uint8_t alphaMode, uint32_t tint, const LightGrid *lights,
		    const VoxmapEmitOptions *opts, const float debugUV[4][2])
{
	float quad[4][3] = {
		{ (float)x, (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)(z + 1) },
		{ (float)x, (float)height, (float)(z + 1) },
	};
	uint8_t light[4][3];
	int ao[4] = { 0, 0, 0, 0 };
	uint32_t cornerTint[4];

	if (opts->smooth)
		topCorners(lights, x, z, height, light, ao);
	else
		sampleLight(lights, x, height, z, light[0]);
	faceCornerTints(tint, VOXMAP_SHADE_TOP, x, z, light, ao, opts->smooth,
			opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
}

/* Bottom face of the solid voxel at level `y`: the same x/z quad as the top,
 * at world y, sampling the air cell below (y - 1). Shade is the darkest face
 * (VOXMAP_SHADE_BOTTOM). */
static void emitBottom(DrawList *list, int x, int z, int y,
		       const float uv[4][2], uint8_t alphaMode, uint32_t tint,
		       const LightGrid *lights, const VoxmapEmitOptions *opts,
		       const float debugUV[4][2])
{
	float quad[4][3] = {
		{ (float)x, (float)y, (float)z },
		{ (float)(x + 1), (float)y, (float)z },
		{ (float)(x + 1), (float)y, (float)(z + 1) },
		{ (float)x, (float)y, (float)(z + 1) },
	};
	uint8_t light[4][3];
	int ao[4] = { 0, 0, 0, 0 };
	uint32_t cornerTint[4];

	if (opts->smooth)
		topCorners(lights, x, z, y - 1, light, ao);
	else
		sampleLight(lights, x, y - 1, z, light[0]);
	faceCornerTints(tint, VOXMAP_SHADE_BOTTOM, x, z, light, ao,
			opts->smooth, opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
}

static void emitSide(DrawList *list, int x, int z, int dir, int y0, int y1,
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

	switch (dir) {
	case 0:	/* +Z face at z + 1 */
		quad[0][0] = fx;	quad[0][1] = (float)y0;	quad[0][2] = fz + 1.0f;
		quad[1][0] = fx + 1.0f;	quad[1][1] = (float)y0;	quad[1][2] = fz + 1.0f;
		quad[2][0] = fx + 1.0f;	quad[2][1] = (float)y1;	quad[2][2] = fz + 1.0f;
		quad[3][0] = fx;	quad[3][1] = (float)y1;	quad[3][2] = fz + 1.0f;
		break;
	case 1:	/* +X face at x + 1 */
		quad[0][0] = fx + 1.0f;	quad[0][1] = (float)y0;	quad[0][2] = fz;
		quad[1][0] = fx + 1.0f;	quad[1][1] = (float)y0;	quad[1][2] = fz + 1.0f;
		quad[2][0] = fx + 1.0f;	quad[2][1] = (float)y1;	quad[2][2] = fz + 1.0f;
		quad[3][0] = fx + 1.0f;	quad[3][1] = (float)y1;	quad[3][2] = fz;
		break;
	case 2:	/* -Z face at z */
		quad[0][0] = fx + 1.0f;	quad[0][1] = (float)y0;	quad[0][2] = fz;
		quad[1][0] = fx;	quad[1][1] = (float)y0;	quad[1][2] = fz;
		quad[2][0] = fx;	quad[2][1] = (float)y1;	quad[2][2] = fz;
		quad[3][0] = fx + 1.0f;	quad[3][1] = (float)y1;	quad[3][2] = fz;
		break;
	default: /* 3: -X face at x */
		quad[0][0] = fx;	quad[0][1] = (float)y0;	quad[0][2] = fz + 1.0f;
		quad[1][0] = fx;	quad[1][1] = (float)y0;	quad[1][2] = fz;
		quad[2][0] = fx;	quad[2][1] = (float)y1;	quad[2][2] = fz;
		quad[3][0] = fx;	quad[3][1] = (float)y1;	quad[3][2] = fz + 1.0f;
		break;
	}
	if (opts->smooth)
		sideCorners(lights, x, z, dir, y0, y1, light, ao);
	else
		sampleLight(lights, x + kSideDx[dir], y0, z + kSideDz[dir],
			    light[0]);
	faceCornerTints(tint, kSideShade[dir], x, z, light, ao, opts->smooth,
			opts->lightDebug, cornerTint);
	emitFace(list, quad, opts->lightDebug ? debugUV : uv,
		 opts->lightDebug ? (uint8_t)ALPHA_OPAQUE : alphaMode,
		 cornerTint);
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
				emitTop(list, x, z, 0, uv,
					materialAlpha(materials, id), options->tint,
					lights, options, debugUV);
			}
			/* Tops + bottoms, ascending level. */
			for (y = 0; y < map->levels; y++) {
				size_t idx = voxelIndex(map, x, y, z);
				int id;
				float uv[4][2];

				if (!map->solid[idx])
					continue;
				id = map->materials[idx];
				if (!voxmapSolidAt(map, x, y + 1, z)) {
					materialUV(materials, id, FACE_TOP, uv);
					emitTop(list, x, z, y + 1, uv,
						materialAlpha(materials, id),
						options->tint, lights, options,
						debugUV);
				}
				if (y > 0 && !voxmapSolidAt(map, x, y - 1, z)) {
					materialUV(materials, id, FACE_BOTTOM, uv);
					emitBottom(list, x, z, y, uv,
						   materialAlpha(materials, id),
						   options->tint, lights, options,
						   debugUV);
				}
			}
			/* Sides: one quad per exposed vertical run. */
			for (dir = 0; dir < 4; dir++) {
				if (sideCulled(dir, toCamX, toCamZ))
					continue;
				y = 0;
				while (y < map->levels) {
					int start;
					int id;
					float uv[4][2];

					if (!voxmapSolidAt(map, x, y, z) ||
					    voxmapSolidAt(map,
							  x + kSideDx[dir], y,
							  z + kSideDz[dir])) {
						y++;
						continue;
					}
					start = y;
					id = voxmapMaterialAtVoxel(map, x, y, z);
					while (y < map->levels &&
					       voxmapSolidAt(map, x, y, z) &&
					       !voxmapSolidAt(map,
							      x + kSideDx[dir], y,
							      z + kSideDz[dir]))
						y++;
					materialUV(materials, id,
						   materialFaceForSideDir(dir),
						   uv);
					emitSide(list, x, z, dir, start, y, uv,
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
