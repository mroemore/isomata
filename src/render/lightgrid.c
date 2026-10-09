/*
 * Light grid (see lightgrid.h for the contract). Pure: voxmap + libc/libm
 * only, no SDL.
 *
 * Storage is one set of allocations at create and nothing per step: the sky
 * plane, the three block planes, a scratch solid volume, and a bucket-queue
 * entry pool. The fill is a Dial-style bucket flood: every cell is inserted at
 * most twice (once by the initial scan at its seeded value, once when a
 * brighter neighbour relaxes it), so a pool of 2 * cells is a hard bound and
 * the scan-then-flood order makes the result independent of seed order.
 */

#include "render/lightgrid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f
#define LIGHT_BUCKETS 256
#define LIGHT_EPS 1e-6f
/* LOS DDA safety bound (the ray always reaches the target cell first). */
#define LIGHT_DDA_MAX_STEPS 100000

struct LightGrid {
	int w;
	int d;
	int h;
	size_t n;
	uint8_t *sky;		/* n */
	uint8_t *block;		/* 3n, planes R, G, B */
	uint8_t *solid;		/* n scratch: 0 air, 1 solid */
	int32_t *bucketCell;	/* 2n entries */
	int32_t *bucketNext;	/* 2n entries */
	int32_t bucketHead[LIGHT_BUCKETS];
	size_t poolUsed;
	size_t poolCap;
};

/* x fastest, then y, then z (see lightgrid.h). */
static size_t cellIndex(const LightGrid *g, int x, int y, int z)
{
	return (size_t)((z * g->h + y) * g->w + x);
}

static bool cellInBounds(const LightGrid *g, int x, int y, int z)
{
	return x >= 0 && y >= 0 && z >= 0 && x < g->w && y < g->h && z < g->d;
}

LightGrid *lightGridCreate(int w, int d, int h)
{
	LightGrid *g;
	uint64_t n64;
	size_t n;

	if (w <= 0 || d <= 0 || h <= 0)
		return NULL;
	if (w > LIGHTGRID_MAX_DIM || d > LIGHTGRID_MAX_DIM ||
	    h > LIGHTGRID_MAX_DIM)
		return NULL;
	n64 = (uint64_t)w * (uint64_t)d * (uint64_t)h;
	if (n64 > LIGHTGRID_MAX_CELLS)
		return NULL;
	n = (size_t)n64;

	g = calloc(1, sizeof(*g));
	if (g == NULL)
		return NULL;
	g->sky = calloc(n, 1);
	g->block = calloc(n * 3, 1);
	g->solid = calloc(n, 1);
	g->poolCap = n * 2;
	g->bucketCell = malloc(g->poolCap * sizeof(*g->bucketCell));
	g->bucketNext = malloc(g->poolCap * sizeof(*g->bucketNext));
	if (g->sky == NULL || g->block == NULL || g->solid == NULL ||
	    g->bucketCell == NULL || g->bucketNext == NULL) {
		free(g->sky);
		free(g->block);
		free(g->solid);
		free(g->bucketCell);
		free(g->bucketNext);
		free(g);
		return NULL;
	}
	g->w = w;
	g->d = d;
	g->h = h;
	g->n = n;
	return g;
}

void destroyLightGrid(LightGrid *grid)
{
	if (grid == NULL)
		return;
	free(grid->sky);
	free(grid->block);
	free(grid->solid);
	free(grid->bucketCell);
	free(grid->bucketNext);
	free(grid);
}

int lightGridWidth(const LightGrid *grid)
{
	return grid == NULL ? 0 : grid->w;
}

int lightGridDepth(const LightGrid *grid)
{
	return grid == NULL ? 0 : grid->d;
}

int lightGridHeight(const LightGrid *grid)
{
	return grid == NULL ? 0 : grid->h;
}

uint8_t lightGridSkyAt(const LightGrid *grid, int x, int y, int z)
{
	if (grid == NULL || !cellInBounds(grid, x, y, z))
		return 0;
	return grid->sky[cellIndex(grid, x, y, z)];
}

