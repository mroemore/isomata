/*
 * Voxmap (see voxmap.h for the format and query contract). Pure: file I/O and
 * a small malloc, no SDL. Diagnostics go to stderr (a pure module cannot use
 * SDL_Log).
 *
 * Two entry points share one parser: parseVoxmapText() parses an in-memory
 * buffer (the SDL tier uses it for APK assets, which are not filesystem
 * files), and loadVoxmap() is the desktop/test convenience that reads a file
 * and parses it. The parser never assumes NUL termination — every scan is
 * bounded by the caller's length — so a slice of a larger buffer is safe.
 *
 * Parsing is three passes over the whole buffer: a legend pass collects the
 * `@ <char> <height> <material>` lines into a char -> (height, material)
 * table, a validation pass pins width/depth and rejects unknown cells, and a
 * fill pass writes the cell grid and its parallel material ids. Rows are split
 * on '\n'; a trailing '\r' and trailing spaces/tabs are stripped, and blank
 * and legend lines are skipped, so CRLF files and trailing newlines are fine.
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
	int8_t *cells;		/* width * depth; -1 = void, 0..9 = height */
	int16_t *materials;	/* width * depth; material id, -1 = void */
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount;
};

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

/* Parse `length` bytes of heightmap text (see voxmap.h). */
static Voxmap *parseVoxmap(const char *text, size_t length, const char *label,
			   const MaterialTable *materials)
{
	Voxmap *map;
	int width = -1;
	int depth = 0;
	size_t pos = 0;
	int8_t legendH[LEGEND_CHARS];
	int16_t legendM[LEGEND_CHARS];
	int16_t defaultMat = 0;
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount = 0;

	if (text == NULL)
		return NULL;
	if (materials != NULL) {
		int d = materialIdByName(materials, "default");

		if (d >= 0)
			defaultMat = (int16_t)d;
	}
	legendDefaults(legendH, legendM, defaultMat);

	/* Pass 0: collect legend lines. */
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
			if (!parseLegend(text, start, rowLen, label, legendH,
					 legendM, materials, defaultMat))
				return NULL;
		} else if (isLightLine(text, start, rowLen)) {
			parseLightLine(lights, &lightCount, text, start, rowLen,
				       label);
		}
	}

	/* Pass 1: validate map rows, pin width, count depth. */
	pos = 0;
	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		size_t i;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;			/* skip '\n' */
		rowLen = trimmedLength(text, start, end);
		if (rowLen == 0 || isDirectiveLine(text, start, rowLen))
			continue;
		if (width < 0) {
			if (rowLen > VOXMAP_MAX_DIM) {
				fprintf(stderr,
					"voxmap: '%s' row too wide (%zu > %d)\n",
					label, rowLen, VOXMAP_MAX_DIM);
				return NULL;
			}
			width = (int)rowLen;
		} else if ((int)rowLen != width) {
			fprintf(stderr,
				"voxmap: '%s' ragged row %d (%zu chars, expected %d)\n",
				label, depth + 1, rowLen, width);
			return NULL;
		}
		for (i = start; i < start + rowLen; i++) {
			unsigned char c = (unsigned char)text[i];

			if (c >= LEGEND_CHARS || legendH[c] == LEGEND_INVALID) {
				fprintf(stderr,
					"voxmap: '%s' invalid cell '%c' at row %d\n",
					label, (c >= 32 && c < 127) ? c : '?',
					depth + 1);
				return NULL;
			}
		}
		depth++;
		if (depth > VOXMAP_MAX_DIM) {
			fprintf(stderr, "voxmap: '%s' too many rows (> %d)\n",
				label, VOXMAP_MAX_DIM);
			return NULL;
		}
	}
	if (width <= 0 || depth <= 0) {
		fprintf(stderr, "voxmap: '%s' is empty\n", label);
		return NULL;
	}

	map = calloc(1, sizeof(*map));
	if (map == NULL)
		return NULL;
	map->cells = malloc((size_t)width * (size_t)depth);
	map->materials = malloc((size_t)width * (size_t)depth *
				sizeof(*map->materials));
	if (map->cells == NULL || map->materials == NULL) {
		free(map->cells);
		free(map->materials);
		free(map);
		return NULL;
	}
	map->width = width;
	map->depth = depth;
	if (lightCount > 0)
		memcpy(map->lights, lights,
		       (size_t)lightCount * sizeof(*lights));
	map->lightCount = lightCount;

	/* Pass 2: fill the grid and its material ids. */
	{
		int row = 0;

		pos = 0;
		while (pos < length && row < depth) {
			size_t start = pos;
			size_t end;
			int col;

			while (pos < length && text[pos] != '\n')
				pos++;
			end = pos;
			if (pos < length)
				pos++;
			if (trimmedLength(text, start, end) == 0 ||
			    isDirectiveLine(text, start,
					    trimmedLength(text, start, end)))
				continue;
			for (col = 0; col < width; col++) {
				unsigned char c =
					(unsigned char)text[start + (size_t)col];
				int8_t h = legendH[c];
				size_t idx = (size_t)row * width + col;

				map->cells[idx] = h;
				map->materials[idx] =
					(h < 0) ? -1 : legendM[c];
			}
			row++;
		}
	}

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
	free(map->cells);
	free(map->materials);
	free(map);
}

