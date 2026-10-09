#ifndef ISOMATA_RENDER_LIGHTGRID_H
#define ISOMATA_RENDER_LIGHTGRID_H

/*
 * Voxel light grid (pure: voxmap + libc/libm only, no SDL).
 *
 * Two parallel grids over a w (x) * d (z) * h (y) volume of cells:
 *
 *   - a sky scalar (uint8 per cell), and
 *   - a block RGB light (3 uint8 per cell).
 *
 * They are kept separate so a later day/night tint can scale the sky term
 * without touching coloured lamps, and so indoor/outdoor can be read from the
 * sky term alone. lightGridAt() combines them (sky as white + block, per
 * channel saturated) for the sampling layer.
 *
 * SOLIDITY. A cell is solid when the caller's solid volume marks it solid.
 * lightGridPropagate() derives that volume from a Voxmap: solid iff
 * y < voxmapHeightAt(map, x, z), so a void column (or a column shorter than y)
 * is air. lightGridPropagateSolid() takes a caller-built volume, which is how
 * overhangs/ceilings that the ASCII heightmap cannot express are modelled.
 * Light lives only in air; a solid cell's stored value stays 0 and light never
 * crosses it.
 *
 * PROPAGATION. Light is seeded (sky from the volume, block from the emitters),
 * then flood-filled over the 6 air neighbours. Each step is per-channel
 * max(0, value - LIGHT_ATTEN) merged into the neighbour as a per-channel MAX
 * (never a sum): two overlapping lights take the brighter channel, so red over
 * green reads yellow and cannot blow out. The fill is deterministic and
 * allocates nothing after lightGridCreate().
 *
 * SKY RULE. A cell is *sky-open* when it is air and every cell above it in its
 * column is air. Every sky-open cell holds LIGHT_SKY_FULL (the "straight down,
 * zero attenuation" shaft/field case is exactly "all cells in an open column
 * are sky-open"), and the sky then spreads from there into the remaining air
 * with the normal LIGHT_ATTEN rule. Under a ceiling the cells below are not
 * sky-open, so they see only the attenuated spill from the nearest opening; a
 * fully enclosed air pocket sees no sky at all.
 *
 * TUNING CONSTANTS (255-scale):
 *   LIGHT_ATTEN   per air step, per channel, subtracted from the light. The
 *                 range of a full-strength (255) source is 255/16 ~= 15.9
 *                 cells, so LIGHT_ATTEN is the single "how far does light
 *                 reach" knob.
 *   LIGHT_SKY_FULL the value of a sky-open cell.
 *   LIGHT_AMBIENT a constant floor the sampling layer (T15) adds to every
 *                 face's light; defined here so the tuning lives in one place.
 *
 * INDEX ORDER. Cells are addressed (x, y, z) with x fastest, then y, then z:
 * index = (z * h + y) * w + x. A caller-built solid volume uses the same order.
 * Out-of-bounds queries return 0; NULL grid/out arguments are no-ops.
 */

#include "render/voxmap.h"

#include <stddef.h>
#include <stdint.h>

/* Per-step, per-channel attenuation of a 255-scale light value. */
#define LIGHT_ATTEN 16
/* Value of a sky-open cell (see the sky rule above). */
#define LIGHT_SKY_FULL 255
/* Sampling floor added to every face's light (used by T15, defined here). */
#define LIGHT_AMBIENT 24
/* Sky gain, percent. The factor helper scales the sky term by this before
 * combining it with block light: a fully sky-open world would otherwise
 * saturate the factor at 255 and a lamp could never read above the uniform
 * skylight. 100 = no gain (sky saturates). The ASCII heightmap has no
 * overhangs, so sky is uniform and this gain IS the daylight level. */
#define LIGHT_SKY_GAIN_PCT 30

/* Dimension / volume guards: a bad size must fail cleanly rather than force a
 * huge allocation. The map is at most 256 x 256 x 10, so these are generous. */
#define LIGHTGRID_MAX_DIM 4096
#define LIGHTGRID_MAX_CELLS (1u << 20)

#ifndef ISOMATA_LIGHTGRID_TYPEDEF
#define ISOMATA_LIGHTGRID_TYPEDEF
typedef struct LightGrid LightGrid;
#endif

/* Create a grid of w (x) * d (z) * h (y) cells, all dark. Returns NULL for a
 * non-positive dimension, an over-large dimension, a volume over
 * LIGHTGRID_MAX_CELLS, or an allocation failure. */
LightGrid *lightGridCreate(int w, int d, int h);

/* Release a grid. NULL is a no-op. */
void destroyLightGrid(LightGrid *grid);

/* Grid dimensions (0 for a NULL grid). */
int lightGridWidth(const LightGrid *grid);
int lightGridDepth(const LightGrid *grid);
int lightGridHeight(const LightGrid *grid);