uint8_t lightGridBlockAt(const LightGrid *grid, int x, int y, int z,
			 int channel)
{
	if (grid == NULL || channel < 0 || channel > 2 ||
	    !cellInBounds(grid, x, y, z))
		return 0;
	return grid->block[(size_t)channel * grid->n +
			   cellIndex(grid, x, y, z)];
}

void lightGridAt(const LightGrid *grid, int x, int y, int z, uint8_t out[3])
{
	size_t idx;
	int c;

	if (out == NULL)
		return;
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	if (grid == NULL || !cellInBounds(grid, x, y, z))
		return;
	idx = cellIndex(grid, x, y, z);
	for (c = 0; c < 3; c++) {
		int v = (int)grid->sky[idx] +
			(int)grid->block[(size_t)c * grid->n + idx];

		out[c] = (uint8_t)(v > 255 ? 255 : v);
	}
}

/* Map a 0..255 combined light to the ambient-floored brightness factor. The
 * caller only passes v >= 0 (sky and block are unsigned), so there is no lower
 * clamp; v is capped at 255 before the map. */
static uint8_t factorFromCombined(int v)
{
	if (v > 255)
		v = 255;
	return (uint8_t)(LIGHT_AMBIENT +
			 (v * (255 - LIGHT_AMBIENT) + 127) / 255);
}

void lightGridFactorAt(const LightGrid *grid, int x, int y, int z,
		       uint8_t out[3])
{
	int c;
	int skyFull;

	if (out == NULL)
		return;
	if (grid == NULL) {
		out[0] = 255;
		out[1] = 255;
		out[2] = 255;
		return;
	}
	if (!cellInBounds(grid, x, y, z)) {
		/* Beyond the grid is open sky: a sky-open cell, no block light. */
		skyFull = factorFromCombined(LIGHT_SKY_FULL *
					     LIGHT_SKY_GAIN_PCT / 100);
		out[0] = (uint8_t)skyFull;
		out[1] = (uint8_t)skyFull;
		out[2] = (uint8_t)skyFull;
		return;
	}
	{
		size_t idx = cellIndex(grid, x, y, z);
		int sky = (int)grid->sky[idx] * LIGHT_SKY_GAIN_PCT / 100;

		for (c = 0; c < 3; c++) {
			int v = sky +
				(int)grid->block[(size_t)c * grid->n + idx];

			out[c] = factorFromCombined(v);
		}
	}
}

/* --- smooth per-corner sampling and ambient occlusion (T15) ------------ */

bool lightGridSolidAt(const LightGrid *grid, int x, int y, int z)
{
	if (grid == NULL)
		return false;
	if (y < 0)
		return true;	/* below the world: ground */
	if (!cellInBounds(grid, x, y, z))
		return false;	/* x/z out of bounds or above the grid: sky */
	return grid->solid[cellIndex(grid, x, y, z)] != 0;
}

int lightGridAoPercent(int count)
{
	if (count <= 0)
		return LIGHT_AO_PCT_0;
	if (count == 1)
		return LIGHT_AO_PCT_1;
	if (count == 2)
		return LIGHT_AO_PCT_2;
	return LIGHT_AO_PCT_3;
}

void lightGridCornerAverage(const LightGrid *grid, const int cells[4][3],
			    int ownIndex, uint8_t out[3])
{
	int sum[3] = { 0, 0, 0 };
	int n = 0;
	int i;
	int c;
	uint8_t f[3];

	if (out == NULL || cells == NULL)
		return;
	if (grid == NULL) {
		out[0] = 255;
		out[1] = 255;
		out[2] = 255;
		return;
	}
	if (ownIndex < 0 || ownIndex > 3)
		ownIndex = 0;
	for (i = 0; i < 4; i++) {
		if (lightGridSolidAt(grid, cells[i][0], cells[i][1],
				     cells[i][2]))
			continue;	/* solid: not a light source */
		lightGridFactorAt(grid, cells[i][0], cells[i][1], cells[i][2],
				  f);
		for (c = 0; c < 3; c++)
			sum[c] += f[c];
		n++;
	}
	if (n == 0) {
		/* Every cell solid: fall back to the corner's own air cell (its
		 * stored value is what a solid cell would read). */
		lightGridFactorAt(grid, cells[ownIndex][0], cells[ownIndex][1],
				  cells[ownIndex][2], out);
		return;
	}
	for (c = 0; c < 3; c++)
		out[c] = (uint8_t)((sum[c] + n / 2) / n);
}

