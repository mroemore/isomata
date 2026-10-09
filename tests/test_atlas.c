/*
 * Atlas packing tests (CTOL rung 1: unit + boundary).
 *
 * Pins the pure packer: supported cell-size selection, the near-square grid
 * layout, slot pixel origins, the half-texel-inset UV rects (so a nearest
 * sampler never bleeds a neighbour in), the bounds-clipped 4-byte blit, and
 * the generated magenta/black fallback cell.
 *
 * Pure: links atlas.c and the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_atlas().
 */

#include "unity.h"

#include "render/atlas.h"

#include <string.h>

#define EPS 1e-6f

/* Cell size snaps to the smallest supported size that fits the source. */
static void test_cell_size_selection(void)
{
	TEST_ASSERT_EQUAL_INT(16, atlasCellSizeFor(1));
	TEST_ASSERT_EQUAL_INT(16, atlasCellSizeFor(16));
	TEST_ASSERT_EQUAL_INT(32, atlasCellSizeFor(17));
	TEST_ASSERT_EQUAL_INT(32, atlasCellSizeFor(32));
	TEST_ASSERT_EQUAL_INT(64, atlasCellSizeFor(33));
	TEST_ASSERT_EQUAL_INT(64, atlasCellSizeFor(64));
	TEST_ASSERT_EQUAL_INT(64, atlasCellSizeFor(1024));	/* clamp */
	TEST_ASSERT_EQUAL_INT(16, atlasCellSizeFor(0));	/* clamp */
	TEST_ASSERT_EQUAL_INT(16, atlasCellSizeFor(-5));
}

/* Layout is the smallest near-square grid holding the slots. */
static void test_layout_grid(void)
{
	AtlasLayout l;

	TEST_ASSERT_TRUE(atlasComputeLayout(1, 16, &l));
	TEST_ASSERT_EQUAL_INT(1, l.columns);
	TEST_ASSERT_EQUAL_INT(1, l.rows);
	TEST_ASSERT_EQUAL_INT(16, l.width);
	TEST_ASSERT_EQUAL_INT(16, l.height);

	TEST_ASSERT_TRUE(atlasComputeLayout(4, 32, &l));
	TEST_ASSERT_EQUAL_INT(2, l.columns);
	TEST_ASSERT_EQUAL_INT(2, l.rows);
	TEST_ASSERT_EQUAL_INT(64, l.width);
	TEST_ASSERT_EQUAL_INT(64, l.height);

	TEST_ASSERT_TRUE(atlasComputeLayout(5, 16, &l));
	TEST_ASSERT_EQUAL_INT(3, l.columns);	/* ceil(sqrt(5)) = 3 */
	TEST_ASSERT_EQUAL_INT(2, l.rows);
	TEST_ASSERT_EQUAL_INT(48, l.width);
	TEST_ASSERT_EQUAL_INT(32, l.height);

	TEST_ASSERT_TRUE(atlasComputeLayout(12, 64, &l));
	TEST_ASSERT_EQUAL_INT(4, l.columns);
	TEST_ASSERT_EQUAL_INT(3, l.rows);
	TEST_ASSERT_EQUAL_INT(256, l.width);
	TEST_ASSERT_EQUAL_INT(192, l.height);
}

/* Bad layout arguments are refused and leave the output untouched. */
static void test_layout_rejects_bad_args(void)
{
	AtlasLayout l = { 7, 7, 7, 7, 7, 7 };

	TEST_ASSERT_FALSE(atlasComputeLayout(0, 16, &l));
	TEST_ASSERT_FALSE(atlasComputeLayout(ATLAS_MAX_SLOTS + 1, 16, &l));
	TEST_ASSERT_FALSE(atlasComputeLayout(1, 8, &l));	/* below 16 */
	TEST_ASSERT_FALSE(atlasComputeLayout(1, 128, &l));	/* above 64 */
	TEST_ASSERT_FALSE(atlasComputeLayout(1, 16, NULL));
	TEST_ASSERT_EQUAL_INT(7, l.columns);	/* untouched */
}

