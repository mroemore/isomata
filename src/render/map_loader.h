#ifndef ISOMATA_RENDER_MAP_LOADER_H
#define ISOMATA_RENDER_MAP_LOADER_H

/*
 * SDL-tier PNG slice map directory loader. This is glue: it owns the SDL I/O
 * (directory enumeration + SDL_LoadPNG + the asset path), while every pure
 * concern (slice order, colour legend, assembly) lives in mapsource.h. Never
 * compiled into isomata_pure and never linked by the headless suite.
 *
 * See docs/map-authoring.md for the directory format and mapsource.h for the
 * pure model. Android APK assets resolve through SDL_LoadPNG and
 * SDL_EnumerateDirectory (the app falls back to the ASCII demo if the
 * directory cannot be enumerated).
 */

#include "render/materials.h"
#include "render/voxmap.h"

/* Load a PNG slice map directory:
 *   1. enumerate `dir` for *.png and natural-sort the names (slice order),
 *   2. read dir/legend.txt and parse it (mapsource.h),
 *   3. SDL_LoadPNG every slice, convert to RGBA32, and assemble.
 * Returns the assembled Voxmap, or NULL with an SDL_LogError diagnostic when
 * the directory cannot be enumerated, holds no PNG, holds more than
 * VOXMAP_MAX_DIM slices, a PNG fails to decode, legend.txt is missing, or the
 * assembly fails. `materials` resolves legend names (NULL is allowed, like the
 * ASCII parser). `outSlices` (may be NULL) receives the slice count on
 * success. */
Voxmap *loadVoxmapDirectory(const char *dir, const MaterialTable *materials,
			    int *outSlices);

#endif /* ISOMATA_RENDER_MAP_LOADER_H */