int lightGridCornerOcclusion(const LightGrid *grid, const int cells[4][3],
			     int ownIndex)
{
	int count = 0;
	int i;

	if (grid == NULL || cells == NULL)
		return 0;
	if (ownIndex < 0 || ownIndex > 3)
		ownIndex = 0;
	for (i = 0; i < 4; i++) {
		if (i == ownIndex)
			continue;
		if (lightGridSolidAt(grid, cells[i][0], cells[i][1],
				     cells[i][2]))
			count++;
	}
	return count;
}

/* --- bucket flood ------------------------------------------------------ */

static void bucketReset(LightGrid *g)
{
	int i;

	for (i = 0; i < LIGHT_BUCKETS; i++)
		g->bucketHead[i] = -1;
	g->poolUsed = 0;
}

/* Push one entry. The pool never overflows: the initial scan inserts each cell
 * at most once and a cell is relaxed (inserted) at most once more, so the
 * total is <= 2n = poolCap. */
static void bucketPush(LightGrid *g, int value, size_t cell)
{
	int32_t e;

	if (g->poolUsed >= g->poolCap)
		return;		/* provably unreachable; never corrupt state */
	e = (int32_t)g->poolUsed++;
	g->bucketCell[e] = (int32_t)cell;
	g->bucketNext[e] = g->bucketHead[value];
	g->bucketHead[value] = e;
}

static void relaxCell(LightGrid *g, uint8_t *field, size_t nc, int cand)
{
	if (g->solid[nc])
		return;
	if (cand > field[nc]) {
		field[nc] = (uint8_t)cand;
		bucketPush(g, cand, nc);
	}
}

/* Dial flood over one scalar plane: seed every lit cell into its value bucket,
 * then process buckets from bright to dark. A cell's first extraction is its
 * final value (all brighter paths were processed already), so each cell is
 * relaxed at most once. */
static void floodField(LightGrid *g, uint8_t *field)
{
	size_t i;
	int b;

	bucketReset(g);
	for (i = 0; i < g->n; i++) {
		if (field[i] > 0)
			bucketPush(g, field[i], i);
	}
	for (b = LIGHT_BUCKETS - 1; b >= 0; b--) {
		int32_t e;

		while ((e = g->bucketHead[b]) != -1) {
			size_t c = (size_t)g->bucketCell[e];
			size_t row = (size_t)g->w * (size_t)g->h;
			int x;
			int y;
			int z;
			int cand;

			g->bucketHead[b] = g->bucketNext[e];
			if (field[c] != (uint8_t)b)
				continue;	/* stale: a brighter path won */
			cand = (b >= LIGHT_ATTEN) ? b - LIGHT_ATTEN : 0;
			if (cand == 0)
				continue;	/* light stops at 0 */
			x = (int)(c % (size_t)g->w);
			y = (int)((c / (size_t)g->w) % (size_t)g->h);
			z = (int)(c / row);
			if (x + 1 < g->w)
				relaxCell(g, field, c + 1, cand);
			if (x > 0)
				relaxCell(g, field, c - 1, cand);
			if (y + 1 < g->h)
				relaxCell(g, field, c + (size_t)g->w, cand);
			if (y > 0)
				relaxCell(g, field, c - (size_t)g->w, cand);
			if (z + 1 < g->d)
				relaxCell(g, field, c + row, cand);
			if (z > 0)
				relaxCell(g, field, c - row, cand);
		}
	}
}

