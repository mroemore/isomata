/*
 * Input-mutation target for parseVoxmapText (meson/mutate.sh).
 *
 * Self-contained: it flips bits of a known-good map buffer, runs the pure
 * length-bounded parser on each mutant, and checks the parser against an
 * INDEPENDENT oracle for the documented format (rows split on '\n', trailing
 * CR/space/tab stripped, blank rows skipped, equal non-blank row widths,
 * '.'/0..9 cells, bounded dims). A mutant whose parse disagrees with the
 * oracle is a survivor and fails the target.
 *
 * Deterministic: the sweep is driven by tests/support/mutate.c with the seed
 * from MUTATE_SEED (default 1), so a survivor is reproducible from the seed.
 *
 * This is also a normal pure test (registered in tests/meson.build): the same
 * executable runs in the headless suite, so it guards the parser every build.
 *
 * Uses the CTOL micro-harness (tests/harness.h), not the Unity subset: the
 * mutation target is a standalone executable with its own main().
 */

#include "harness.h"

#include "render/voxmap.h"
#include "support/mutate.h"

#include <stdio.h>
#include <string.h>

#define MUTATE_ITERATIONS 512

typedef struct OracleDims {
	int width;
	int depth;
} OracleDims;

/* Independent format check. Returns 1 and fills dims when `text` is a valid
 * map by the documented contract, else 0. */
static int oracleScan(const unsigned char *text, size_t length, OracleDims *dims)
{
	int width = -1;
	int depth = 0;
	size_t pos = 0;

	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;
		size_t i;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;
		rowLen = end - start;
		while (rowLen > 0 &&
		       (text[start + rowLen - 1] == '\r' ||
			text[start + rowLen - 1] == ' ' ||
			text[start + rowLen - 1] == '\t'))
			rowLen--;
		if (rowLen == 0)
			continue;
		if (width < 0) {
			if (rowLen > VOXMAP_MAX_DIM)
				return 0;
			width = (int)rowLen;
		} else if ((int)rowLen != width) {
			return 0;
		}
		for (i = start; i < start + rowLen; i++) {
			unsigned char c = text[i];

			if (c != '.' && (c < '0' || c > '9'))
				return 0;
		}
		depth++;
		if (depth > VOXMAP_MAX_DIM)
			return 0;
	}
	if (width <= 0 || depth <= 0)
		return 0;
	dims->width = width;
	dims->depth = depth;
	return 1;
}

/* Independent decode of one cell of an already-validated map. */
static int oracleHeight(const unsigned char *text, size_t length, int row, int col)
{
	int r = 0;
	size_t pos = 0;

	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;
		rowLen = end - start;
		while (rowLen > 0 &&
		       (text[start + rowLen - 1] == '\r' ||
			text[start + rowLen - 1] == ' ' ||
			text[start + rowLen - 1] == '\t'))
			rowLen--;
		if (rowLen == 0)
			continue;
		if (r == row) {
			unsigned char c = text[start + (size_t)col];

			return (c == '.') ? -1 : (c - '0');
		}
		r++;
	}
	return -2;	/* unreachable for a validated map */
}

/* 1 when the parser's result matches the oracle for this input, else 0. */
static int parseAgreesWithOracle(const unsigned char *buf, size_t len)
{
	Voxmap *map = parseVoxmapText((const char *)buf, len);
	OracleDims dims;
	int valid = oracleScan(buf, len, &dims);
	int ok = 1;

	if (map == NULL) {
		ok = !valid;
	} else {
		if (!valid) {
			ok = 0;
		} else if (voxmapWidth(map) != dims.width ||
			   voxmapDepth(map) != dims.depth) {
			ok = 0;
		} else {
			int y;

			for (y = 0; y < dims.depth && ok; y++) {
				int x;

				for (x = 0; x < dims.width; x++) {
					if (voxmapHeightAt(map, x, y) !=
					    oracleHeight(buf, len, y, x)) {
						ok = 0;
						break;
					}
				}
			}
		}
		destroyVoxmap(map);
	}
	return ok;
}

static int test_parsevoxmap_mutation_sweep(void)
{
	static const char kBase[] = "012\n345\n";
	const size_t len = sizeof(kBase) - 1;
	unsigned int seed = mutate_seed();
	int killed = 0;
	int survived = 0;
	int i;

	mutate_reset(seed, 1);
	for (i = 0; i < MUTATE_ITERATIONS; i++) {
		unsigned char buf[32];

		memcpy(buf, kBase, len);
		if (mutate_bytes(buf, len) == 0)
			continue;
		if (parseAgreesWithOracle(buf, len)) {
			killed++;
		} else {
			survived++;
			fprintf(stderr,
				"mutate_parsevoxmap: survivor at seed=%u step=%d\n",
				seed, i + 1);
		}
	}
	printf("mutation target parsevoxmap: seed=%u killed=%d survived=%d\n",
	       seed, killed, survived);
	ASSERT_TRUE(killed > 0);
	ASSERT_EQ_INT(0, survived);
	return 0;
}

int main(void)
{
	int failed = 0;

	RUN(test_parsevoxmap_mutation_sweep);
	HARNESS_SUMMARY("mutate_parsevoxmap");
}
