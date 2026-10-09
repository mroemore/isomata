/*
 * Light-grid tests (CTOL rung 1: unit + boundary).
 *
 * Pins the pure lighting core's contract with exact golden values:
 *  - point-light attenuation rings (min(255, 255 - k*LIGHT_ATTEN));
 *  - per-channel independence (a red light leaves G/B at 0);
 *  - per-channel MAX merge (overlap is the brighter channel, never a sum);
 *  - blocking by a solid wall and corner-bending through a gap;
 *  - the sky rule: open field = full sky, a roof = an attenuated gradient from
 *    the edge, a 1x1 shaft = a bright column with sideways falloff, a fully
 *    enclosed air pocket = sky 0 (the roof/shaft/enclosed cases need overhangs
 *    the ASCII heightmap cannot express, so they use lightGridPropagateSolid
 *    with a caller-built solid volume);
 *  - spot seeding: a downward cone with a monotonic angle falloff, off-axis
 *    dark, and a wall blocking line of sight;
 *  - radius clamping the point-light reach;
 *  - bounds / NULL / bad-argument no-ops, determinism, and the dump format.
 *
 * Pure: links only lightgrid.c (+ voxmap/camera/drawlist deps) and Unity.
 * Harness convention: no main()/setUp()/tearDown(); exposes run_test_lightgrid().
 */

#include "unity.h"

#include "render/lightgrid.h"
#include "render/textures.h"
#include "render/voxmap.h"

#include <math.h>
#include <string.h>

/* Grid / solid-volume index order (see lightgrid.h): x fastest, then y, then z. */
static size_t gidx(int w, int h, int x, int y, int z)
{
	return (size_t)((z * h + y) * w + x);
}

/* Parse a map from a string literal (NULL material table). */
static Voxmap *mkMap(const char *text)
{
	return parseVoxmapText(text, strlen(text), NULL);
}

/* Mark one cell solid in a caller-built volume. */
static void setSolid(uint8_t *mask, int w, int h, int x, int y, int z)
{
	mask[gidx(w, h, x, y, z)] = 1;
}

/* --- 1. point-light attenuation rings ---------------------------------- */

static void test_point_attenuation_rings(void)
{
	LightGrid *g = lightGridCreate(11, 11, 4);

	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, NULL);	/* all air */
	lightGridSeedPoint(g, 5.5f, 1.5f, 5.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagate(g, NULL);

	/* 255 - k*16 along a straight line. */
	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(g, 5, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 6, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(223, lightGridBlockAt(g, 7, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(207, lightGridBlockAt(g, 8, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(191, lightGridBlockAt(g, 9, 1, 5, 0));
	/* All channels equal for white light. */
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 6, 1, 5, 1));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 6, 1, 5, 2));
	/* Vertical is a normal step too (6-neighbour). */
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 5, 2, 5, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 5, 0, 5, 0));
	destroyLightGrid(g);
}

/* --- 2. per-channel independence --------------------------------------- */

static void test_per_channel_independence(void)
{
	LightGrid *g = lightGridCreate(11, 11, 4);

	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, NULL);
	lightGridSeedPoint(g, 5.5f, 1.5f, 5.5f, 255.0f, 0.0f, 0.0f, 0.0f);
	lightGridPropagate(g, NULL);

	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(g, 5, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 6, 1, 5, 0));
	TEST_ASSERT_EQUAL_INT(223, lightGridBlockAt(g, 7, 1, 5, 0));
	/* A red light must never put anything in green or blue. */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 5, 1, 5, 1));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 5, 1, 5, 2));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 6, 1, 5, 1));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 6, 1, 5, 2));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 9, 1, 5, 1));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 9, 1, 5, 2));
	destroyLightGrid(g);
}

/* --- 3. per-channel max merge ------------------------------------------ */