/* Sky-open cells (air with all cells above air) are exactly the air cells down
 * to the highest solid in each column. Seed them full; the flood then spreads
 * the attenuated spill under any ceiling. */
static void seedSky(LightGrid *g)
{
	int x;
	int y;
	int z;

	for (z = 0; z < g->d; z++) {
		for (x = 0; x < g->w; x++) {
			for (y = g->h - 1; y >= 0; y--) {
				size_t idx = cellIndex(g, x, y, z);

				if (g->solid[idx])
					break;
				g->sky[idx] = LIGHT_SKY_FULL;
			}
		}
	}
}

static void propagateInternal(LightGrid *g)
{
	memset(g->sky, 0, g->n);
	seedSky(g);
	floodField(g, g->sky);
	floodField(g, g->block);
	floodField(g, g->block + g->n);
	floodField(g, g->block + 2 * g->n);
}

/* Rebuild the solid volume from the map's occupancy grid. For a
 * single-section heightmap this is exactly "y < voxmapHeightAt(x, z)" (the old
 * rule), so single-section lighting is unchanged; a slice map's overhangs and
 * interiors are honoured instead of being flattened to a column height. */
static void buildSolidFromMap(LightGrid *g, const Voxmap *map)
{
	int x;
	int y;
	int z;

	for (z = 0; z < g->d; z++) {
		for (x = 0; x < g->w; x++) {
			for (y = 0; y < g->h; y++)
				g->solid[cellIndex(g, x, y, z)] =
					voxmapSolidAt(map, x, y, z) ? 1 : 0;
		}
	}
}

void lightGridPropagate(LightGrid *grid, const Voxmap *map)
{
	if (grid == NULL)
		return;
	buildSolidFromMap(grid, map);
	propagateInternal(grid);
}

void lightGridPropagateSolid(LightGrid *grid, const uint8_t *solid)
{
	if (grid == NULL)
		return;
	if (solid != NULL)
		memcpy(grid->solid, solid, grid->n);
	else
		memset(grid->solid, 0, grid->n);
	propagateInternal(grid);
}

/* --- emitters ---------------------------------------------------------- */

static uint8_t clampU8(float v)
{
	if (!(v > 0.0f))
		return 0;
	if (v >= 255.0f)
		return 255;
	return (uint8_t)(v + 0.5f);
}

static void seedChannel(LightGrid *g, size_t idx, int channel, uint8_t value,
			uint8_t cap)
{
	uint8_t v = value < cap ? value : cap;
	uint8_t *slot = &g->block[(size_t)channel * g->n + idx];

	if (v > *slot)
		*slot = v;
}

/* True when (x, y, z) is finite and the cell containing it is inside the grid.
 * Both emitters reject an origin failing this in float space, before any
 * float->int cast: a huge (or non-finite) coordinate would be UB to cast, and
 * an emitter origin outside the grid seeds nothing. A spot apex outside the
 * grid is therefore malformed too (the same rule as a point). */
static bool originInGrid(const LightGrid *grid, float x, float y, float z)
{
	return isfinite(x) && isfinite(y) && isfinite(z) &&
	       x >= 0.0f && y >= 0.0f && z >= 0.0f &&
	       x < (float)grid->w && y < (float)grid->h &&
	       z < (float)grid->d;
}

void lightGridSeedPoint(LightGrid *grid, float x, float y, float z, float r,
			float g, float b, float radius)
{
	int cx;
	int cy;
	int cz;
	uint8_t cap;
	size_t idx;

	if (grid == NULL)
		return;
	if (!originInGrid(grid, x, y, z))
		return;
	cx = (int)floorf(x);
	cy = (int)floorf(y);
	cz = (int)floorf(z);
	cap = (radius > 0.0f) ? clampU8(radius * (float)LIGHT_ATTEN) : 255;
	idx = cellIndex(grid, cx, cy, cz);
	seedChannel(grid, idx, 0, clampU8(r), cap);
	seedChannel(grid, idx, 1, clampU8(g), cap);
	seedChannel(grid, idx, 2, clampU8(b), cap);
}

