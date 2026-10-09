#ifndef ISOMATA_RENDER_TEXTURES_H
#define ISOMATA_RENDER_TEXTURES_H

/*
 * Built-in fallback atlas regions, used ONLY when no MaterialTable is
 * supplied to the emitters (the pure-test path; at runtime every face samples
 * the material table built from assets/textures/materials.txt).
 *
 * The regions describe assets/textures/placeholder.png, a 64x64 PNG used as a
 * 2x2 atlas of 32x32 quadrants (red top-left, green top-right, blue
 * bottom-left, yellow bottom-right) plus a black dot inside the red quadrant.
 *
 * UV space matches the SDL_gpu upload: v = 0 is the texture's TOP row (row 0
 * of the source surface), so the red top-left quadrant occupies u, v in
 * [0, 0.5] and the green top-right quadrant occupies u in [0.5, 1],
 * v in [0, 0.5].
 *
 * Region assignment:
 *   voxel top face  -> yellow (u 0.5..1.0, v 0.5..1.0)
 *   voxel side face -> green  (u 0.5..1.0, v 0.0..0.5)
 *   sprite          -> blue   (u 0.0..0.5, v 0.5..1.0)
 *   spare           -> red    (u 0.0..0.5, v 0.0..0.5; carries the black dot)
 *
 * Each region is a 4-corner UV quad in the ONE canonical corner order every
 * draw item uses: index 0..3 = (u0,v0), (u1,v0), (u1,v1), (u0,v1), i.e.
 * top-left, top-right, bottom-right, bottom-left of the atlas region.
 *
 * ORIENTATION: side faces and sprites use the quad corner order bottom-left,
 * bottom-right, top-right, top-left, so their UV quads list the region's
 * BOTTOM corners first (index 0 = region bottom-left) to keep the texture
 * upright. This is the ledgered UV-convention flip, fixed and pinned by
 * tests/test_materials.c::test_face_uv_orientation.
 *
 * Macros, not objects: a header include adds no storage and no unused
 * file-scope const in any translation unit.
 */

/* Top face: far-left, far-right, near-right, near-left (region TL,TR,BR,BL). */
#define ATLAS_UV_TOP \
	{ { 0.5f, 0.5f }, { 1.0f, 0.5f }, { 1.0f, 1.0f }, { 0.5f, 1.0f } }
/* Side face: world bottom-left..top-left; region BL,BR,TR,TL (upright). */
#define ATLAS_UV_SIDE \
	{ { 0.5f, 0.5f }, { 1.0f, 0.5f }, { 1.0f, 0.0f }, { 0.5f, 0.0f } }
/* Sprite billboard: same corner order as a side face (upright). */
#define ATLAS_UV_SPRITE \
	{ { 0.0f, 1.0f }, { 0.5f, 1.0f }, { 0.5f, 0.5f }, { 0.0f, 0.5f } }
#define ATLAS_UV_SPARE \
	{ { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 0.5f, 0.5f }, { 0.0f, 0.5f } }

#endif /* ISOMATA_RENDER_TEXTURES_H */
