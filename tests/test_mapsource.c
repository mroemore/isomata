/*
 * Mapsource tests (CTOL rung 1: unit + boundary).
 *
 * Pins the pure PNG slice map source: natural-sort file ordering, the
 * colour -> material legend parser (case-insensitive hex, duplicate last-wins,
 * malformed lines skipped, `$` lights reused verbatim) and slice assembly into
 * a Voxmap (alpha 0 = air, unknown colour = air + counted, mismatched sizes /
 * zero slices / over-large volumes fail cleanly). Also covers the voxmap.c
 * entry points the glue/assembler rely on (voxmapParseLightLine,
 * voxmapBuildRaw).
 *
 * Pure: links only the pure render tier + the Unity subset. Harness
 * convention: no main()/setUp()/tearDown(); exposes run_test_mapsource().
 */

#include "unity.h"

#include "render/materials.h"
#include "render/mapsource.h"
#include "render/voxmap.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* --- fixtures ---------------------------------------------------------- */

typedef struct Rgba {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} Rgba;

static const Rgba C_GRASS = { 0x00, 0xFF, 0x00, 0xFF };
static const Rgba C_STONE = { 0x80, 0x80, 0x80, 0xFF };
static const Rgba C_AIR = { 0, 0, 0, 0 };
static const Rgba C_RED = { 0xFF, 0x00, 0x00, 0xFF };

/* default = 0, grass = 1, stone = 2, wood = 3. */
static void buildTable(MaterialTable *t)
{
	memset(t, 0, sizeof(*t));
	materialTableAdd(t, "default", ALPHA_OPAQUE);
	materialTableAdd(t, "grass", ALPHA_CUTOUT);
	materialTableAdd(t, "stone", ALPHA_OPAQUE);
	materialTableAdd(t, "wood", ALPHA_OPAQUE);
}

/* Copy a row-major pixel grid into `buf` and fill `img`. */
static void makeImage(MapSourceImage *img, uint8_t *buf, int w, int h,
		      const Rgba *px)
{
	int i;

	for (i = 0; i < w * h; i++) {
		buf[i * 4 + 0] = px[i].r;
		buf[i * 4 + 1] = px[i].g;
		buf[i * 4 + 2] = px[i].b;
		buf[i * 4 + 3] = px[i].a;
	}
	img->width = w;
	img->height = h;
	img->rgba = buf;
	img->pitch = (size_t)w * 4u;
}

/* --- natural sort ------------------------------------------------------ */

static void test_natural_sort_basic(void)
{
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("9.png", "10.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("10.png", "9.png") > 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("2.png", "10.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("12.png", "13.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("img2.png", "img12.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("img12.png", "img2.png") > 0);
	TEST_ASSERT_EQUAL_INT(0, mapSourceNaturalCompare("7.png", "7.png"));
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("a.png", "b.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("b.png", "a.png") > 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("ab.png", "a.png") > 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("a.png", "1.png") > 0);
}

/* Equal numeric value orders by fewer leading zeros first. */
static void test_natural_sort_leading_zeros(void)
{
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("1.png", "01.png") < 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("01.png", "1.png") > 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("0.png", "00.png") < 0);
	TEST_ASSERT_EQUAL_INT(0, mapSourceNaturalCompare("01.png", "01.png"));
}

static void test_natural_sort_null_and_suffix(void)
{
	TEST_ASSERT_EQUAL_INT(0, mapSourceNaturalCompare(NULL, NULL));
	TEST_ASSERT_TRUE(mapSourceNaturalCompare(NULL, "a") > 0);
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("a", NULL) < 0);
	/* A shorter name that is a prefix sorts first. */
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("a", "ab") < 0);
	/* Digits vs non-digits at the same position. */
	TEST_ASSERT_TRUE(mapSourceNaturalCompare("1", "a") < 0);
}

static int cmpName(const void *a, const void *b)
{
	return mapSourceNaturalCompare(*(const char *const *)a,
				       *(const char *const *)b);
}

static void test_natural_sort_full_order(void)
{
	const char *names[] = { "10.png", "2.png", "01.png", "1.png", "9.png" };
	const char *want[] = { "1.png", "01.png", "2.png", "9.png", "10.png" };
	int i;

	qsort(names, 5, sizeof(names[0]), cmpName);
	for (i = 0; i < 5; i++)
		TEST_ASSERT_EQUAL_STRING(want[i], names[i]);
}

