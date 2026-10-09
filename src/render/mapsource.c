/*
 * PNG slice map source (see mapsource.h for the format and contract). Pure:
 * no SDL. Diagnostics go to stderr (a pure module cannot use SDL_Log).
 *
 * The assembler writes the occupancy + material arrays in the SAME canonical
 * voxel order voxmap.c uses (x fastest, then y, then z) and hands them to
 * voxmapBuildRaw, so it never reaches into the opaque Voxmap struct. The light
 * grammar is not reimplemented: legend.txt `$` lines go straight through
 * voxmapParseLightLine, so both map formats share one parser.
 */

#include "render/mapsource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Longest legend line we accept (short by construction). */
#define MAPSOURCE_LINE_MAX 256

/* --- small helpers ------------------------------------------------------ */

static bool isDigit(char c)
{
	return c >= '0' && c <= '9';
}

static int hexNibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Split a mutable line on spaces/tabs; returns the token count (tokens beyond
 * `max` are counted but not stored, matching voxmap.c's tokenize). */
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

/* Parse a "#RRGGBB" token (exactly 7 chars, case-insensitive hex). The caller
 * only passes a token that starts with '#' (it comes from a '#'-prefixed
 * line), so only the length and the hex digits need checking. */
static bool parseHexColor(const char *tok, uint32_t *out)
{
	uint32_t v = 0;
	int i;

	if (strlen(tok) != 7)
		return false;
	for (i = 1; i <= 6; i++) {
		int nib = hexNibble(tok[i]);

		if (nib < 0)
			return false;
		v = (v << 4) | (uint32_t)nib;
	}
	*out = v;
	return true;
}

/* A display label for diagnostics ("<map>" when the caller passed NULL). */
static const char *mapLabel(const char *label)
{
	return label != NULL ? label : "<map>";
}

/* --- natural sort ------------------------------------------------------- */

int mapSourceNaturalCompare(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b ? 0 : (a == NULL ? 1 : -1);
	while (*a != '\0' && *b != '\0') {
		if (isDigit(*a) && isDigit(*b)) {
			const char *za = a;
			const char *zb = b;
			const char *da;
			const char *db;
			size_t la;
			size_t lb;
			int cmp;

			while (*za == '0')
				za++;
			while (*zb == '0')
				zb++;
			da = za;
			db = zb;
			while (isDigit(*da))
				da++;
			while (isDigit(*db))
				db++;
			la = (size_t)(da - za);
			lb = (size_t)(db - zb);
			if (la != lb)
				return la < lb ? -1 : 1;
			cmp = memcmp(za, zb, la);
			if (cmp != 0)
				return cmp < 0 ? -1 : 1;
			/* Equal numeric value: fewer leading zeros first. */
			if ((size_t)(za - a) != (size_t)(zb - b))
				return (za - a) < (zb - b) ? -1 : 1;
			a = da;
			b = db;
			continue;
		}
		if ((unsigned char)*a != (unsigned char)*b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++;
		b++;
	}
	if (*a != '\0')
		return 1;
	if (*b != '\0')
		return -1;
	return 0;
}

/* --- colour lookup ------------------------------------------------------ */

int mapSourceColorLookup(const MapSourceLegend *legend, uint32_t rgb)
{
	int i;

	if (legend == NULL)
		return -1;
	for (i = 0; i < legend->colorCount; i++)
		if (legend->colors[i].rgb == rgb)
			return legend->colors[i].material;
	return -1;
}

/* --- legend parse ------------------------------------------------------- */

/* Resolve a legend material name; -1 (air) with a diagnostic when unknown.
 * A NULL table resolves everything to id 0, like the ASCII parser. */
static int resolveMaterial(const MaterialTable *materials, const char *name,
			   MapSourceLegend *out)
{
	int id;

	if (materials == NULL)
		return 0;
	id = materialIdByName(materials, name);
	if (id < 0) {
		fprintf(stderr,
			"mapsource: legend material '%s' unknown; colour treated as air\n",
			name);
		out->unknownMaterials++;
		return -1;
	}
	return id;
}

/* Add one colour line (tokens[0] = "#RRGGBB", tokens[1] = material). */
static void addColorLine(MapSourceLegend *out, const MaterialTable *materials,
			 char *const *tokens)
{
	uint32_t rgb;
	int id;
	int i;

	if (!parseHexColor(tokens[0], &rgb)) {
		fprintf(stderr, "mapsource: bad colour '%s'; line skipped\n",
			tokens[0]);
		out->badLines++;
		return;
	}
	id = resolveMaterial(materials, tokens[1], out);
	if (id < 0)
		return;
	for (i = 0; i < out->colorCount; i++) {
		if (out->colors[i].rgb == rgb) {
			fprintf(stderr,
				"mapsource: duplicate colour #%06X; last wins\n",
				(unsigned)rgb);
			out->colors[i].material = (int16_t)id;
			out->duplicateColors++;
			return;
		}
	}
	if (out->colorCount >= MAPSOURCE_MAX_COLORS) {
		fprintf(stderr, "mapsource: more than %d legend colours; skipped\n",
			MAPSOURCE_MAX_COLORS);
		out->overflowColors++;
		return;
	}
	out->colors[out->colorCount].rgb = rgb;
	out->colors[out->colorCount].material = (int16_t)id;
	out->colorCount++;
}

bool mapSourceLegendParse(const char *text, size_t length,
			  const MaterialTable *materials,
			  MapSourceLegend *out)
{
	size_t pos = 0;

	if (text == NULL || out == NULL)
		return false;
	memset(out, 0, sizeof(*out));
	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		size_t i;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;
		rowLen = end - start;
		while (rowLen > 0 && text[start + rowLen - 1] == '\r')
			rowLen--;
		i = start;
		while (i < start + rowLen &&
		       (text[i] == ' ' || text[i] == '\t'))
			i++;
		if (i >= start + rowLen)
			continue;	/* blank line */
		if (text[i] == '$') {
			VoxmapLight l;

			if (!voxmapParseLightLine(text + i,
						  (start + rowLen) - i, &l)) {
				out->badLines++;
				continue;
			}
			if (out->lightCount >= VOXMAP_MAX_LIGHTS) {
				fprintf(stderr,
					"mapsource: more than %d lights; skipped\n",
					VOXMAP_MAX_LIGHTS);
				out->overflowLights++;
				continue;
			}
			out->lights[out->lightCount++] = l;
			continue;
		}
		if (text[i] == '#') {
			char line[MAPSOURCE_LINE_MAX];
			char *tokens[4];
			size_t len = (start + rowLen) - i;
			int ntok;

			if (len >= sizeof(line)) {
				fprintf(stderr,
					"mapsource: legend line too long; skipped\n");
				out->badLines++;
				continue;
			}
			memcpy(line, text + i, len);
			line[len] = '\0';
			ntok = tokenize(line, tokens, 4);
			if (ntok != 2) {
				fprintf(stderr,
					"mapsource: colour line needs '#RRGGBB material' (%d tokens); skipped\n",
					ntok);
				out->badLines++;
				continue;
			}
			addColorLine(out, materials, tokens);
			continue;
		}
		fprintf(stderr, "mapsource: unrecognised legend line; skipped\n");
		out->badLines++;
	}
	return true;
}

/* --- slice assembly ----------------------------------------------------- */

Voxmap *mapSourceAssembleSlices(const MapSourceImage *images, int count,
				const MapSourceLegend *legend, const char *label,
				MapSourceStats *outStats)
{
	uint8_t *solid;
	int16_t *mats;
	Voxmap *map;
	int width;
	int height;
	int levels;
	size_t n;
	int unknown = 0;
	int solidCount = 0;
	int s;
	int x;
	int z;

	if (outStats != NULL) {
		outStats->levels = 0;
		outStats->solidVoxels = 0;
		outStats->unknownColorVoxels = 0;
	}
	if (images == NULL || legend == NULL || count < 1) {
		fprintf(stderr, "mapsource: '%s' no slices\n",
			mapLabel(label));
		return NULL;
	}
	width = images[0].width;
	height = images[0].height;
	levels = count;
	if (width < 1 || height < 1 || width > VOXMAP_MAX_DIM ||
	    height > VOXMAP_MAX_DIM || levels > VOXMAP_MAX_DIM) {
		fprintf(stderr, "mapsource: '%s' bad slice size %dx%d x %d\n",
			mapLabel(label), width, height, levels);
		return NULL;
	}
	for (s = 0; s < levels; s++) {
		if (images[s].rgba == NULL) {
			fprintf(stderr, "mapsource: '%s' slice %d has no pixels\n",
				mapLabel(label), s);
			return NULL;
		}
		if (images[s].width != width || images[s].height != height) {
			fprintf(stderr,
				"mapsource: '%s' slice %d is %dx%d, expected %dx%d\n",
				mapLabel(label), s,
				images[s].width, images[s].height, width,
				height);
			return NULL;
		}
		if (images[s].pitch < (size_t)width * 4u) {
			fprintf(stderr,
				"mapsource: '%s' slice %d pitch too small\n",
				mapLabel(label), s);
			return NULL;
		}
	}
	n = (size_t)width * (size_t)height * (size_t)levels;
	if (n > VOXMAP_MAX_CELLS) {
		fprintf(stderr, "mapsource: '%s' map too large (%zux%dx%d)\n",
			mapLabel(label), (size_t)width,
			height, levels);
		return NULL;
	}
	solid = calloc(n, 1);
	mats = malloc(n * sizeof(*mats));
	if (solid == NULL || mats == NULL) {
		free(solid);
		free(mats);
		fprintf(stderr, "mapsource: '%s' out of memory\n",
			mapLabel(label));
		return NULL;
	}
	memset(mats, 0xFF, n * sizeof(*mats));	/* -1 = air */

	for (s = 0; s < levels; s++) {
		for (z = 0; z < height; z++) {
			const uint8_t *row =
				images[s].rgba + (size_t)z * images[s].pitch;

			for (x = 0; x < width; x++) {
				const uint8_t *px = row + (size_t)x * 4u;
				size_t idx = ((size_t)z * (size_t)levels +
					      (size_t)s) *
						     (size_t)width +
					     (size_t)x;
				int mat;

				if (px[3] == 0)
					continue;	/* transparent = air */
				mat = mapSourceColorLookup(
					legend,
					((uint32_t)px[0] << 16) |
						((uint32_t)px[1] << 8) |
						(uint32_t)px[2]);
				if (mat < 0) {
					unknown++;
					continue;	/* unknown colour = air */
				}
				solid[idx] = 1;
				mats[idx] = (int16_t)mat;
				solidCount++;
			}
		}
	}

	map = voxmapBuildRaw(width, height, levels, solid, mats, legend->lights,
			     legend->lightCount);
	free(solid);
	free(mats);
	if (map == NULL) {
		fprintf(stderr, "mapsource: '%s' voxmap build failed\n",
			mapLabel(label));
		return NULL;
	}
	if (unknown > 0)
		fprintf(stderr,
			"mapsource: '%s' %d voxel%s used a colour absent from the legend; treated as air\n",
			mapLabel(label), unknown,
			unknown == 1 ? "" : "s");
	if (outStats != NULL) {
		outStats->levels = levels;
		outStats->solidVoxels = solidCount;
		outStats->unknownColorVoxels = unknown;
	}
	return map;
}
