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
 * Parsing is two passes over the whole buffer: the first validates every row
 * and pins width/depth, the second fills the cell grid. Rows are split on
 * '\n'; a trailing '\r' and trailing spaces/tabs are stripped, and blank
 * lines are skipped, so CRLF files and trailing newlines are fine.
 */

#include "render/voxmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Voxmap {
	int width;
	int depth;
	int8_t *cells;	/* width * depth; -1 = void, 0..9 = column height */
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

/* Parse `length` bytes of heightmap text. The buffer need not be
 * NUL-terminated: every scan is bounded by `length`, so a slice of a larger
 * buffer parses exactly its own bytes and never reads past text + length.
 * `label` names the source in stderr diagnostics. Returns NULL on any
 * malformed input (bad cell, ragged/over-large/empty map, allocation
 * failure) or a NULL text pointer. */
static Voxmap *parseVoxmap(const char *text, size_t length, const char *label)
{
	Voxmap *map;
	int width = -1;
	int depth = 0;
	size_t pos = 0;

	if (text == NULL)
		return NULL;

	/* Pass 1: validate rows, pin width, count depth. */
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
		if (rowLen == 0)
			continue;		/* blank line */
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
			char c = text[i];

			if (c != '.' && (c < '0' || c > '9')) {
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
	if (map->cells == NULL) {
		free(map);
		return NULL;
	}
	map->width = width;
	map->depth = depth;

	/* Pass 2: fill the grid. */
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
			if (trimmedLength(text, start, end) == 0)
				continue;
			for (col = 0; col < width; col++) {
				char c = text[start + (size_t)col];

				map->cells[(size_t)row * width + col] =
					(c == '.') ? -1 : (int8_t)(c - '0');
			}
			row++;
		}
	}

	return map;
}

Voxmap *parseVoxmapText(const char *text, size_t length)
{
	return parseVoxmap(text, length, "<memory>");
}

Voxmap *loadVoxmap(const char *path)
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
	map = parseVoxmap(data, size, path);
	free(data);
	return map;
}

void destroyVoxmap(Voxmap *map)
{
	if (map == NULL)
		return;
	free(map->cells);
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

/* --- face generation --------------------------------------------------- */

/* Side directions: 0 = +Z, 1 = +X, 2 = -Z, 3 = -X. */
static const int kSideDx[4] = { 0, 1, 0, -1 };
static const int kSideDz[4] = { 1, 0, -1, 0 };

/* One-time overflow diagnostic: appendVoxelFace returns false when the draw
 * list is full, and the face is silently dropped by design (a bounded list
 * must not grow mid-frame). Report the first drop so vanishing geometry is
 * diagnosable; later drops stay quiet to avoid per-face log spam. */
static bool g_faceOverflowReported = false;

static void emitFace(DrawList *list, const float quad[4][3], DrawFace face,
		     uint32_t tint)
{
	if (!appendVoxelFace(list, quad, face, tint) && !g_faceOverflowReported) {
		g_faceOverflowReported = true;
		fprintf(stderr,
			"voxmap: draw list full (%zu items); faces dropped (reported once)\n",
			drawListCount(list));
	}
}

/* The single axis-aligned side facing the camera, or -1 mid-tween. The camera
 * yaw quarter index maps directly to the side direction (yaw 0 sees +Z, 90
 * sees +X, 180 sees -Z, 270 sees -X). */
static int facingSide(float yawDeg)
{
	float quarters = yawDeg / 90.0f;
	float nearest = roundf(quarters);
	int side;

	if (fabsf(quarters - nearest) > 1e-3f)
		return -1;
	side = (int)nearest % 4;
	if (side < 0)
		side += 4;
	return side;
}

static void emitTop(DrawList *list, int x, int z, int height, uint32_t tint)
{
	float quad[4][3] = {
		{ (float)x, (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)z },
		{ (float)(x + 1), (float)height, (float)(z + 1) },
		{ (float)x, (float)height, (float)(z + 1) },
	};

	emitFace(list, quad, DRAW_FACE_TOP, tint);
}
static void emitSide(DrawList *list, int x, int z, int dir, int y0, int y1,
		     uint32_t tint)
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
	emitFace(list, quad, DRAW_FACE_SIDE, tint);
}

void voxmapEmitFaces(const Voxmap *map, DrawList *list, const Camera3D *camera,
		     uint32_t tint)
{
	int facing;
	int x;
	int z;
	int dir;

	if (map == NULL || list == NULL)
		return;
	facing = facingSide(cameraYawDeg(camera));
	for (z = 0; z < map->depth; z++) {
		for (x = 0; x < map->width; x++) {
			int height = map->cells[(size_t)z * map->width + x];

			if (height < 0)
				continue;	/* void: no column, no faces */
			emitTop(list, x, z, height, tint);
			for (dir = 0; dir < 4; dir++) {
				int neighbour;

				if (facing >= 0 && dir != facing)
					continue;	/* cull away-facing sides */
				neighbour = voxmapHeightAt(
					map, x + kSideDx[dir], z + kSideDz[dir]);
				if (neighbour < 0)
					neighbour = 0;	/* void/OOB = ground */
				if (neighbour >= height)
					continue;	/* not exposed */
				emitSide(list, x, z, dir, neighbour, height, tint);
			}
		}
	}
}