static void test_max_merge(void)
{
	LightGrid *same = lightGridCreate(9, 1, 1);
	LightGrid *mix = lightGridCreate(9, 1, 1);

	TEST_ASSERT_NOT_NULL(same);
	TEST_ASSERT_NOT_NULL(mix);

	/* Two red lights a step apart: the midpoint takes 239, NOT 255 (sum). */
	lightGridSeedPoint(same, 2.5f, 0.5f, 0.5f, 255.0f, 0.0f, 0.0f, 0.0f);
	lightGridSeedPoint(same, 4.5f, 0.5f, 0.5f, 255.0f, 0.0f, 0.0f, 0.0f);
	lightGridPropagate(same, NULL);
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(same, 3, 0, 0, 0));

	/* Red + green overlap: per-channel max gives yellow, no accumulation. */
	lightGridSeedPoint(mix, 2.5f, 0.5f, 0.5f, 255.0f, 0.0f, 0.0f, 0.0f);
	lightGridSeedPoint(mix, 4.5f, 0.5f, 0.5f, 0.0f, 255.0f, 0.0f, 0.0f);
	lightGridPropagate(mix, NULL);
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(mix, 3, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(mix, 3, 0, 0, 1));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(mix, 3, 0, 0, 2));
	destroyLightGrid(same);
	destroyLightGrid(mix);
}

/* --- 4. blocking and corner bending ------------------------------------ */

static void test_blocking_and_corner_bending(void)
{
	/* A solid wall at x = 3 (height 3, filling the grid) splits the map. */
	Voxmap *blocked = mkMap("1113111\n1113111\n1113111\n");
	Voxmap *gap = mkMap("1111111\n1113111\n1113111\n");
	LightGrid *gb = lightGridCreate(7, 3, 3);
	LightGrid *gg = lightGridCreate(7, 3, 3);

	TEST_ASSERT_NOT_NULL(blocked);
	TEST_ASSERT_NOT_NULL(gap);
	TEST_ASSERT_NOT_NULL(gb);
	TEST_ASSERT_NOT_NULL(gg);

	/* No gap: the far side is completely dark. */
	lightGridSeedPoint(gb, 1.5f, 1.5f, 1.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagate(gb, blocked);
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(gb, 2, 1, 1, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(gb, 5, 1, 1, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(gb, 6, 1, 1, 0));

	/* A gap at (3, *, 0) lets a gradient bend around the wall corner. */
	lightGridSeedPoint(gg, 1.5f, 1.5f, 1.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagate(gg, gap);
	TEST_ASSERT_EQUAL_INT(207, lightGridBlockAt(gg, 3, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(191, lightGridBlockAt(gg, 4, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(175, lightGridBlockAt(gg, 5, 1, 0, 0));
	TEST_ASSERT_EQUAL_INT(159, lightGridBlockAt(gg, 5, 1, 1, 0));
	TEST_ASSERT_EQUAL_INT(159, lightGridBlockAt(gg, 5, 1, 1, 1));

	destroyVoxmap(blocked);
	destroyVoxmap(gap);
	destroyLightGrid(gb);
	destroyLightGrid(gg);
}

/* --- 5. sky: open field ------------------------------------------------- */

static void test_sky_open_field(void)
{
	Voxmap *map = mkMap("11111\n11111\n11111\n11111\n11111\n");
	LightGrid *g = lightGridCreate(5, 5, 4);

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridPropagate(g, map);

	/* Every air cell in an open column is sky-open (zero attenuation down). */
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 1, 2));
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 2, 2));
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 3, 2));
	/* Solid cells hold no sky. */
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 2, 0, 2));
	destroyVoxmap(map);
	destroyLightGrid(g);
}

/* --- 6. sky: roof gradient --------------------------------------------- */

