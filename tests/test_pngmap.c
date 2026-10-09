/*
 * PNG slice importer equivalence pin (SDL tier).
 *
 * The strongest T13b test: the committed PNG slice version of the 3D demo
 * scene (assets/maps/demo/00.png..08.png + legend.txt) must load, through the
 * real SDL glue (SDL_EnumerateDirectory + SDL_LoadPNG + the pure assembler),
 * to a Voxmap IDENTICAL to the ASCII source of truth assets/maps/demo.txt:
 * dimensions, per-voxel occupancy and material, and every parsed light.
 *
 * SDL tier (not the headless pure suite): it decodes real PNGs. Built natively
 * only (like the app smoke). Its own main().
 */

#include "unity.h"

#include "render/map_loader.h"
#include "render/materials.h"
#include "render/voxmap.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

/* Resolve an asset path relative to the test executable's build tree:
 * <build>/tests/test_pngmap -> <build>/../assets/<rel> = <build>/assets/<rel>. */
static void assetPath(const char *rel, char *out, size_t outSize)
{
	const char *base = SDL_GetBasePath();

	SDL_snprintf(out, outSize, "%s../assets/%s", base != NULL ? base : "",
		     rel);
}

/* Build the same name -> id material table the app builds from
 * assets/textures/materials.txt (names in manifest order). */
static void buildMaterials(MaterialTable *table)
{
	char path[1024];
	void *text;
	size_t size = 0;
	MaterialManifest manifest;
	size_t i;

	memset(table, 0, sizeof(*table));
	assetPath("textures/materials.txt", path, sizeof(path));
	text = SDL_LoadFile(path, &size);
	TEST_ASSERT_NOT_NULL(text);
	TEST_ASSERT_TRUE(materialManifestParse((const char *)text, size,
					       &manifest));
	for (i = 0; i < manifest.count; i++)
		materialTableAdd(table, manifest.defs[i].name,
				 manifest.defs[i].alpha);
	SDL_free(text);
}

/* True when every voxel's occupancy + material and every light match. */
static void assertVoxmapsIdentical(const Voxmap *a, const Voxmap *b)
{
	int x;
	int y;
	int z;
	int i;

	TEST_ASSERT_EQUAL_INT(voxmapWidth(a), voxmapWidth(b));
	TEST_ASSERT_EQUAL_INT(voxmapDepth(a), voxmapDepth(b));
	TEST_ASSERT_EQUAL_INT(voxmapLevels(a), voxmapLevels(b));
	for (z = 0; z < voxmapDepth(a); z++) {
		for (y = 0; y < voxmapLevels(a); y++) {
			for (x = 0; x < voxmapWidth(a); x++) {
				TEST_ASSERT_EQUAL_INT(
					voxmapSolidAt(a, x, y, z),
					voxmapSolidAt(b, x, y, z));
				TEST_ASSERT_EQUAL_INT(
					voxmapMaterialAtVoxel(a, x, y, z),
					voxmapMaterialAtVoxel(b, x, y, z));
			}
		}
	}
	TEST_ASSERT_EQUAL_INT(voxmapLightCount(a), voxmapLightCount(b));
	for (i = 0; i < voxmapLightCount(a); i++) {
		const VoxmapLight *la = voxmapLightAt(a, i);
		const VoxmapLight *lb = voxmapLightAt(b, i);

		TEST_ASSERT_NOT_NULL(la);
		TEST_ASSERT_NOT_NULL(lb);
		TEST_ASSERT_EQUAL_INT(la->kind, lb->kind);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->x, lb->x);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->y, lb->y);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->z, lb->z);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->r, lb->r);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->g, lb->g);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->b, lb->b);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->radius, lb->radius);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->dir[0], lb->dir[0]);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->dir[1], lb->dir[1]);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->dir[2], lb->dir[2]);
		TEST_ASSERT_DOUBLE_WITHIN(0.0, la->halfAngleDeg,
					  lb->halfAngleDeg);
	}
}

/* The PNG directory loads identical to demo.txt. */
static void test_png_slices_equal_ascii_demo(void)
{
	MaterialTable materials;
	char dir[1024];
	char txt[1024];
	void *text;
	size_t size = 0;
	Voxmap *fromPng;
	Voxmap *fromAscii;
	int slices = 0;

	buildMaterials(&materials);

	assetPath("maps/demo", dir, sizeof(dir));
	fromPng = loadVoxmapDirectory(dir, &materials, &slices);
	TEST_ASSERT_NOT_NULL(fromPng);
	TEST_ASSERT_EQUAL_INT(9, slices);
	TEST_ASSERT_EQUAL_INT(16, voxmapWidth(fromPng));
	TEST_ASSERT_EQUAL_INT(16, voxmapDepth(fromPng));
	TEST_ASSERT_EQUAL_INT(9, voxmapLevels(fromPng));

	assetPath("maps/demo.txt", txt, sizeof(txt));
	text = SDL_LoadFile(txt, &size);
	TEST_ASSERT_NOT_NULL(text);
	fromAscii = parseVoxmapText((const char *)text, size, &materials);
	SDL_free(text);
	TEST_ASSERT_NOT_NULL(fromAscii);

	assertVoxmapsIdentical(fromPng, fromAscii);

	destroyVoxmap(fromPng);
	destroyVoxmap(fromAscii);
}

/* A missing directory / a NULL dir fail cleanly (the app then falls back). */
static void test_loader_missing_directory(void)
{
	MaterialTable materials;
	char path[1024];

	buildMaterials(&materials);
	assetPath("maps/definitely_missing_dir", path, sizeof(path));
	TEST_ASSERT_NULL(loadVoxmapDirectory(path, &materials, NULL));
	TEST_ASSERT_NULL(loadVoxmapDirectory(NULL, &materials, NULL));
}

int main(void)
{
	int rc;

	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
	if (!SDL_Init(0)) {
		fprintf(stderr, "test_pngmap: SDL_Init failed: %s\n",
			SDL_GetError());
		return 1;
	}
	UNITY_BEGIN();
	RUN_TEST(test_png_slices_equal_ascii_demo);
	RUN_TEST(test_loader_missing_directory);
	rc = UNITY_END();
	SDL_Quit();
	return rc;
}
