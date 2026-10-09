/*
 * Material model + manifest tests (CTOL rung 1: unit + boundary).
 *
 * Pins the manifest grammar (one file fills all six faces, per-face overrides,
 * the `side` shorthand, alpha modes, comments/blank lines, and the malformed
 * cases), the table add/set/lookup, the side-direction -> FaceId mapping, and
 * the per-face UV orientation. The orientation pin is the ledgered
 * UV-convention flip: a side face must sample its texture UPRIGHT (texture top
 * at the quad's top corners), not flipped, and east/west must not be mirrored
 * against north/south.
 *
 * Pure: links materials.c and the Unity subset. Harness convention: no
 * main()/setUp()/tearDown(); exposes run_test_materials().
 */

#include "unity.h"

#include "render/materials.h"

#include <stdio.h>
#include <string.h>

#define EPS 1e-6f

static bool parseManifest(MaterialManifest *m, const char *text)
{
	return materialManifestParse(text, strlen(text), m);
}

static void assertUV(const float uv[4][2], const float expect[4][2])
{
	int i;

	for (i = 0; i < 4; i++) {
		TEST_ASSERT_FLOAT_WITHIN(EPS, expect[i][0], uv[i][0]);
		TEST_ASSERT_FLOAT_WITHIN(EPS, expect[i][1], uv[i][1]);
	}
}

/* One file fills all six faces. */
static void test_manifest_one_file_fills_six(void)
{
	MaterialManifest m;
	int i;

	TEST_ASSERT_TRUE(parseManifest(&m, "material grass grass.png\n"));
	TEST_ASSERT_EQUAL_INT(1, (int)m.count);
	TEST_ASSERT_EQUAL_STRING("grass", m.defs[0].name);
	TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, m.defs[0].alpha);
	for (i = 0; i < 6; i++)
		TEST_ASSERT_EQUAL_STRING("grass.png", m.defs[0].file[i]);
}

/* Comments, blank lines, CRLF and trailing whitespace are tolerated. */
static void test_manifest_comments_and_blank(void)
{
	MaterialManifest m;

	TEST_ASSERT_TRUE(parseManifest(&m,
		"# a comment\r\n"
		"\r\n"
		"material wood wood.png\r\n"
		"   \r\n"
		"material stone stone.png\r\n"));
	TEST_ASSERT_EQUAL_INT(2, (int)m.count);
	TEST_ASSERT_EQUAL_STRING("wood", m.defs[0].name);
	TEST_ASSERT_EQUAL_STRING("stone", m.defs[1].name);
}

/* Per-face overrides win; `side` fills all four sides; unset faces inherit
 * the primary (default) file. */
static void test_manifest_face_overrides(void)
{
	MaterialManifest m;

	TEST_ASSERT_TRUE(parseManifest(&m,
		"material special base.png face top=top.png side=side.png\n"));
	TEST_ASSERT_EQUAL_INT(1, (int)m.count);
	TEST_ASSERT_EQUAL_STRING("top.png", m.defs[0].file[FACE_TOP]);
	TEST_ASSERT_EQUAL_STRING("side.png", m.defs[0].file[FACE_NORTH]);
	TEST_ASSERT_EQUAL_STRING("side.png", m.defs[0].file[FACE_SOUTH]);
	TEST_ASSERT_EQUAL_STRING("side.png", m.defs[0].file[FACE_EAST]);
	TEST_ASSERT_EQUAL_STRING("side.png", m.defs[0].file[FACE_WEST]);
	/* Bottom was not overridden: it inherits the default file. */
	TEST_ASSERT_EQUAL_STRING("base.png", m.defs[0].file[FACE_BOTTOM]);

	/* Six distinct faces, no default: every face gets its override. */
	TEST_ASSERT_TRUE(parseManifest(&m,
		"material six face top=a.png bottom=b.png north=c.png "
		"south=d.png east=e.png west=f.png\n"));
	TEST_ASSERT_EQUAL_STRING("a.png", m.defs[0].file[FACE_TOP]);
	TEST_ASSERT_EQUAL_STRING("b.png", m.defs[0].file[FACE_BOTTOM]);
	TEST_ASSERT_EQUAL_STRING("c.png", m.defs[0].file[FACE_NORTH]);
	TEST_ASSERT_EQUAL_STRING("d.png", m.defs[0].file[FACE_SOUTH]);
	TEST_ASSERT_EQUAL_STRING("e.png", m.defs[0].file[FACE_EAST]);
	TEST_ASSERT_EQUAL_STRING("f.png", m.defs[0].file[FACE_WEST]);

	/* One override only: the other five faces inherit the primary. */
	TEST_ASSERT_TRUE(parseManifest(&m, "material a face top=only.png\n"));
	TEST_ASSERT_EQUAL_STRING("only.png", m.defs[0].file[FACE_TOP]);
	TEST_ASSERT_EQUAL_STRING("only.png", m.defs[0].file[FACE_WEST]);
}