static void test_sky_roof_gradient(void)
{
	/* Floor at y=0, a ceiling at y=3 over x >= 2, open sky over x < 2. */
	LightGrid *g = lightGridCreate(5, 5, 5);
	uint8_t mask[5 * 5 * 5];
	int x;
	int z;

	TEST_ASSERT_NOT_NULL(g);
	memset(mask, 0, sizeof(mask));
	for (z = 0; z < 5; z++) {
		for (x = 0; x < 5; x++)
			setSolid(mask, 5, 5, x, 0, z);	/* floor */
		for (x = 2; x < 5; x++)
			setSolid(mask, 5, 5, x, 3, z);	/* ceiling */
	}
	lightGridPropagateSolid(g, mask);

	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 1, 2, 2));
	/* Attenuated gradient from the roof edge inward: 239, 223, 207. */
	TEST_ASSERT_EQUAL_INT(239, lightGridSkyAt(g, 2, 2, 2));
	TEST_ASSERT_EQUAL_INT(223, lightGridSkyAt(g, 3, 2, 2));
	TEST_ASSERT_EQUAL_INT(207, lightGridSkyAt(g, 4, 2, 2));
	/* Above the ceiling is still open sky. */
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 4, 2));
	destroyLightGrid(g);
}

/* --- 7. sky: 1x1 shaft -------------------------------------------------- */

static void test_sky_shaft(void)
{
	/* Ceiling at y=3 with a single hole at (2, 3, 2). */
	LightGrid *g = lightGridCreate(5, 5, 6);
	uint8_t mask[5 * 5 * 6];
	int x;
	int z;

	TEST_ASSERT_NOT_NULL(g);
	memset(mask, 0, sizeof(mask));
	for (z = 0; z < 5; z++) {
		for (x = 0; x < 5; x++) {
			setSolid(mask, 5, 6, x, 0, z);	/* floor */
			setSolid(mask, 5, 6, x, 3, z);	/* ceiling */
		}
	}
	mask[gidx(5, 6, 2, 3, 2)] = 0;	/* the shaft */

	lightGridPropagateSolid(g, mask);

	/* The shaft column is sky-open all the way down (bright column). */
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 1, 2));
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 2, 2));
	/* Sideways falloff: 239, 223 away from the shaft. */
	TEST_ASSERT_EQUAL_INT(239, lightGridSkyAt(g, 1, 1, 2));
	TEST_ASSERT_EQUAL_INT(223, lightGridSkyAt(g, 0, 1, 2));
	TEST_ASSERT_EQUAL_INT(239, lightGridSkyAt(g, 1, 2, 2));
	TEST_ASSERT_EQUAL_INT(223, lightGridSkyAt(g, 0, 2, 2));
	destroyLightGrid(g);
}

/* --- 8. sky: fully enclosed room --------------------------------------- */

static void test_sky_enclosed_room(void)
{
	/* Floor and a solid ceiling everywhere: the layer between sees no sky. */
	LightGrid *g = lightGridCreate(5, 5, 6);
	uint8_t mask[5 * 5 * 6];
	int x;
	int z;

	TEST_ASSERT_NOT_NULL(g);
	memset(mask, 0, sizeof(mask));
	for (z = 0; z < 5; z++) {
		for (x = 0; x < 5; x++) {
			setSolid(mask, 5, 6, x, 0, z);
			setSolid(mask, 5, 6, x, 3, z);
		}
	}
	lightGridPropagateSolid(g, mask);

	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 2, 1, 2));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 2, 2, 2));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 0, 1, 0));
	/* Above the ceiling is open sky. */
	TEST_ASSERT_EQUAL_INT(LIGHT_SKY_FULL, lightGridSkyAt(g, 2, 4, 2));
	destroyLightGrid(g);
}

/* --- 9. spot: pool and angle falloff ----------------------------------- */