/* --- legend parse ------------------------------------------------------ */

static void test_legend_basic_colours_and_lights(void)
{
	MaterialTable t;
	MapSourceLegend lg;
	const char *text =
		"#00ff00 grass\n"
		"#808080 stone\n"
		"#A0522D wood\n"
		"$ point 1 2 3 10 20 30 5\n"
		"$ spot 4 5 6 255 0 0 0 1 0 45 9\n";

	buildTable(&t);
	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), &t, &lg));
	TEST_ASSERT_EQUAL_INT(3, lg.colorCount);
	TEST_ASSERT_EQUAL_INT(1, mapSourceColorLookup(&lg, 0x00FF00u));
	TEST_ASSERT_EQUAL_INT(2, mapSourceColorLookup(&lg, 0x808080u));
	TEST_ASSERT_EQUAL_INT(3, mapSourceColorLookup(&lg, 0xA0522Du));
	TEST_ASSERT_EQUAL_INT(-1, mapSourceColorLookup(&lg, 0x123456u));
	TEST_ASSERT_EQUAL_INT(0, lg.badLines);
	TEST_ASSERT_EQUAL_INT(2, lg.lightCount);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_POINT, lg.lights[0].kind);
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_SPOT, lg.lights[1].kind);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, lg.lights[1].dir[1]);
}

/* Duplicate colour: last wins + diagnostic count. */
static void test_legend_duplicate_colour_last_wins(void)
{
	MaterialTable t;
	MapSourceLegend lg;
	const char *text =
		"#00ff00 grass\n"
		"#00FF00 wood\n";

	buildTable(&t);
	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), &t, &lg));
	TEST_ASSERT_EQUAL_INT(1, lg.colorCount);
	TEST_ASSERT_EQUAL_INT(1, lg.duplicateColors);
	TEST_ASSERT_EQUAL_INT(3, mapSourceColorLookup(&lg, 0x00FF00u));
}

/* Malformed hex, unknown material, unrecognised line and an over-long line are
 * all diagnostics, never a failure. */
static void test_legend_malformed_lines(void)
{
	MaterialTable t;
	MapSourceLegend lg;
	char longLine[300];
	const char *text =
		"#GGGGGG grass\n"
		"#123456 nope\n"
		"hello there\n"
		"#12345 stone\n"
		"#00ff00 grass\n";

	buildTable(&t);
	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), &t, &lg));
	TEST_ASSERT_EQUAL_INT(1, lg.colorCount);	/* only the good line */
	TEST_ASSERT_EQUAL_INT(3, lg.badLines);		/* GG, hello, #12345 */
	TEST_ASSERT_EQUAL_INT(1, lg.unknownMaterials);
	TEST_ASSERT_EQUAL_INT(-1, mapSourceColorLookup(&lg, 0x123456u));

	/* An over-long '#' line is skipped, not read past the buffer. */
	memset(longLine, 'a', sizeof(longLine));
	longLine[0] = '#';
	longLine[sizeof(longLine) - 1] = '\n';
	TEST_ASSERT_TRUE(mapSourceLegendParse(longLine, sizeof(longLine), &t,
					      &lg));
	TEST_ASSERT_EQUAL_INT(1, lg.badLines);
}

/* A NULL material table resolves every legend colour to id 0 (ASCII parity). */
static void test_legend_null_materials(void)
{
	MapSourceLegend lg;
	const char *text = "#00ff00 anything\n";

	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), NULL, &lg));
	TEST_ASSERT_EQUAL_INT(1, lg.colorCount);
	TEST_ASSERT_EQUAL_INT(0, mapSourceColorLookup(&lg, 0x00FF00u));
}

static void test_legend_null_args(void)
{
	MapSourceLegend lg;

	TEST_ASSERT_FALSE(mapSourceLegendParse(NULL, 0, NULL, &lg));
	TEST_ASSERT_FALSE(mapSourceLegendParse("x", 1, NULL, NULL));
	TEST_ASSERT_EQUAL_INT(-1, mapSourceColorLookup(NULL, 0));
}