/* Row-major pixel origins; out-of-range slots are refused. */
static void test_slot_origins(void)
{
	AtlasLayout l;
	int x;
	int y;

	TEST_ASSERT_TRUE(atlasComputeLayout(12, 64, &l));	/* 4x3 */
	TEST_ASSERT_TRUE(atlasSlotOrigin(&l, 0, &x, &y));
	TEST_ASSERT_EQUAL_INT(0, x);
	TEST_ASSERT_EQUAL_INT(0, y);
	TEST_ASSERT_TRUE(atlasSlotOrigin(&l, 5, &x, &y));
	TEST_ASSERT_EQUAL_INT(64, x);
	TEST_ASSERT_EQUAL_INT(64, y);
	TEST_ASSERT_TRUE(atlasSlotOrigin(&l, 11, &x, &y));
	TEST_ASSERT_EQUAL_INT(192, x);
	TEST_ASSERT_EQUAL_INT(128, y);
	TEST_ASSERT_FALSE(atlasSlotOrigin(&l, 12, &x, &y));
	TEST_ASSERT_FALSE(atlasSlotOrigin(&l, -1, &x, &y));
	TEST_ASSERT_FALSE(atlasSlotOrigin(NULL, 0, &x, &y));
	/* NULL out pointers are allowed (both may be skipped). */
	TEST_ASSERT_TRUE(atlasSlotOrigin(&l, 3, NULL, NULL));
}

/* The UV rect is inset by half a texel at each edge: a 16-texel source in a
 * 64-texel cell at slot 0 of a 4x3/64 atlas. */
static void test_slot_rect_half_texel_inset(void)
{
	AtlasLayout l;
	AtlasRect r;

	TEST_ASSERT_TRUE(atlasComputeLayout(12, 64, &l));	/* 256x192 */
	TEST_ASSERT_TRUE(atlasSlotRect(&l, 0, 16, &r));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f / 256.0f, r.u0);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f / 192.0f, r.v0);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 15.5f / 256.0f, r.u1);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 15.5f / 192.0f, r.v1);

	/* Slot 5 (col 1, row 1): origin (64, 64). */
	TEST_ASSERT_TRUE(atlasSlotRect(&l, 5, 64, &r));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 64.5f / 256.0f, r.u0);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 64.5f / 192.0f, r.v0);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 127.5f / 256.0f, r.u1);
	TEST_ASSERT_FLOAT_WITHIN(EPS, 127.5f / 192.0f, r.v1);

	/* A srcSize larger than the cell clamps to the cell. */
	TEST_ASSERT_TRUE(atlasSlotRect(&l, 0, 999, &r));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 63.5f / 256.0f, r.u1);

	TEST_ASSERT_FALSE(atlasSlotRect(&l, 12, 16, &r));
	TEST_ASSERT_FALSE(atlasSlotRect(NULL, 0, 16, &r));
	TEST_ASSERT_FALSE(atlasSlotRect(&l, 0, 16, NULL));
}

/* The blit copies a 4-bpp source at an offset and clips at the edges. */
static void test_blit_copies_and_clips(void)
{
	uint8_t dst[4 * 4 * 4];	/* 4x4, zeroed */
	uint8_t src[2 * 2 * 4];
	int i;

	memset(dst, 0, sizeof(dst));
	for (i = 0; i < 4; i++) {
		src[i * 4 + 0] = (uint8_t)(10 + i);
		src[i * 4 + 1] = 20;
		src[i * 4 + 2] = 30;
		src[i * 4 + 3] = 40;
	}
	TEST_ASSERT_TRUE(atlasBlitPixels(dst, 4, 4, 1, 1, src, 2, 2, 8));
	/* (1,1) gets src texel 0. */
	TEST_ASSERT_EQUAL_INT(10, dst[(1 * 4 + 1) * 4 + 0]);
	TEST_ASSERT_EQUAL_INT(40, dst[(1 * 4 + 1) * 4 + 3]);
	/* (2,2) gets src texel 3. */
	TEST_ASSERT_EQUAL_INT(13, dst[(2 * 4 + 2) * 4 + 0]);
	/* (0,0) untouched. */
	TEST_ASSERT_EQUAL_INT(0, dst[0]);

	/* Partly off the right edge: only the in-bounds column lands. */
	memset(dst, 0, sizeof(dst));
	TEST_ASSERT_TRUE(atlasBlitPixels(dst, 4, 4, 3, 0, src, 2, 2, 8));
	TEST_ASSERT_EQUAL_INT(10, dst[(0 * 4 + 3) * 4 + 0]);
	TEST_ASSERT_EQUAL_INT(0, dst[(0 * 4 + 0) * 4 + 0]);

	/* Entirely off: a no-op success. */
	memset(dst, 0, sizeof(dst));
	TEST_ASSERT_TRUE(atlasBlitPixels(dst, 4, 4, 100, 100, src, 2, 2, 8));
	for (i = 0; i < 16; i++)
		TEST_ASSERT_EQUAL_INT(0, dst[i * 4]);

	/* Bad args. */
	TEST_ASSERT_FALSE(atlasBlitPixels(NULL, 4, 4, 0, 0, src, 2, 2, 8));
	TEST_ASSERT_FALSE(atlasBlitPixels(dst, 4, 4, 0, 0, NULL, 2, 2, 8));
	TEST_ASSERT_FALSE(atlasBlitPixels(dst, 4, 4, 0, 0, src, 2, 2, 4));
	TEST_ASSERT_FALSE(atlasBlitPixels(dst, 0, 4, 0, 0, src, 2, 2, 8));
}