static void test_spot_pool_and_falloff(void)
{
	Voxmap *map = mkMap("11111111111\n11111111111\n11111111111\n"
			    "11111111111\n11111111111\n11111111111\n"
			    "11111111111\n11111111111\n11111111111\n"
			    "11111111111\n11111111111\n");
	LightGrid *g = lightGridCreate(11, 11, 11);
	const float dir[3] = { 0.0f, -1.0f, 0.0f };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	/* Apex above the floor, straight down, 45-degree half-angle, radius 10. */
	lightGridSeedSpot(g, map, 5.5f, 9.5f, 5.5f, dir, 45.0f,
			  255.0f, 255.0f, 255.0f, 10.0f);
	/* No propagation: the raw cone is observable directly. */

	/* Axis: 255 * (1 - dist/10), monotonic with distance. */
	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(g, 5, 9, 5, 0));
	TEST_ASSERT_EQUAL_INT(153, lightGridBlockAt(g, 5, 5, 5, 0));
	TEST_ASSERT_EQUAL_INT(51, lightGridBlockAt(g, 5, 1, 5, 0));
	/* Off-axis (14.04 deg, dist ~4.123): 255 * 0.5877 * 0.6881 = 103. */
	TEST_ASSERT_EQUAL_INT(103, lightGridBlockAt(g, 6, 5, 5, 0));
	TEST_ASSERT_TRUE(lightGridBlockAt(g, 5, 5, 5, 0) >
			 lightGridBlockAt(g, 6, 5, 5, 0));
	/* At the 45-degree cone edge the angle falloff is 0; outside is dark. */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 9, 5, 5, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 10, 5, 5, 0));
	/* The floor cell itself is solid and never seeded. */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 5, 0, 5, 0));
	destroyVoxmap(map);
	destroyLightGrid(g);
}

/* --- 10. spot: wall blocks line of sight ------------------------------- */

static void test_spot_los_blocked(void)
{
	/* A wall at x=4 (height 6) between the apex and the far cells. */
	Voxmap *map = mkMap("111161111\n111161111\n111161111\n");
	LightGrid *g = lightGridCreate(9, 3, 8);
	const float dir[3] = { 1.0f, 0.0f, 0.0f };

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedSpot(g, map, 2.5f, 5.5f, 1.5f, dir, 30.0f,
			  255.0f, 255.0f, 255.0f, 8.0f);

	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(g, 2, 5, 1, 0));	/* apex */
	TEST_ASSERT_EQUAL_INT(223, lightGridBlockAt(g, 3, 5, 1, 0));	/* before */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 4, 5, 1, 0));	/* wall */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 5, 5, 1, 0));	/* behind */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 6, 5, 1, 0));	/* behind */
	destroyVoxmap(map);
	destroyLightGrid(g);
}

/* --- 11. radius clamp --------------------------------------------------- */