/* Malformed `$` lines are skipped by the shared light parser. */
static void test_legend_malformed_light_lines(void)
{
	MapSourceLegend lg;
	const char *text =
		"#00ff00 grass\n"
		"$ point 1\n"
		"$ nonsense\n"
		"  $ point 1 2 3 4 5 6\n";

	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), NULL, &lg));
	TEST_ASSERT_EQUAL_INT(1, lg.lightCount);	/* only the last, valid one */
}

static void test_legend_overflow_colours_and_lights(void)
{
	MapSourceLegend lg;
	char text[MAPSOURCE_MAX_COLORS * 32 + VOXMAP_MAX_LIGHTS * 40 + 64];
	size_t n = 0;
	int i;

	for (i = 0; i < MAPSOURCE_MAX_COLORS + 3; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n,
				      "#%06X grass\n", (unsigned)i);
	for (i = 0; i < VOXMAP_MAX_LIGHTS + 2; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n,
				      "$ point %d 0 0 1 2 3\n", i);
	TEST_ASSERT_TRUE(mapSourceLegendParse(text, n, NULL, &lg));
	TEST_ASSERT_EQUAL_INT(MAPSOURCE_MAX_COLORS, lg.colorCount);
	TEST_ASSERT_EQUAL_INT(3, lg.overflowColors);
	TEST_ASSERT_EQUAL_INT(VOXMAP_MAX_LIGHTS, lg.lightCount);
	TEST_ASSERT_EQUAL_INT(2, lg.overflowLights);
}

/* Whitespace handling: blank lines, leading tabs, tab separators, trailing
 * spaces, a missing final newline, and a line with too many tokens. */
static void test_legend_whitespace_and_extra_tokens(void)
{
	MapSourceLegend lg;
	const char *text =
		"#00ff00 grass\n"
		"\n"
		"   \n"
		"#808080\tstone\n"
		"  #0000FF sixface\n"
		"\t$ point 1 2 3 4 5 6\n"
		"#A0522D wood extra more five\n"
		"#-12345 grass\n"
		"#gggggg grass\n"
		"#FF00FF foliage   ";

	TEST_ASSERT_TRUE(mapSourceLegendParse(text, strlen(text), NULL, &lg));
	TEST_ASSERT_EQUAL_INT(4, lg.colorCount);	/* grass, stone, sixface, foliage */
	TEST_ASSERT_EQUAL_INT(3, lg.badLines);		/* 5 tokens, -, gggggg */
	TEST_ASSERT_EQUAL_INT(1, lg.lightCount);
	TEST_ASSERT_EQUAL_INT(0, mapSourceColorLookup(&lg, 0x0000FFu));
	TEST_ASSERT_EQUAL_INT(0, mapSourceColorLookup(&lg, 0xFF00FFu));
}

/* --- assembly ---------------------------------------------------------- */

/* 2x2 x 2 levels. Level 0: grass, stone / air, grass. Level 1: air, air /
 * stone, air. Alpha 1 is solid (documented threshold: only alpha 0 is air). */
static void test_assemble_exact_occupancy_and_materials(void)
{
	MaterialTable t;
	MapSourceLegend lg;
	MapSourceImage imgs[2];
	uint8_t buf0[16];
	uint8_t buf1[16];
	Rgba px0[4] = { C_GRASS, C_STONE, C_AIR, C_GRASS };
	Rgba px1[4] = { C_AIR, C_AIR, C_STONE, C_AIR };
	MapSourceStats stats;
	Voxmap *map;
	const char *legend = "#00ff00 grass\n#808080 stone\n";

	buildTable(&t);
	TEST_ASSERT_TRUE(mapSourceLegendParse(legend, strlen(legend), &t, &lg));
	makeImage(&imgs[0], buf0, 2, 2, px0);
	makeImage(&imgs[1], buf1, 2, 2, px1);
	map = mapSourceAssembleSlices(imgs, 2, &lg, "test", &stats);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(2, voxmapWidth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapDepth(map));
	TEST_ASSERT_EQUAL_INT(2, voxmapLevels(map));
	TEST_ASSERT_EQUAL_INT(2, stats.levels);
	TEST_ASSERT_EQUAL_INT(4, stats.solidVoxels);
	TEST_ASSERT_EQUAL_INT(0, stats.unknownColorVoxels);

	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAtVoxel(map, 0, 0, 0));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(2, voxmapMaterialAtVoxel(map, 1, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 0, 1));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 1, 0, 1));
	TEST_ASSERT_EQUAL_INT(1, voxmapMaterialAtVoxel(map, 1, 0, 1));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 1, 1));
	TEST_ASSERT_EQUAL_INT(2, voxmapMaterialAtVoxel(map, 0, 1, 1));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 1, 0));
	/* The top of column (1,0) is the level-0 stone voxel (height 1). */
	TEST_ASSERT_EQUAL_INT(1, voxmapHeightAt(map, 1, 0));
	destroyVoxmap(map);
}