/* Alpha modes parse; an unknown mode is an error. */
static void test_manifest_alpha_modes(void)
{
	MaterialManifest m;

	TEST_ASSERT_TRUE(parseManifest(&m,
		"material a a.png alpha=opaque\n"
		"material b b.png alpha=blend\n"
		"material c c.png alpha=cutout\n"));
	TEST_ASSERT_EQUAL_INT(ALPHA_OPAQUE, m.defs[0].alpha);
	TEST_ASSERT_EQUAL_INT(ALPHA_BLEND, m.defs[1].alpha);
	TEST_ASSERT_EQUAL_INT(ALPHA_CUTOUT, m.defs[2].alpha);

	TEST_ASSERT_FALSE(parseManifest(&m, "material x x.png alpha=nope\n"));
}

/* Malformed manifests are refused. */
static void test_manifest_rejects_bad_input(void)
{
	MaterialManifest m;

	TEST_ASSERT_FALSE(materialManifestParse(NULL, 5, &m));
	TEST_ASSERT_FALSE(materialManifestParse("material a a.png\n", 17, NULL));
	TEST_ASSERT_FALSE(parseManifest(&m, "notmaterial a a.png\n"));
	TEST_ASSERT_FALSE(parseManifest(&m, "material a\n"));
	TEST_ASSERT_FALSE(parseManifest(&m, "material a face =z.png\n"));
	TEST_ASSERT_FALSE(parseManifest(&m, "material a face bogus=z.png\n"));
	/* A material needs at least one file. */
	TEST_ASSERT_FALSE(parseManifest(&m, "material a alpha=blend\n"));

	/* An empty manifest is valid and yields zero materials. */
	TEST_ASSERT_TRUE(materialManifestParse("", 0, &m));
	TEST_ASSERT_EQUAL_INT(0, (int)m.count);
}

/* Table add / set / lookup. */
static void test_table_add_lookup(void)
{
	MaterialTable t;
	AtlasRect r = { 0.1f, 0.2f, 0.3f, 0.4f };
	int id;

	memset(&t, 0, sizeof(t));
	id = materialTableAdd(&t, "grass", ALPHA_BLEND);
	TEST_ASSERT_EQUAL_INT(0, id);
	TEST_ASSERT_EQUAL_INT(1, materialTableAdd(&t, "stone", ALPHA_OPAQUE));
	TEST_ASSERT_EQUAL_INT(0, materialIdByName(&t, "grass"));
	TEST_ASSERT_EQUAL_INT(1, materialIdByName(&t, "stone"));
	TEST_ASSERT_EQUAL_INT(-1, materialIdByName(&t, "nope"));
	TEST_ASSERT_EQUAL_INT(-1, materialIdByName(&t, NULL));
	TEST_ASSERT_EQUAL_INT(-1, materialIdByName(NULL, "grass"));
	TEST_ASSERT_EQUAL_INT(ALPHA_BLEND, t.items[0].alpha);

	TEST_ASSERT_TRUE(materialTableSetRect(&t, 0, FACE_TOP, r));
	TEST_ASSERT_FLOAT_WITHIN(EPS, 0.1f, t.items[0].rect[FACE_TOP].u0);
	TEST_ASSERT_FALSE(materialTableSetRect(&t, 9, FACE_TOP, r));
	TEST_ASSERT_FALSE(materialTableSetRect(NULL, 0, FACE_TOP, r));

	/* Name truncation is bounded, not an overflow. */
	id = materialTableAdd(&t, "a-very-long-material-name-exceeding-the-cap",
			      ALPHA_OPAQUE);
	TEST_ASSERT_TRUE(id >= 0);
	TEST_ASSERT_TRUE(strlen(t.items[id].name) < MATERIAL_NAME_MAX);
}

/* Side direction -> FaceId. */
static void test_face_for_side_dir(void)
{
	TEST_ASSERT_EQUAL_INT(FACE_SOUTH, materialFaceForSideDir(0));	/* +Z */
	TEST_ASSERT_EQUAL_INT(FACE_EAST, materialFaceForSideDir(1));	/* +X */
	TEST_ASSERT_EQUAL_INT(FACE_NORTH, materialFaceForSideDir(2));	/* -Z */
	TEST_ASSERT_EQUAL_INT(FACE_WEST, materialFaceForSideDir(3));	/* -X */
	TEST_ASSERT_EQUAL_INT(FACE_SOUTH, materialFaceForSideDir(99));
}

/* THE ORIENTATION PIN. A distinct rect so any flip or mirror is visible.
 * Top reads (u0,v0) at corner 0; a side face must read its texture top
 * (v0) at the quad's TOP corners (2 and 3), not its bottom. */
