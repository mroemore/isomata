#ifndef ISOMATA_RENDER_MAPSOURCE_H
#define ISOMATA_RENDER_MAPSOURCE_H

/*
 * PNG slice map source (pure). No SDL: the glue (render/map_loader.c) decodes
 * the PNGs with SDL_LoadPNG and hands the raw RGBA pixels here, so everything
 * about slice ORDER, colour -> material lookup and slice STACKING is
 * headless-testable.
 *
 * FORMAT (see docs/map-authoring.md). A map directory holds:
 *   - NN.png   one PNG per horizontal slice. Natural sort of the file names
 *              gives the slice order; ascending = bottom (y 0) first. Every
 *              PNG must be the same width x height; a pixel is one voxel:
 *              alpha == 0 is AIR, any other alpha is SOLID (a fully opaque
 *              colour is the authoring rule; a partial alpha is treated as
 *              solid, so do not rely on it).
 *   - legend.txt  the colour legend + optional lights:
 *       #RRGGBB <material>   map an exact RGB (case-insensitive hex) to a
 *                            material name. A duplicate colour is a diagnostic
 *                            and the LAST line wins. A malformed hex or an
 *                            unresolvable material name is a diagnostic and the
 *                            line is skipped (it never fails the load).
 *       $ point ... / $ spot ...   lights, the SAME grammar as the ASCII
 *                            parser (voxmap.h::voxmapParseLightLine).
 *     An opaque pixel whose colour has no legend entry is a diagnostic and is
 *     treated as air (and counted in MapSourceStats.unknownColorVoxels).
 *     There is no manifest: natural sort only. A manifest is a documented
 *     future extension if a naming convention is ever not enough.
 *
 * The assembler produces exactly the Voxmap the ASCII slice path produces
 * (same occupancy, per-voxel materials and lights), so everything downstream
 * (emission, lighting) is format-agnostic. This is pinned by the equivalence
 * test (tests/test_pngmap.c) against assets/maps/demo.txt.
 *
 * BOUNDS. width/height/levels are each <= VOXMAP_MAX_DIM and
 * width*height*levels <= VOXMAP_MAX_CELLS (see voxmap.h), so a bad directory
 * fails cleanly rather than forcing a huge allocation. At most
 * MAPSOURCE_MAX_COLORS legend colours are kept (extras are a diagnostic).
 */

#include "render/materials.h"
#include "render/voxmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Legend colours kept (extras are counted in MapSourceLegend.overflowColors). */
#define MAPSOURCE_MAX_COLORS 64

/* One decoded RGBA8 slice image. `rgba` is width*height*4 bytes, row-major,
 * top row = z 0, pixel (x, z) at offset z*pitch + x*4; `pitch` >= width*4. */
typedef struct MapSourceImage {
	int width;
	int height;
	const uint8_t *rgba;
	size_t pitch;
} MapSourceImage;

/* One colour -> material entry. `material` is a material id (0 is valid). */
typedef struct MapSourceColor {
	uint32_t rgb;		/* 0xRRGGBB */
	int16_t material;
} MapSourceColor;

/* A parsed legend.txt. Malformed lines never fail the parse; they are counted
 * here (a diagnostic is printed for each). */
typedef struct MapSourceLegend {
	MapSourceColor colors[MAPSOURCE_MAX_COLORS];
	int colorCount;
	VoxmapLight lights[VOXMAP_MAX_LIGHTS];
	int lightCount;
	int badLines;		/* unrecognised / malformed lines skipped */
	int duplicateColors;	/* duplicate colour lines (last wins) */
	int unknownMaterials;	/* colour lines naming an unknown material */
	int overflowColors;	/* colour lines past MAPSOURCE_MAX_COLORS */
	int overflowLights;	/* light lines past VOXMAP_MAX_LIGHTS */
} MapSourceLegend;

/* Diagnostics from one assembly. */
typedef struct MapSourceStats {
	int levels;		/* slices stacked */
	int solidVoxels;	/* solid voxels written */
	int unknownColorVoxels;	/* opaque voxels whose colour was not in the legend
				 * (treated as air) */
} MapSourceStats;

/* Natural-sort comparator for file names: a run of digits compares by NUMERIC
 * value, so "9.png" < "10.png". Runs with equal value order by fewer leading
 * zeros first ("1.png" < "01.png"); every other byte compares by unsigned
 * value. Returns <0, 0 or >0 like strcmp. A NULL argument sorts last. */
int mapSourceNaturalCompare(const char *a, const char *b);

/* Parse legend.txt text (need not be NUL-terminated). `materials` resolves
 * material names (NULL resolves every name to id 0, like the ASCII parser).
 * Returns false only for NULL `text`/`out`; malformed content is skipped with
 * a diagnostic and counted. */
bool mapSourceLegendParse(const char *text, size_t length,
			  const MaterialTable *materials,
			  MapSourceLegend *out);

/* Material id for an exact 0xRRGGBB colour, or -1 (air) when absent / NULL. */
int mapSourceColorLookup(const MapSourceLegend *legend, uint32_t rgb);

/* Assemble `count` slices (index 0 = level y 0, the bottom) into a Voxmap.
 * All images must share one width/height. Returns NULL with a stderr
 * diagnostic on NULL/empty input, a mismatched size, an out-of-range
 * dimension, an over-large volume, or OOM. `label` names the source in
 * diagnostics (NULL is allowed). `outStats` may be NULL. */
Voxmap *mapSourceAssembleSlices(const MapSourceImage *images, int count,
				const MapSourceLegend *legend, const char *label,
				MapSourceStats *outStats);

#endif /* ISOMATA_RENDER_MAPSOURCE_H */