/* Alpha 1 is solid; alpha 0 is air. */
static void test_assemble_alpha_threshold(void)
{
	MapSourceLegend lg;
	MapSourceImage img;
	uint8_t buf[8];
	Rgba px[2] = { { 0, 255, 0, 1 }, { 0, 255, 0, 0 } };
	Voxmap *map;

	memset(&lg, 0, sizeof(lg));
	lg.colors[0].rgb = 0x00FF00u;
	lg.colors[0].material = 7;
	lg.colorCount = 1;
	makeImage(&img, buf, 2, 1, px);
	map = mapSourceAssembleSlices(&img, 1, &lg, "alpha", NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(7, voxmapMaterialAtVoxel(map, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 0, 0));
	/* No ground flag: a fully air column is void (height -1), not height 0. */
	TEST_ASSERT_TRUE(voxmapIsVoid(map, 1, 0));
	destroyVoxmap(map);
}

/* An opaque colour absent from the legend is air and counted (plural too). */
static void test_assemble_unknown_colour_is_air(void)
{
	MapSourceLegend lg;
	MapSourceImage img;
	uint8_t buf[12];
	Rgba px[3] = { C_RED, C_RED, C_GRASS };
	MapSourceStats stats;
	Voxmap *map;

	memset(&lg, 0, sizeof(lg));
	lg.colors[0].rgb = 0x00FF00u;
	lg.colors[0].material = 1;
	lg.colorCount = 1;
	makeImage(&img, buf, 3, 1, px);
	map = mapSourceAssembleSlices(&img, 1, &lg, "unknown", &stats);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 0, 0));
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 2, 0, 0));
	TEST_ASSERT_EQUAL_INT(2, stats.unknownColorVoxels);
	destroyVoxmap(map);

	/* A singular unknown count also hits the singular diagnostic arm. */
	{
		Rgba one[1] = { C_RED };

		makeImage(&img, buf, 1, 1, one);
		map = mapSourceAssembleSlices(&img, 1, &lg, "one", &stats);
		TEST_ASSERT_NOT_NULL(map);
		TEST_ASSERT_EQUAL_INT(1, stats.unknownColorVoxels);
		destroyVoxmap(map);
	}
}

/* The legend's lights are attached to the assembled map. */
static void test_assemble_attaches_lights(void)
{
	MapSourceLegend lg;
	MapSourceImage img;
	uint8_t buf[4];
	Rgba px[1] = { C_GRASS };
	Voxmap *map;
	const VoxmapLight *l;

	memset(&lg, 0, sizeof(lg));
	lg.colors[0].rgb = 0x00FF00u;
	lg.colors[0].material = 1;
	lg.colorCount = 1;
	lg.lights[0].kind = VOXMAP_LIGHT_POINT;
	lg.lights[0].x = 2.0f;
	lg.lights[0].y = 3.0f;
	lg.lights[0].z = 4.0f;
	lg.lights[0].r = 10.0f;
	lg.lightCount = 1;
	makeImage(&img, buf, 1, 1, px);
	map = mapSourceAssembleSlices(&img, 1, &lg, "lights", NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_EQUAL_INT(1, voxmapLightCount(map));
	l = voxmapLightAt(map, 0);
	TEST_ASSERT_NOT_NULL(l);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, l->x);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, l->y);
	destroyVoxmap(map);
}