static void test_radius_clamp(void)
{
	LightGrid *shortReach = lightGridCreate(9, 1, 1);
	LightGrid *global = lightGridCreate(9, 1, 1);

	TEST_ASSERT_NOT_NULL(shortReach);
	TEST_ASSERT_NOT_NULL(global);

	/* radius 4 -> seed min(255, 64) = 64, so the reach ends at 4 steps. */
	lightGridSeedPoint(shortReach, 1.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f,
			   4.0f);
	lightGridPropagate(shortReach, NULL);
	TEST_ASSERT_EQUAL_INT(64, lightGridBlockAt(shortReach, 1, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(48, lightGridBlockAt(shortReach, 2, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(32, lightGridBlockAt(shortReach, 3, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(16, lightGridBlockAt(shortReach, 4, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(shortReach, 5, 0, 0, 0));

	/* radius <= 0 keeps the full channel value. */
	lightGridSeedPoint(global, 1.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagate(global, NULL);
	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(global, 1, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(global, 2, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(223, lightGridBlockAt(global, 3, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(207, lightGridBlockAt(global, 4, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(191, lightGridBlockAt(global, 5, 0, 0, 0));
	destroyLightGrid(shortReach);
	destroyLightGrid(global);
}

/* --- 11b. stale bucket entry + combined saturation --------------------- */

static void test_stale_and_combined_saturation(void)
{
	LightGrid *g = lightGridCreate(3, 1, 1);
	uint8_t out[3];
	char buf[16];

	TEST_ASSERT_NOT_NULL(g);
	/* Cell 0 is seeded weakly (radius 4 -> 64) and then beaten by the
	 * neighbour of a full light at cell 2: 239 - 16 = 223. The weak seed's
	 * bucket-64 entry is then stale and must be skipped, not re-applied. */
	lightGridSeedPoint(g, 0.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridSeedPoint(g, 2.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagateSolid(g, NULL);	/* all air: sky 255 everywhere */

	TEST_ASSERT_EQUAL_INT(223, lightGridBlockAt(g, 0, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(239, lightGridBlockAt(g, 1, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(g, 2, 0, 0, 0));

	/* Sky 255 + block saturates the combined query at 255 per channel. */
	lightGridAt(g, 0, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(255, out[0]);
	TEST_ASSERT_EQUAL_INT(255, out[1]);
	TEST_ASSERT_EQUAL_INT(255, out[2]);

	/* The dump's per-cell intensity saturates the same way. */
	lightGridDumpSlice(g, 0, buf, sizeof(buf));
	TEST_ASSERT_EQUAL_STRING("ffffff\n", buf);

	destroyLightGrid(g);
}

/* --- 12. bounds / NULL / bad arguments --------------------------------- */

static void test_bounds_and_null(void)
{
	LightGrid *g = lightGridCreate(3, 2, 2);
	uint8_t out[3] = { 9, 9, 9 };

	TEST_ASSERT_NULL(lightGridCreate(0, 1, 1));
	TEST_ASSERT_NULL(lightGridCreate(1, 0, 1));
	TEST_ASSERT_NULL(lightGridCreate(1, 1, 0));
	TEST_ASSERT_NULL(lightGridCreate(-1, 1, 1));
	TEST_ASSERT_NULL(lightGridCreate(LIGHTGRID_MAX_DIM + 1, 1, 1));
	TEST_ASSERT_NULL(lightGridCreate(1, LIGHTGRID_MAX_DIM + 1, 1));
	TEST_ASSERT_NULL(lightGridCreate(1, 1, LIGHTGRID_MAX_DIM + 1));
	TEST_ASSERT_NULL(lightGridCreate(LIGHTGRID_MAX_DIM, LIGHTGRID_MAX_DIM,
					 LIGHTGRID_MAX_DIM));

	TEST_ASSERT_NOT_NULL(g);
	TEST_ASSERT_EQUAL_INT(3, lightGridWidth(g));
	TEST_ASSERT_EQUAL_INT(2, lightGridDepth(g));
	TEST_ASSERT_EQUAL_INT(2, lightGridHeight(g));

	/* NULL grid: dims 0, queries 0, no-ops everywhere. */
	TEST_ASSERT_EQUAL_INT(0, lightGridWidth(NULL));
	TEST_ASSERT_EQUAL_INT(0, lightGridDepth(NULL));
	TEST_ASSERT_EQUAL_INT(0, lightGridHeight(NULL));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(NULL, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(NULL, 0, 0, 0, 0));
	lightGridAt(NULL, 0, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(0, out[0]);
	lightGridSeedPoint(NULL, 0.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridPropagate(NULL, NULL);
	lightGridPropagateSolid(NULL, NULL);
	destroyLightGrid(NULL);

	/* Out-of-bounds queries on a real grid return 0 / {0,0,0}. */
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, -1, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 3, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 0, -1, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 0, 2, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 0, 0, -1));
	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 0, 0, 2));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, -1, 0, 0, 0));
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 0, 0, 0, -1));	/* bad ch */
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 0, 0, 0, 3));	/* bad ch */
	lightGridAt(g, 9, 9, 9, out);
	TEST_ASSERT_EQUAL_INT(0, out[0]);
	TEST_ASSERT_EQUAL_INT(0, out[1]);
	TEST_ASSERT_EQUAL_INT(0, out[2]);
	lightGridAt(g, 0, 0, 0, NULL);		/* NULL out: no-op */

	/* A point outside the grid is a no-op; nothing lights up. */
	lightGridSeedPoint(g, -5.0f, -5.0f, -5.0f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridSeedPoint(g, 0.5f, -1.0f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridSeedPoint(g, 0.5f, 0.5f, -1.0f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridSeedPoint(g, 100.0f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridSeedPoint(g, 0.5f, 100.0f, 0.5f, 255.0f, 255.0f, 255.0f, 0.0f);
	lightGridSeedPoint(g, 0.5f, 0.5f, 100.0f, 255.0f, 255.0f, 255.0f, 0.0f);
	/* Non-finite coordinates are rejected per axis. */
	lightGridSeedPoint(g, (float)NAN, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f,
			   0.0f);
	lightGridSeedPoint(g, 0.5f, (float)NAN, 0.5f, 255.0f, 255.0f, 255.0f,
			   0.0f);
	lightGridSeedPoint(g, 0.5f, 0.5f, (float)INFINITY, 255.0f, 255.0f,
			   255.0f, 0.0f);
	lightGridPropagate(g, NULL);
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 0, 0, 0, 0));

	/* A NULL / zero dir, or non-positive radius / angle, is a no-op. */
	{
		const float dir[3] = { 0.0f, -1.0f, 0.0f };
		const float zero[3] = { 0.0f, 0.0f, 0.0f };

		lightGridSeedSpot(NULL, NULL, 0.5f, 0.5f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);	/* NULL grid */
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 0.5f, zero, 45.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);	/* zero dir */
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 0.5f, NULL, 45.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);	/* NULL dir */
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 0.5f, dir, 0.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);	/* zero angle */
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 0.0f);	/* zero radius */
		lightGridSeedSpot(g, NULL, (float)NAN, 0.5f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);	/* NaN apex */
		lightGridSeedSpot(g, NULL, 0.5f, (float)NAN, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 5.0f);
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, (float)INFINITY, dir,
				  45.0f, 255.0f, 255.0f, 255.0f, 5.0f);
		{
			const float inf[3] = { (float)INFINITY, 0.0f, 0.0f };

			lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 0.5f, inf, 45.0f,
					  255.0f, 255.0f, 255.0f, 5.0f);
		}
		/* Finite apexes whose search box misses the grid on either side. */
		lightGridSeedSpot(g, NULL, 100.0f, 0.5f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 2.0f);
		lightGridSeedSpot(g, NULL, -100.0f, 0.5f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 2.0f);
		lightGridSeedSpot(g, NULL, 0.5f, 100.0f, 0.5f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 2.0f);
		lightGridSeedSpot(g, NULL, 0.5f, 0.5f, 100.0f, dir, 45.0f,
				  255.0f, 255.0f, 255.0f, 2.0f);
	}
	TEST_ASSERT_EQUAL_INT(0, lightGridBlockAt(g, 0, 0, 0, 0));

	/* Dump into a NULL / zero-size buffer is a no-op. */
	{
		char buf[8] = "keep";

		lightGridDumpSlice(g, 0, NULL, 8);
		lightGridDumpSlice(g, 0, buf, 0);
		TEST_ASSERT_EQUAL_STRING("keep", buf);
		lightGridDumpSlice(g, -1, buf, sizeof(buf));	/* y < 0 */
		TEST_ASSERT_EQUAL_STRING("", buf);
	}
	destroyLightGrid(g);

	/* A spot whose bounding box lies inside the grid needs no clamping. */
	{
		LightGrid *s = lightGridCreate(11, 11, 11);
		const float down[3] = { 0.0f, -1.0f, 0.0f };

		TEST_ASSERT_NOT_NULL(s);
		lightGridSeedSpot(s, NULL, 5.5f, 5.5f, 5.5f, down, 30.0f,
				  255.0f, 255.0f, 255.0f, 2.0f);
		TEST_ASSERT_EQUAL_INT(255, lightGridBlockAt(s, 5, 5, 5, 0));
		destroyLightGrid(s);
	}

	/* A buffer that fits the cells but not the row's newline truncates the
	 * newline and stays NUL-terminated. */
	{
		LightGrid *two = lightGridCreate(2, 1, 1);
		char buf[8];

		TEST_ASSERT_NOT_NULL(two);
		lightGridDumpSlice(two, 0, buf, 5);	/* "0000" + NUL */
		TEST_ASSERT_EQUAL_STRING("0000", buf);
		destroyLightGrid(two);
	}
}

/* --- 13. determinism ---------------------------------------------------- */

static void test_determinism(void)
{
	Voxmap *map = mkMap("11111\n11111\n11111\n11111\n11111\n");
	LightGrid *a = lightGridCreate(5, 5, 5);
	LightGrid *b = lightGridCreate(5, 5, 5);
	int x;
	int y;
	int z;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_NOT_NULL(b);
	lightGridSeedPoint(a, 2.5f, 1.5f, 2.5f, 200.0f, 100.0f, 50.0f, 0.0f);
	lightGridSeedPoint(b, 2.5f, 1.5f, 2.5f, 200.0f, 100.0f, 50.0f, 0.0f);
	lightGridPropagate(a, map);
	lightGridPropagate(b, map);

	for (z = 0; z < 5; z++) {
		for (y = 0; y < 5; y++) {
			for (x = 0; x < 5; x++) {
				TEST_ASSERT_EQUAL_INT(lightGridSkyAt(a, x, y, z),
						      lightGridSkyAt(b, x, y, z));
				TEST_ASSERT_EQUAL_INT(lightGridBlockAt(a, x, y, z, 0),
						      lightGridBlockAt(b, x, y, z, 0));
				TEST_ASSERT_EQUAL_INT(lightGridBlockAt(a, x, y, z, 1),
						      lightGridBlockAt(b, x, y, z, 1));
				TEST_ASSERT_EQUAL_INT(lightGridBlockAt(a, x, y, z, 2),
						      lightGridBlockAt(b, x, y, z, 2));
			}
		}
	}
	destroyVoxmap(map);
	destroyLightGrid(a);
	destroyLightGrid(b);
}

/* --- 14. dump format + combined query ---------------------------------- */

static void test_dump_format_and_combined(void)
{
	/* An enclosed room (sky 0) with a red point light along x. */
	LightGrid *g = lightGridCreate(3, 1, 3);
	uint8_t mask[3 * 1 * 3];
	char buf[64];
	uint8_t out[3];

	TEST_ASSERT_NOT_NULL(g);
	memset(mask, 0, sizeof(mask));
	setSolid(mask, 3, 3, 0, 0, 0);
	setSolid(mask, 3, 3, 1, 0, 0);
	setSolid(mask, 3, 3, 2, 0, 0);
	setSolid(mask, 3, 3, 0, 2, 0);
	setSolid(mask, 3, 3, 1, 2, 0);
	setSolid(mask, 3, 3, 2, 2, 0);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 0.0f, 0.0f, 0.0f);
	lightGridPropagateSolid(g, mask);

	TEST_ASSERT_EQUAL_INT(0, lightGridSkyAt(g, 1, 1, 0));
	lightGridAt(g, 1, 1, 0, out);
	TEST_ASSERT_EQUAL_INT(239, out[0]);
	TEST_ASSERT_EQUAL_INT(0, out[1]);
	TEST_ASSERT_EQUAL_INT(0, out[2]);

	/* Two hex digits per cell, x-major, newline per row. */
	lightGridDumpSlice(g, 1, buf, sizeof(buf));
	TEST_ASSERT_EQUAL_STRING("ffefdf\n", buf);

	/* Truncation stays NUL-terminated at a cell boundary. */
	lightGridDumpSlice(g, 1, buf, 4);
	TEST_ASSERT_EQUAL_STRING("ff", buf);

	/* Out-of-range slice / NULL grid leaves the buffer empty. */
	lightGridDumpSlice(g, 99, buf, sizeof(buf));
	TEST_ASSERT_EQUAL_STRING("", buf);
	lightGridDumpSlice(NULL, 0, buf, sizeof(buf));
	TEST_ASSERT_EQUAL_STRING("", buf);

	destroyLightGrid(g);
}

/* --- 15. sampling factor + face integration ---------------------------- */

/* The emitted top face (identified by its atlas UV, so the base-line sort of
 * the side faces cannot confuse the lookup). */
static const DrawItem *findTopFace(const DrawList *list)
{
	const float topUV[4][2] = ATLAS_UV_TOP;
	size_t i;

	for (i = 0; i < drawListCount(list); i++) {
		const DrawItem *item = drawListItem(list, i);

		if (memcmp(item->uv, topUV, sizeof(topUV)) == 0)
			return item;
	}
	return NULL;
}

static void test_factor_helper(void)
{
	LightGrid *g = lightGridCreate(3, 1, 1);
	uint8_t out[3];

	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 0.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagateSolid(g, NULL);	/* all air: sky 255 */

	/* sky 255 -> gained 76; block 64/48/32 -> combined 140/124/108. */
	lightGridFactorAt(g, 0, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(151, out[0]);
	lightGridFactorAt(g, 1, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(136, out[0]);
	lightGridFactorAt(g, 2, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(122, out[0]);
	/* Out of grid = open sky (no block light). */
	lightGridFactorAt(g, 5, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(93, out[0]);
	/* NULL grid = lighting disabled. */
	lightGridFactorAt(NULL, 0, 0, 0, out);
	TEST_ASSERT_EQUAL_INT(255, out[0]);
	lightGridFactorAt(g, 0, 0, 0, NULL);	/* no-op */
	destroyLightGrid(g);

	/* A dark (enclosed) air cell reads the ambient floor. */
	{
		LightGrid *d = lightGridCreate(1, 1, 3);
		uint8_t mask[3];

		memset(mask, 0, sizeof(mask));
		mask[gidx(1, 3, 0, 0, 0)] = 1;	/* y=0 solid */
		mask[gidx(1, 3, 0, 2, 0)] = 1;	/* y=2 solid */
		lightGridPropagateSolid(d, mask);
		lightGridFactorAt(d, 0, 1, 0, out);
		TEST_ASSERT_EQUAL_INT(LIGHT_AMBIENT, out[0]);
		destroyLightGrid(d);
	}
}

static void test_factor_sampled_into_top_face(void)
{
	Voxmap *map = mkMap("1\n");
	LightGrid *g = lightGridCreate(1, 1, 2);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 255.0f, 255.0f, 4.0f);
	lightGridPropagate(g, map);

	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	top = findTopFace(&list);
	TEST_ASSERT_NOT_NULL(top);
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(151, 151, 151, 255),
			      (int)top->tint);
	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

static void test_factor_coloured_light_into_face(void)
{
	Voxmap *map = mkMap("1\n");
	LightGrid *g = lightGridCreate(1, 1, 2);
	DrawList list;
	Camera3D cam;
	const DrawItem *top;

	TEST_ASSERT_NOT_NULL(map);
	TEST_ASSERT_NOT_NULL(g);
	lightGridSeedPoint(g, 0.5f, 1.5f, 0.5f, 255.0f, 0.0f, 0.0f, 4.0f);
	lightGridPropagate(g, map);

	initDrawList(&list, 16);
	initCamera3D(&cam);
	voxmapEmitFaces(map, NULL, g, &list, &cam,
			DRAW_TINT(255, 255, 255, 255));
	top = findTopFace(&list);
	TEST_ASSERT_NOT_NULL(top);
	/* R: sky 76 + 64 -> 151; G/B: sky only 76 -> 93. */
	TEST_ASSERT_EQUAL_INT((int)DRAW_TINT(151, 93, 93, 255),
			      (int)top->tint);
	destroyDrawList(&list);
	destroyLightGrid(g);
	destroyVoxmap(map);
}

void run_test_lightgrid(void);

void run_test_lightgrid(void)
{
	RUN_TEST(test_point_attenuation_rings);
	RUN_TEST(test_per_channel_independence);
	RUN_TEST(test_max_merge);
	RUN_TEST(test_blocking_and_corner_bending);
	RUN_TEST(test_sky_open_field);
	RUN_TEST(test_sky_roof_gradient);
	RUN_TEST(test_sky_shaft);
	RUN_TEST(test_sky_enclosed_room);
	RUN_TEST(test_spot_pool_and_falloff);
	RUN_TEST(test_spot_los_blocked);
	RUN_TEST(test_radius_clamp);
	RUN_TEST(test_stale_and_combined_saturation);
	RUN_TEST(test_bounds_and_null);
	RUN_TEST(test_determinism);
	RUN_TEST(test_dump_format_and_combined);
	RUN_TEST(test_factor_helper);
	RUN_TEST(test_factor_sampled_into_top_face);
	RUN_TEST(test_factor_coloured_light_into_face);
}
