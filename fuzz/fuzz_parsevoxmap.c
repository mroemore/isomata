/*
 * libFuzzer harness for parseVoxmapText (src/render/voxmap.c).
 *
 * The target is the engine's one untrusted-input parser: it consumes bytes
 * that on Android come straight out of the APK (SDL_LoadFile) and is the only
 * place a malformed map can drive an out-of-bounds read. The parser is
 * length-bounded and explicitly does not assume NUL termination, so the
 * fuzzer feeds it exactly the fuzzed slice (never a NUL-terminated copy).
 *
 * On a successful parse the accessors are walked so a valid map's cell grid is
 * read too, not only the validation paths.
 */

#include "render/voxmap.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	Voxmap *map = parseVoxmapText((const char *)data, size);

	if (map != NULL) {
		int w = voxmapWidth(map);
		int d = voxmapDepth(map);
		int x;
		int y;

		for (y = 0; y < d; y++)
			for (x = 0; x < w; x++)
				(void)voxmapHeightAt(map, x, y);
		(void)voxmapIsVoid(map, 0, 0);
		destroyVoxmap(map);
	}
	return 0;
}