/* Every malformed assembly input fails cleanly (NULL), never crashes. */
static void test_assemble_malformed_inputs(void)
{
	MapSourceLegend lg;
	MapSourceImage imgs[2];
	uint8_t buf[16];
	uint8_t dummy[4] = { 0, 0, 0, 0 };
	Rgba px[4] = { C_GRASS, C_GRASS, C_GRASS, C_GRASS };
	MapSourceImage big;
	MapSourceImage *many;
	int i;

	memset(&lg, 0, sizeof(lg));
	/* Zero slices / NULL inputs. */
	TEST_ASSERT_NULL(mapSourceAssembleSlices(NULL, 0, &lg, "x", NULL));
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 0, &lg, "x", NULL));
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 1, NULL, "x", NULL));

	makeImage(&imgs[0], buf, 2, 2, px);
	/* NULL pixel buffer. */
	imgs[0].rgba = NULL;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 1, &lg, "x", NULL));
	imgs[0].rgba = buf;
	/* Mismatched slice sizes. */
	makeImage(&imgs[0], buf, 2, 2, px);
	imgs[1].width = 1;
	imgs[1].height = 1;
	imgs[1].rgba = dummy;
	imgs[1].pitch = 4;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 2, &lg, "x", NULL));
	/* Same width, different height (the second operand of the size check). */
	imgs[1].width = 2;
	imgs[1].height = 1;
	imgs[1].pitch = 8;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 2, &lg, "x", NULL));
	/* Pitch too small. */
	imgs[1].width = 2;
	imgs[1].height = 2;
	imgs[1].pitch = 7;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(imgs, 2, &lg, "x", NULL));
	/* Width over the dimension cap. */
	big.width = VOXMAP_MAX_DIM + 1;
	big.height = 1;
	big.rgba = dummy;
	big.pitch = (size_t)(VOXMAP_MAX_DIM + 1) * 4u;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(&big, 1, &lg, "x", NULL));

	/* Over-large volume: 256x256 x 17 > VOXMAP_MAX_CELLS. The validation
	 * loop runs before the size check, so every slice needs a valid (but
	 * never-read) buffer. */
	many = malloc(17 * sizeof(*many));
	TEST_ASSERT_NOT_NULL(many);
	for (i = 0; i < 17; i++) {
		many[i].width = 256;
		many[i].height = 256;
		many[i].rgba = dummy;
		many[i].pitch = 256u * 4u;
	}
	TEST_ASSERT_NULL(mapSourceAssembleSlices(many, 17, &lg, "x", NULL));
	free(many);

	/* Levels over the dimension cap. */
	many = malloc((VOXMAP_MAX_DIM + 1) * sizeof(*many));
	TEST_ASSERT_NOT_NULL(many);
	for (i = 0; i < VOXMAP_MAX_DIM + 1; i++) {
		many[i].width = 1;
		many[i].height = 1;
		many[i].rgba = dummy;
		many[i].pitch = 4;
	}
	TEST_ASSERT_NULL(mapSourceAssembleSlices(many, VOXMAP_MAX_DIM + 1,
						 &lg, "x", NULL));
	free(many);
}

/* Dimension guards (width/height below 1 or above the cap) and a NULL label
 * are all clean failures; a NULL label on a valid map still assembles. */
static void test_assemble_size_guards_and_null_label(void)
{
	MapSourceLegend lg;
	MapSourceImage img;
	uint8_t buf[4] = { 0, 255, 0, 255 };
	Voxmap *map;

	memset(&lg, 0, sizeof(lg));
	lg.colors[0].rgb = 0x00FF00u;
	lg.colors[0].material = 1;
	lg.colorCount = 1;
	img.rgba = buf;
	img.pitch = 4;

	img.width = 0;
	img.height = 1;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(&img, 1, &lg, NULL, NULL));
	img.width = 1;
	img.height = 0;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(&img, 1, &lg, NULL, NULL));
	img.width = 1;
	img.height = VOXMAP_MAX_DIM + 1;
	TEST_ASSERT_NULL(mapSourceAssembleSlices(&img, 1, &lg, NULL, NULL));

	img.width = 1;
	img.height = 1;
	map = mapSourceAssembleSlices(&img, 1, &lg, NULL, NULL);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	destroyVoxmap(map);
}

/* --- voxmap entry points used by the glue ------------------------------ */