/* True when (x, y, z) is a solid voxel of `map` (a NULL/OOB/void cell is
 * air). Occupancy-based, so a slice map's overhangs block a spot's line of
 * sight correctly; for a single-section map it matches the old
 * "y < voxmapHeightAt" rule exactly. */
static bool mapCellSolid(const Voxmap *map, int x, int y, int z)
{
	return voxmapSolidAt(map, x, y, z);
}

/* Amanatides-Woo voxel DDA from (ax, ay, az) to the centre of (tx, ty, tz).
 * Returns false when a solid map cell strictly between the endpoints blocks.
 *
 * Precondition: the apex is finite and inside the grid (lightGridSeedSpot
 * rejects anything else before calling), and the target is a grid cell. The
 * start cell is therefore in [0, dim) and each step moves cx/cy/cz by at most
 * one, so with the step guard the integer walk cannot overflow int. */
static bool losClear(const Voxmap *map, float ax, float ay, float az, int tx,
		     int ty, int tz)
{
	int cx = (int)floorf(ax);
	int cy = (int)floorf(ay);
	int cz = (int)floorf(az);
	float dx = ((float)tx + 0.5f) - ax;
	float dy = ((float)ty + 0.5f) - ay;
	float dz = ((float)tz + 0.5f) - az;
	int stepX = (dx > 0.0f) - (dx < 0.0f);
	int stepY = (dy > 0.0f) - (dy < 0.0f);
	int stepZ = (dz > 0.0f) - (dz < 0.0f);
	float tDeltaX;
	float tDeltaY;
	float tDeltaZ;
	float tMaxX;
	float tMaxY;
	float tMaxZ;
	int guard = 0;

	if (dx == 0.0f) {
		tDeltaX = INFINITY;
		tMaxX = INFINITY;
	} else {
		tDeltaX = fabsf(1.0f / dx);
		tMaxX = ((dx > 0.0f) ? (floorf(ax) + 1.0f - ax)
				     : (ax - floorf(ax))) * tDeltaX;
	}
	if (dy == 0.0f) {
		tDeltaY = INFINITY;
		tMaxY = INFINITY;
	} else {
		tDeltaY = fabsf(1.0f / dy);
		tMaxY = ((dy > 0.0f) ? (floorf(ay) + 1.0f - ay)
				     : (ay - floorf(ay))) * tDeltaY;
	}
	if (dz == 0.0f) {
		tDeltaZ = INFINITY;
		tMaxZ = INFINITY;
	} else {
		tDeltaZ = fabsf(1.0f / dz);
		tMaxZ = ((dz > 0.0f) ? (floorf(az) + 1.0f - az)
				     : (az - floorf(az))) * tDeltaZ;
	}

	while ((cx != tx || cy != ty || cz != tz) &&
	       guard++ < LIGHT_DDA_MAX_STEPS) {
		if (tMaxX < tMaxY && tMaxX < tMaxZ) {
			cx += stepX;
			tMaxX += tDeltaX;
		} else if (tMaxY < tMaxZ) {
			cy += stepY;
			tMaxY += tDeltaY;
		} else {
			cz += stepZ;
			tMaxZ += tDeltaZ;
		}
		if (cx == tx && cy == ty && cz == tz)
			break;
		if (mapCellSolid(map, cx, cy, cz))
			return false;
	}
	return true;
}

/* Integer cell range [lo, hi] on one axis covered by [c - radius, c + radius],
 * clamped to [0, n - 1]. Precondition: c is finite and in [0, n) -- the caller
 * rejects an out-of-grid apex -- so floorf(c) is in [0, n-1], the range always
 * contains it, and lo <= hi (never empty). Safe for any radius: the clamps keep
 * both float->int casts in range. */
static void axisRange(float c, float radius, int n, int *lo, int *hi)
{
	float a = floorf(c) - radius;
	float b = floorf(c) + radius;

	if (a < 0.0f)
		a = 0.0f;
	if (b > (float)(n - 1))
		b = (float)(n - 1);
	*lo = (int)a;
	*hi = (int)b;
}

