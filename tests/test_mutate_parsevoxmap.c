/*
 * Input-mutation target for parseVoxmapText (meson/mutate.sh).
 *
 * Self-contained: it flips bits of a known-good map buffer, runs the pure
 * length-bounded parser on each mutant, and checks the parser against an
 * INDEPENDENT oracle for the documented format (rows split on '\n', trailing
 * CR/space/tab stripped, blank and `@` legend lines skipped, equal non-blank
 * non-legend row widths, cells are '.', '0'..'9' or a char declared by a
 * legend line, '.' is void while '0' and a legend height 0 are SOLID height-0
 * cells, a legend line with a char >= 128 is skipped, bounded dims). A
 * mutant whose parse disagrees with the oracle is a survivor and fails the
 * target.
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

/* -2 = not a valid cell, -1 = void, 1..9 = column height. */
#define ORACLE_INVALID (-2)

typedef struct Oracle {
	int valid;
	int width;
	int depth;
	int8_t h[128];
	size_t rowStart[VOXMAP_MAX_DIM];
} Oracle;

static void oracleLegendDefaults(int8_t h[128])
{
	int c;

	for (c = 0; c < 128; c++)
		h[c] = ORACLE_INVALID;
	h['.'] = -1;
	for (c = '0'; c <= '9'; c++)
		h[c] = (int8_t)(c - '0');
}

/* Trim a line's trailing CR/space/tab; returns the trimmed length. */
static size_t oracleTrim(const unsigned char *text, size_t start, size_t end)
{
	while (end > start && (text[end - 1] == '\r' || text[end - 1] == ' ' ||
			       text[end - 1] == '\t'))
		end--;
	return end - start;
}

static int oracleIsLegend(const unsigned char *text, size_t start, size_t len)
{
	size_t i = start;
	size_t end = start + len;

	while (i < end && (text[i] == ' ' || text[i] == '\t'))
		i++;
	return i < end && text[i] == '@';
}

/* Split a bounded copy of a legend line on spaces/tabs. Returns token count. */
static int oracleTokens(const unsigned char *text, size_t start, size_t len,
			char *buf, size_t cap, char *tokens[8])
{
	size_t n = len < cap - 1 ? len : cap - 1;
	char *p = buf;
	int nt = 0;

	memcpy(buf, text + start, n);
	buf[n] = '\0';
	while (*p != '\0') {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (*p == '\0')
			break;
		if (nt < 8)
			tokens[nt] = p;
		nt++;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
	}
	return nt;
}

/* Independent format check + legend collection. Sets o->valid. */
static void oracleBuild(const unsigned char *text, size_t length, Oracle *o)
{
	size_t pos = 0;
	int width = -1;
	int depth = 0;

	memset(o, 0, sizeof(*o));
	oracleLegendDefaults(o->h);

	/* Pass 0: legends. */
	while (pos < length) {
		size_t start = pos;
		size_t end;
		size_t rowLen;

		while (pos < length && text[pos] != '\n')
			pos++;
		end = pos;
		if (pos < length)
			pos++;
		rowLen = oracleTrim(text, start, end);
		if (rowLen == 0)
			continue;
		if (oracleIsLegend(text, start, rowLen)) {
			char buf[256];
			char *tok[8];
			int nt = oracleTokens(text, start, rowLen, buf,
					      sizeof(buf), tok);
			unsigned char ch;
			int hIdx;

			if (nt > 8 || nt < 3) {
				o->valid = 0;
				return;
			}
			if (strcmp(tok[0], "@") == 0) {
				if (nt < 4 || strlen(tok[1]) != 1) {
					o->valid = 0;
					return;
				}
				ch = (unsigned char)tok[1][0];
				hIdx = 2;
			} else if (tok[0][0] == '@' && strlen(tok[0]) == 2) {
				if (nt < 3) {
					o->valid = 0;
					return;
				}
				ch = (unsigned char)tok[0][1];
				hIdx = 1;
			} else {
				o->valid = 0;
				return;
			}
			if (ch >= 128)		/* unusable legend cell: skipped */
				continue;
			if (strlen(tok[hIdx]) != 1 || tok[hIdx][0] < '0' ||
			    tok[hIdx][0] > '9') {
				o->valid = 0;
				return;
			}
			o->h[ch] = (int8_t)(tok[hIdx][0] - '0');
		}
	}

	/* Pass 1: validate map rows, pin width/depth, record row starts. */
	pos = 0;
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
		rowLen = oracleTrim(text, start, end);
		if (rowLen == 0 || oracleIsLegend(text, start, rowLen))
			continue;
		if (width < 0) {
			if (rowLen > VOXMAP_MAX_DIM) {
				o->valid = 0;
				return;
			}
			width = (int)rowLen;
		} else if ((int)rowLen != width) {
			o->valid = 0;
			return;
		}
		for (i = start; i < start + rowLen; i++) {
			unsigned char c = text[i];

			if (c >= 128 || o->h[c] == ORACLE_INVALID) {
				o->valid = 0;
				return;
			}
		}
		if (depth < VOXMAP_MAX_DIM)
			o->rowStart[depth] = start;
		depth++;
		if (depth > VOXMAP_MAX_DIM) {
			o->valid = 0;
			return;
		}
	}
	if (width <= 0 || depth <= 0) {
		o->valid = 0;
		return;
	}
	o->width = width;
	o->depth = depth;
	o->valid = 1;
}

/* 1 when the parser's result matches the oracle for this input, else 0. */
static int parseAgreesWithOracle(const unsigned char *buf, size_t len)
{
	Voxmap *map = parseVoxmapText((const char *)buf, len, NULL);
	Oracle o;
	int ok = 1;

	oracleBuild(buf, len, &o);
	if (map == NULL) {
		ok = !o.valid;
	} else {
		if (!o.valid) {
			ok = 0;
		} else if (voxmapWidth(map) != o.width ||
			   voxmapDepth(map) != o.depth) {
			ok = 0;
		} else {
			int y;

			for (y = 0; y < o.depth && ok; y++) {
				int x;

				for (x = 0; x < o.width; x++) {
					unsigned char c =
						buf[o.rowStart[y] + (size_t)x];

					if (voxmapHeightAt(map, x, y) !=
					    (int)o.h[c]) {
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
	static const char kBase[] = "@ g 2 grass\n1g3\n456\n";
	const size_t len = sizeof(kBase) - 1;
	unsigned int seed = mutate_seed();
	int killed = 0;
	int survived = 0;
	int i;

	mutate_reset(seed, 1);
	for (i = 0; i < MUTATE_ITERATIONS; i++) {
		unsigned char buf[64];

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