static void test_voxmap_parse_light_line_public(void)
{
	VoxmapLight l;
	const char *point = "$ point 1 2 3 4 5 6 7";
	const char *spot = "$ spot 0 0 0 9 9 9 0 0 2 30";
	const char *notLight = "point 1 2 3 4 5 6";
	const char *shortLine = "$ point 1";

	TEST_ASSERT_TRUE(voxmapParseLightLine(point, strlen(point), &l));
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_POINT, l.kind);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, l.x);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.0f, l.radius);
	TEST_ASSERT_TRUE(voxmapParseLightLine(spot, strlen(spot), &l));
	TEST_ASSERT_EQUAL_INT(VOXMAP_LIGHT_SPOT, l.kind);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, l.dir[2]);
	/* Not a light line / malformed / NULL args. */
	TEST_ASSERT_FALSE(voxmapParseLightLine(notLight, strlen(notLight), &l));
	TEST_ASSERT_FALSE(voxmapParseLightLine(shortLine, strlen(shortLine),
					       &l));
	TEST_ASSERT_FALSE(voxmapParseLightLine(NULL, 0, &l));
	TEST_ASSERT_FALSE(voxmapParseLightLine(point, strlen(point), NULL));
}

static void test_voxmap_build_raw(void)
{
	uint8_t solid[2] = { 1, 0 };
	int16_t mats[2] = { 5, -1 };
	VoxmapLight lights[VOXMAP_MAX_LIGHTS + 4];
	Voxmap *map;
	int i;

	memset(lights, 0, sizeof(lights));
	map = voxmapBuildRaw(2, 1, 1, solid, mats, NULL, 0);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_TRUE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(5, voxmapMaterialAtVoxel(map, 0, 0, 0));
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, voxmapLightCount(map));
	destroyVoxmap(map);

	/* NULL grids build an all-air map; lights are copied and capped. */
	for (i = 0; i < VOXMAP_MAX_LIGHTS + 4; i++)
		lights[i].kind = VOXMAP_LIGHT_POINT;
	map = voxmapBuildRaw(1, 1, 1, NULL, NULL, lights,
			     VOXMAP_MAX_LIGHTS + 4);
	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_FALSE(voxmapSolidAt(map, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(VOXMAP_MAX_LIGHTS, voxmapLightCount(map));
	destroyVoxmap(map);

	/* Out-of-range dimensions and an over-large volume are NULL. */
	TEST_ASSERT_NULL(voxmapBuildRaw(0, 1, 1, NULL, NULL, NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(1, 0, 1, NULL, NULL, NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(1, 1, 0, NULL, NULL, NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(VOXMAP_MAX_DIM + 1, 1, 1, NULL, NULL,
					NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(1, VOXMAP_MAX_DIM + 1, 1, NULL, NULL,
					NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(1, 1, VOXMAP_MAX_DIM + 1, NULL, NULL,
					NULL, 0));
	TEST_ASSERT_NULL(voxmapBuildRaw(256, 256, 17, NULL, NULL, NULL, 0));
}

/* --- runner ------------------------------------------------------------ */

void run_test_mapsource(void);

void run_test_mapsource(void)
{
	RUN_TEST(test_natural_sort_basic);
	RUN_TEST(test_natural_sort_leading_zeros);
	RUN_TEST(test_natural_sort_null_and_suffix);
	RUN_TEST(test_natural_sort_full_order);
	RUN_TEST(test_legend_basic_colours_and_lights);
	RUN_TEST(test_legend_duplicate_colour_last_wins);
	RUN_TEST(test_legend_malformed_lines);
	RUN_TEST(test_legend_null_materials);
	RUN_TEST(test_legend_null_args);
	RUN_TEST(test_legend_malformed_light_lines);
	RUN_TEST(test_legend_overflow_colours_and_lights);
	RUN_TEST(test_legend_whitespace_and_extra_tokens);
	RUN_TEST(test_assemble_exact_occupancy_and_materials);
	RUN_TEST(test_assemble_alpha_threshold);
	RUN_TEST(test_assemble_unknown_colour_is_air);
	RUN_TEST(test_assemble_attaches_lights);
	RUN_TEST(test_assemble_malformed_inputs);
	RUN_TEST(test_assemble_size_guards_and_null_label);
	RUN_TEST(test_voxmap_parse_light_line_public);
	RUN_TEST(test_voxmap_build_raw);
}