void lightGridSeedSpot(LightGrid *grid, const Voxmap *map, float x, float y,
		       float z, const float dir[3], float halfAngleDeg, float r,
		       float g, float b, float radius)
{
	float len;
	float ux;
	float uy;
	float uz;
	float cosHalf;
	float angleRad;
	int cx;
	int cy;
	int cz;
	int minX;
	int maxX;
	int minY;
	int maxY;
	int minZ;
	int maxZ;

	if (grid == NULL || dir == NULL)
		return;
	if (!(radius > 0.0f) || !(halfAngleDeg > 0.0f))
		return;
	if (!originInGrid(grid, x, y, z))
		return;
	len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(len > 0.0f) || !isfinite(len))
		return;
	ux = dir[0] / len;
	uy = dir[1] / len;
	uz = dir[2] / len;
	angleRad = halfAngleDeg * (PI_F / 180.0f);
	cosHalf = cosf(angleRad);

	axisRange(x, radius, grid->w, &minX, &maxX);
	axisRange(y, radius, grid->h, &minY, &maxY);
	axisRange(z, radius, grid->d, &minZ, &maxZ);

	for (cz = minZ; cz <= maxZ; cz++) {
		for (cy = minY; cy <= maxY; cy++) {
			for (cx = minX; cx <= maxX; cx++) {
				float ex = (float)cx + 0.5f;
				float ey = (float)cy + 0.5f;
				float ez = (float)cz + 0.5f;
				float dx = ex - x;
				float dy = ey - y;
				float dz = ez - z;
				float dist = sqrtf(dx * dx + dy * dy + dz * dz);
				float cosA;
				float distFall;
				float angleFall;
				float fall;

				if (dist > radius)
					continue;
				if (mapCellSolid(map, cx, cy, cz))
					continue;
				if (dist > LIGHT_EPS) {
					cosA = (dx * ux + dy * uy + dz * uz) /
					       dist;
					if (cosA < cosHalf)
						continue;
					if (cosA > 1.0f)
						cosA = 1.0f;
				} else {
					cosA = 1.0f;
				}
				if (!losClear(map, x, y, z, cx, cy, cz))
					continue;
				distFall = 1.0f - dist / radius;
				angleFall = 1.0f - acosf(cosA) / angleRad;
				if (angleFall < 0.0f)
					angleFall = 0.0f;
				fall = distFall * angleFall;
				seedChannel(grid, cellIndex(grid, cx, cy, cz), 0,
					    clampU8(r * fall), 255);
				seedChannel(grid, cellIndex(grid, cx, cy, cz), 1,
					    clampU8(g * fall), 255);
				seedChannel(grid, cellIndex(grid, cx, cy, cz), 2,
					    clampU8(b * fall), 255);
			}
		}
	}
}

/* --- debug dump -------------------------------------------------------- */

void lightGridDumpSlice(const LightGrid *grid, int y, char *buf, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	size_t pos = 0;
	int x;
	int z;

	if (buf == NULL || n == 0)
		return;
	buf[0] = '\0';
	if (grid == NULL || y < 0 || y >= grid->h)
		return;
	for (z = 0; z < grid->d; z++) {
		for (x = 0; x < grid->w; x++) {
			size_t idx = cellIndex(grid, x, y, z);
			int intensity = 0;
			int c;

			if (pos + 3 > n) {
				buf[pos] = '\0';
				return;
			}
			for (c = 0; c < 3; c++) {
				int v = (int)grid->sky[idx] +
					(int)grid->block[(size_t)c * grid->n +
							 idx];

				if (v > 255)
					v = 255;
				if (v > intensity)
					intensity = v;
			}
			buf[pos++] = hex[(intensity >> 4) & 0xf];
			buf[pos++] = hex[intensity & 0xf];
		}
		if (pos + 2 > n) {
			buf[pos] = '\0';
			return;
		}
		buf[pos++] = '\n';
	}
	buf[pos] = '\0';
}