static void test_face_uv_orientation(void)
{
	const AtlasRect r = { 0.1f, 0.2f, 0.3f, 0.4f };
	float uv[4][2];

	/* Top: far-left, far-right, near-right, near-left; texture top at the
	 * far edge, texture left at the west edge. */
	{
		const float expect[4][2] = {
			{ 0.1f, 0.2f }, { 0.3f, 0.2f },
			{ 0.3f, 0.4f }, { 0.1f, 0.4f },
		};

		materialFaceUV(&r, FACE_TOP, uv);
		assertUV(uv, expect);
	}

	/* South (+Z): quad BL, BR, TR, TL. Upright: bottom corners take the
	 * texture BOTTOM (v1), top corners the texture top (v0). */
	{
		const float expect[4][2] = {
			{ 0.1f, 0.4f }, { 0.3f, 0.4f },
			{ 0.3f, 0.2f }, { 0.1f, 0.2f },
		};

		materialFaceUV(&r, FACE_SOUTH, uv);
		assertUV(uv, expect);
	}

	/* North (-Z) uses the same upright order as south (the emitter's quad
	 * corner order already reverses x for -Z). */
	{
		const float expect[4][2] = {
			{ 0.1f, 0.4f }, { 0.3f, 0.4f },
			{ 0.3f, 0.2f }, { 0.1f, 0.2f },
		};

		materialFaceUV(&r, FACE_NORTH, uv);
		assertUV(uv, expect);
	}

	/* East (+X) and West (-X): upright AND mirrored horizontally so the
	 * texture is not reversed when viewed from outside. */
	{
		const float expect[4][2] = {
			{ 0.3f, 0.4f }, { 0.1f, 0.4f },
			{ 0.1f, 0.2f }, { 0.3f, 0.2f },
		};

		materialFaceUV(&r, FACE_EAST, uv);
		assertUV(uv, expect);
		materialFaceUV(&r, FACE_WEST, uv);
		assertUV(uv, expect);
	}

	/* Bottom mirrors the top vertically (unused by emission for now, but
	 * pinned so a future inside set inherits a correct orientation). */
	{
		const float expect[4][2] = {
			{ 0.1f, 0.4f }, { 0.3f, 0.4f },
			{ 0.3f, 0.2f }, { 0.1f, 0.2f },
		};

		materialFaceUV(&r, FACE_BOTTOM, uv);
		assertUV(uv, expect);
	}

	materialFaceUV(NULL, FACE_TOP, uv);	/* no crash */
	materialFaceUV(&r, FACE_TOP, NULL);
}

/* Bounds and long-name guards: a full table, NULL args, out-of-range face
 * ids, an over-long name/path, and a manifest past MATERIAL_MAX all refuse
 * cleanly. */
static void test_table_and_manifest_bounds(void)
{
	MaterialTable t;
	MaterialManifest m;
	char name[64];
	int i;

	memset(&t, 0, sizeof(t));
	TEST_ASSERT_EQUAL_INT(-1, materialTableAdd(NULL, "x", ALPHA_OPAQUE));
	TEST_ASSERT_EQUAL_INT(-1, materialTableAdd(&t, NULL, ALPHA_OPAQUE));
	for (i = 0; i < MATERIAL_MAX; i++)
		TEST_ASSERT_TRUE(materialTableAdd(&t, "m", ALPHA_OPAQUE) >= 0);
	TEST_ASSERT_EQUAL_INT(-1, materialTableAdd(&t, "overflow", ALPHA_OPAQUE));

	TEST_ASSERT_FALSE(materialTableSetRect(&t, 0, (FaceId)-1,
					       (AtlasRect){ 0, 0, 1, 1 }));
	TEST_ASSERT_FALSE(materialTableSetRect(&t, 0, (FaceId)(FACE_WEST + 1),
					       (AtlasRect){ 0, 0, 1, 1 }));

	/* A name longer than MATERIAL_NAME_MAX is refused. */
	memset(name, 'a', sizeof(name));
	name[sizeof(name) - 1] = '\0';
	TEST_ASSERT_FALSE(parseManifest(&m, "material aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa a.png\n"));

	/* 65 materials exceed the manifest capacity. */
	{
		char big[4096];
		size_t off = 0;

		for (i = 0; i < MATERIAL_MAX + 1; i++)
			off += (size_t)snprintf(big + off, sizeof(big) - off,
						"material m%d f.png\n", i);
		TEST_ASSERT_FALSE(materialManifestParse(big, off, &m));
	}

	/* A default file given twice on one line is refused. */
	TEST_ASSERT_FALSE(parseManifest(&m, "material a a.png b.png\n"));
}

void run_test_materials(void);

void run_test_materials(void)
{
	RUN_TEST(test_manifest_one_file_fills_six);
	RUN_TEST(test_manifest_comments_and_blank);
	RUN_TEST(test_manifest_face_overrides);
	RUN_TEST(test_manifest_alpha_modes);
	RUN_TEST(test_manifest_rejects_bad_input);
	RUN_TEST(test_table_add_lookup);
	RUN_TEST(test_face_for_side_dir);
	RUN_TEST(test_face_uv_orientation);
	RUN_TEST(test_table_and_manifest_bounds);
}
