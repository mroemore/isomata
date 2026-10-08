#ifndef ISOMATA_RENDER_TEXTURES_H
#define ISOMATA_RENDER_TEXTURES_H

/*
 * Atlas UV regions for assets/textures/placeholder.png, reused as a 2x2
 * atlas by Task 8. The PNG is 64x64: four 32x32 quadrants (red top-left,
 * green top-right, blue bottom-left, yellow bottom-right) plus a black dot
 * inside the red quadrant.
 *
 * UV space matches the SDL_gpu upload: v = 0 is the texture's TOP row
 * (row 0 of the source surface), so the red top-left quadrant occupies
 * u, v in [0, 0.5] and the green top-right quadrant occupies u in [0.5, 1],
 * v in [0, 0.5].
 *
 * Region assignment (pinned by test_drawlist / test_sprites):
 *   voxel top face  -> yellow (u 0.5..1.0, v 0.5..1.0)
 *   voxel side face -> green  (u 0.5..1.0, v 0.0..0.5)
 *   sprite          -> blue   (u 0.0..0.5, v 0.5..1.0)
 *   spare           -> red    (u 0.0..0.5, v 0.0..0.5; carries the black dot)
 *
 * Each region is a 4-corner UV quad in the ONE canonical corner order every
 * draw item uses: index 0..3 = (u0,v0), (u1,v0), (u1,v1), (u0,v1), i.e.
 * top-left, top-right, bottom-right, bottom-left of the atlas region.
 *
 * Macros, not objects: a header include adds no storage and no unused
 * file-scope const in any translation unit.
 */

#define ATLAS_UV_TOP \
	{ { 0.5f, 0.5f }, { 1.0f, 0.5f }, { 1.0f, 1.0f }, { 0.5f, 1.0f } }
#define ATLAS_UV_SIDE \
	{ { 0.5f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 0.5f }, { 0.5f, 0.5f } }
#define ATLAS_UV_SPRITE \
	{ { 0.0f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 1.0f }, { 0.0f, 1.0f } }
#define ATLAS_UV_SPARE \
	{ { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 0.5f, 0.5f }, { 0.0f, 0.5f } }

#endif /* ISOMATA_RENDER_TEXTURES_H */