/* The fallback cell is a magenta/black 8-texel checker, RGBA opaque, clipped
 * at the destination bounds. */
static void test_fallback_cell_checker(void)
{
	uint8_t dst[16 * 16 * 4];
	int i;

	memset(dst, 0, sizeof(dst));
	TEST_ASSERT_TRUE(atlasFillFallbackCell(dst, 16, 16, 0, 0, 16));
	/* (0,0): block 0 -> magenta. */
	TEST_ASSERT_EQUAL_INT(255, dst[0]);
	TEST_ASSERT_EQUAL_INT(0, dst[1]);
	TEST_ASSERT_EQUAL_INT(255, dst[2]);
	TEST_ASSERT_EQUAL_INT(255, dst[3]);
	/* (8,0): next block -> black. */
	TEST_ASSERT_EQUAL_INT(0, dst[(0 * 16 + 8) * 4 + 0]);
	TEST_ASSERT_EQUAL_INT(255, dst[(0 * 16 + 8) * 4 + 3]);
	/* (0,8): next block -> black. */
	TEST_ASSERT_EQUAL_INT(0, dst[(8 * 16 + 0) * 4 + 0]);
	/* (8,8): even blocks -> magenta. */
	TEST_ASSERT_EQUAL_INT(255, dst[(8 * 16 + 8) * 4 + 0]);

	/* Clipped: filling at (12,12) size 16 only lands 4x4. */
	memset(dst, 0, sizeof(dst));
	TEST_ASSERT_TRUE(atlasFillFallbackCell(dst, 16, 16, 12, 12, 16));
	TEST_ASSERT_EQUAL_INT(255, dst[(12 * 16 + 12) * 4 + 0]);
	TEST_ASSERT_EQUAL_INT(255, dst[(15 * 16 + 15) * 4 + 0]);
	TEST_ASSERT_EQUAL_INT(0, dst[(11 * 16 + 11) * 4 + 0]);
	for (i = 0; i < 16 * 16; i++)
		TEST_ASSERT_TRUE(dst[i * 4 + 3] == 0 || dst[i * 4 + 3] == 255);

	TEST_ASSERT_FALSE(atlasFillFallbackCell(NULL, 16, 16, 0, 0, 16));
	TEST_ASSERT_FALSE(atlasFillFallbackCell(dst, 16, 16, 0, 0, 0));
}

/* Negative blit/fill origins clip at the top/left edge too, and a srcSize of
 * 0 clamps to a 1-texel rect. */
static void test_blit_and_fill_clip_edges(void)
{
	uint8_t dst[4 * 4 * 4];
	uint8_t src[2 * 2 * 4];
	AtlasLayout l;
	AtlasRect r;

	memset(dst, 0, sizeof(dst));
	memset(src, 7, sizeof(src));
	TEST_ASSERT_TRUE(atlasBlitPixels(dst, 4, 4, -1, -1, src, 2, 2, 8));
	/* (0,0) receives src texel (1,1) (the only in-bounds copy). */
	TEST_ASSERT_EQUAL_INT(7, dst[0]);

	TEST_ASSERT_TRUE(atlasComputeLayout(1, 16, &l));
	TEST_ASSERT_TRUE(atlasSlotRect(&l, 0, 0, &r));
	/* A 1-texel source clamps to a degenerate (single-texel-centre) rect. */
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f / 16.0f, r.u0);
	TEST_ASSERT_FLOAT_WITHIN(1e-6f, r.u0, r.u1);

	TEST_ASSERT_TRUE(atlasFillFallbackCell(dst, 4, 4, -1, -1, 4));
	TEST_ASSERT_FALSE(atlasFillFallbackCell(dst, 4, 0, 0, 0, 4));
	TEST_ASSERT_FALSE(atlasFillFallbackCell(dst, 4, 4, 0, 0, -2));
}

void run_test_atlas(void);

void run_test_atlas(void)
{
	RUN_TEST(test_cell_size_selection);
	RUN_TEST(test_layout_grid);
	RUN_TEST(test_layout_rejects_bad_args);
	RUN_TEST(test_slot_origins);
	RUN_TEST(test_slot_rect_half_texel_inset);
	RUN_TEST(test_blit_copies_and_clips);
	RUN_TEST(test_fallback_cell_checker);
	RUN_TEST(test_blit_and_fill_clip_edges);
}