/* Sky / block channel (0 = R, 1 = G, 2 = B) at (x, y, z); 0 for out of bounds,
 * a NULL grid, or a bad channel. */
uint8_t lightGridSkyAt(const LightGrid *grid, int x, int y, int z);
uint8_t lightGridBlockAt(const LightGrid *grid, int x, int y, int z,
			 int channel);

/* Combined light at (x, y, z): out[c] = min(255, sky + block[c]). Out of
 * bounds or a NULL grid writes {0, 0, 0}; a NULL out is a no-op. */
void lightGridAt(const LightGrid *grid, int x, int y, int z, uint8_t out[3]);

/* Per-channel 0..255 brightness factor for the air cell a face looks across
 * (the flat per-face sampling of T14; T15 replaces it with per-corner
 * smoothing). The sky term is scaled by LIGHT_SKY_GAIN_PCT and added to the
 * block channel (saturated at 255), then mapped through the ambient floor:
 *   combined = min(255, sky * LIGHT_SKY_GAIN_PCT / 100 + block[c])
 *   out[c]   = LIGHT_AMBIENT + (combined * (255 - LIGHT_AMBIENT) + 127) / 255
 * (integer arithmetic; the +127 makes the division round to nearest, half up,
 * which the tests pin). So a fully dark cell reads LIGHT_AMBIENT and a full
 * cell reads 255. A NULL grid writes {255, 255, 255} (lighting disabled); an
 * out-of-grid cell writes the open-sky factor (a sky-open cell with no block
 * light), so map-edge faces match void-neighbour faces. A NULL out is a
 * no-op. */
void lightGridFactorAt(const LightGrid *grid, int x, int y, int z,
		       uint8_t out[3]);

/* Flood-fill the sky and block grids. The solid volume is rebuilt from `map`
 * (NULL = every cell air). Existing block seeds are kept and merged with the
 * fill; the sky grid is recomputed from scratch, so calling this twice with the
 * same map is idempotent. No-op for a NULL grid. */
void lightGridPropagate(LightGrid *grid, const Voxmap *map);

/* As lightGridPropagate, but the solid volume is the caller's: `solid` is
 * w * d * h bytes in the grid index order, non-zero = solid, or NULL for every
 * cell air. This is how ceilings/overhangs are modelled. */
void lightGridPropagateSolid(LightGrid *grid, const uint8_t *solid);

/* Seed a point light at the cell containing world point (x, y, z). Each channel
 * is capped at min(channel, radius * LIGHT_ATTEN) when radius > 0 (so radius 10
 * reaches ~10 cells before the fill attenuates it to 0); radius <= 0 keeps the
 * full channel values and leaves only the global attenuation. Merges per
 * channel with any existing value. No-op for a NULL grid, a non-finite/absent
 * point, or a point outside the grid. The seed lands on the cell containing
 * (x, y, z) whether or not that cell is solid (the emitter is a point in space,
 * not a surface); the fill then carries the light out into the surrounding air
 * and it never crosses a solid cell. */
void lightGridSeedPoint(LightGrid *grid, float x, float y, float z, float r,
			float g, float b, float radius);

/* Seed a cone (spot) light with apex (x, y, z) and axis `dir` (normalised
 * internally). Every air cell whose centre lies within `radius` of the apex and
 * within `halfAngleDeg` of the axis, with an unobstructed voxel line of sight
 * (a DDA from the apex; solid cells in `map` block), is seeded with
 *   value = channel * distanceFalloff * angleFalloff
 * where both falloffs are linear (1 at the apex/axis, 0 at radius/half-angle).
 * The fill then softens the cone edges. Merges per channel. No-op for a NULL
 * grid, a NULL/zero `dir`, radius <= 0, halfAngleDeg <= 0, or an apex outside
 * the grid (the same float-space bounds rule lightGridSeedPoint uses; this also
 * keeps the LOS DDA's float->int casts in range for any finite input). `map`
 * supplies the LOS solidity (NULL = no blockers). */
void lightGridSeedSpot(LightGrid *grid, const Voxmap *map, float x, float y,
		       float z, const float dir[3], float halfAngleDeg, float r,
		       float g, float b, float radius);

/* Write an ASCII hex dump of the horizontal slice at height `y`: two lowercase
 * hex digits per cell (the brightest combined channel, 0..255), row-major in x,
 * one '\n' after each row (including the last), always NUL-terminated. A NULL
 * grid, an out-of-range y, a NULL buf, or n == 0 leaves buf empty. If `n` is
 * too small the output is truncated at a cell/row boundary and still
 * NUL-terminated. */
void lightGridDumpSlice(const LightGrid *grid, int y, char *buf, size_t n);

#endif /* ISOMATA_RENDER_LIGHTGRID_H */