int voxmapWidth(const Voxmap *map)
{
	return map == NULL ? 0 : map->width;
}

int voxmapDepth(const Voxmap *map)
{
	return map == NULL ? 0 : map->depth;
}

int voxmapHeightAt(const Voxmap *map, int x, int y)
{
	if (map == NULL || x < 0 || y < 0 || x >= map->width || y >= map->depth)
		return -1;
	return map->cells[(size_t)y * map->width + x];
}

bool voxmapIsVoid(const Voxmap *map, int x, int y)
{
	return voxmapHeightAt(map, x, y) < 0;
}

int voxmapMaterialAt(const Voxmap *map, int x, int y)
{
	if (map == NULL || x < 0 || y < 0 || x >= map->width || y >= map->depth)
		return -1;
	return map->materials[(size_t)y * map->width + x];
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
	if (materials != NULL && (size_t)id < materials->count)
		materialFaceUV(&materials->items[id].rect[face], face, uv);
	else
		memcpy(uv, (face == FACE_TOP) ? kFallbackTop : kFallbackSide,
		       sizeof(kFallbackTop));
}

static uint8_t materialAlpha(const MaterialTable *materials, int id)
{
	if (materials != NULL && (size_t)id < materials->count)
		return (uint8_t)materials->items[id].alpha;
	return (uint8_t)ALPHA_BLEND;
}

static void emitFace(DrawList *list, const float quad[4][3],
		     const float uv[4][2], uint8_t alphaMode, uint32_t tint)
{
	if (!appendVoxelFace(list, quad, uv, alphaMode, tint) &&
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

/* Apply the face shade, the per-column checker boost and the sampled light
 * factor to the caller tint: RGB multiplied and clamped per channel, alpha
 * preserved. `x`/`z` are the column's grid coordinates (odd tiles are
 * brightened); `light` is the 0..255 per-channel factor (255 = full). */
static uint32_t shadeTint(uint32_t tint, float faceShade, int x, int z,
			  const uint8_t light[3])
{
	float base = faceShade *
		     (((x + z) & 1) ? VOXMAP_CHECKER_BOOST : 1.0f);
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
		    uint8_t alphaMode, uint32_t tint, const uint8_t light[3])
{
	float quad[4][3] = {
		{ (float)x, (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)(z + 1) },
		{ (float)x, (float)height, (float)(z + 1) },
	};

	emitFace(list, quad, uv, alphaMode,
		 shadeTint(tint, VOXMAP_SHADE_TOP, x, z, light));
}

static void emitSide(DrawList *list, int x, int z, int dir, int y0, int y1,
		     const float uv[4][2], uint8_t alphaMode, uint32_t tint,
		     const uint8_t light[3])
{
	float fx = (float)x;
	float fz = (float)z;
	float quad[4][3];

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
	emitFace(list, quad, uv, alphaMode,
		 shadeTint(tint, kSideShade[dir], x, z, light));
}

void voxmapEmitFaces(const Voxmap *map, const MaterialTable *materials,
		     const LightGrid *lights, DrawList *list,
		     const Camera3D *camera, uint32_t tint)
{
	float toCamX;
	float toCamZ;
	int x;
	int z;
	int dir;

	if (map == NULL || list == NULL)
		return;
	cameraGroundDir(cameraYawDeg(camera), &toCamX, &toCamZ);
	for (z = 0; z < map->depth; z++) {
		for (x = 0; x < map->width; x++) {
			size_t idx = (size_t)z * map->width + x;
			int height = map->cells[idx];
			int id;
			float uv[4][2];
			uint8_t alphaMode;
			uint8_t light[3];

			if (height < 0)
				continue;	/* void: no column, no faces */
			id = map->materials[idx];
			alphaMode = materialAlpha(materials, id);
			materialUV(materials, id, FACE_TOP, uv);
			sampleLight(lights, x, height, z, light);
			emitTop(list, x, z, height, uv, alphaMode, tint, light);
			for (dir = 0; dir < 4; dir++) {
				int neighbour;

				if (sideCulled(dir, toCamX, toCamZ))
					continue;	/* cull away-facing sides */
				neighbour = voxmapHeightAt(
					map, x + kSideDx[dir], z + kSideDz[dir]);
				if (neighbour < 0)
					neighbour = 0;	/* void/OOB = ground */
				if (neighbour >= height)
					continue;	/* not exposed */
				materialUV(materials, id,
					   materialFaceForSideDir(dir), uv);
				sampleLight(lights, x + kSideDx[dir], neighbour,
					    z + kSideDz[dir], light);
				emitSide(list, x, z, dir, neighbour, height, uv,
					 alphaMode, tint, light);
			}
		}
	}
}
