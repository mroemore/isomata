/*
 * libFuzzer harness for mapSourceLegendParse (src/render/mapsource.c).
 *
 * legend.txt is untrusted text (an authoring file that on Android arrives from
 * the APK). The parser is length-bounded and never assumes NUL termination, so
 * the fuzzer feeds it exactly the fuzzed slice. On a successful parse the
 * colour lookup is walked so a valid legend's table is read too.
 */

#include "render/mapsource.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	MapSourceLegend legend;

	if (mapSourceLegendParse((const char *)data, size, NULL, &legend)) {
		int i;

		for (i = 0; i < legend.colorCount; i++)
			(void)mapSourceColorLookup(&legend,
						   legend.colors[i].rgb);
		(void)mapSourceColorLookup(&legend, 0x123456u);
	}
	return 0;
}
