#!/usr/bin/env python3
"""Print every branch record gcov read zero for one source file.

The open-arm lists in meson/coverage-min.txt are produced by this script, so a
reader can re-derive them instead of trusting the prose:

    python3 meson/open-arms.py                       # src/sim/brain.c
    python3 meson/open-arms.py src/agents/errand.c

It reads the lcov artifact (straight out of gcov) rather than a line-number
approximation, and it splits gcov's LOOP-BOUND records out from genuine
never-taken conditionals: gcov emits an extra branch record per loop condition,
so a `for` whose body always leaves through `break` reads "exit side never
taken" even though no decision was skipped. Those are instrumentation artefacts,
not arms anybody should act on, and they are printed separately so nobody lists
them as if they were logic.

Arguments: [source.c] [coverage.info]
"""
import re
import sys
from collections import defaultdict

SRC = sys.argv[1] if len(sys.argv) > 1 else "src/sim/brain.c"
ART = sys.argv[2] if len(sys.argv) > 2 else "build-cov/coverage.info"

by_line = defaultdict(dict)
cur = None
for line in open(ART):
    line = line.rstrip("\n")
    if line.startswith("SF:"):
        cur = line[3:]
    elif line.startswith("BRDA:") and cur and cur.endswith(SRC):
        ln, blk, br, taken = line[5:].split(",")
        by_line[int(ln)][br] = taken

src = open(SRC).read().split("\n")
code = {}
for i, text in enumerate(src, 1):
    stripped = text.lstrip()
    code[i] = (text.split("*/")[-1] if stripped.startswith(("*", "/*"))
               else text).strip() or text.strip()


def is_loop(ln):
    return re.match(r"\s*(for|while)\s*\(", src[ln - 1]) is not None


rows = [(ln, sum(1 for b, t in blocks.items()
                 if t == "0" and not b.endswith("-")),
        len(blocks), is_loop(ln), code[ln])
        for ln, blocks in sorted(by_line.items())
        if any(t == "0" and not b.endswith("-") for b, t in blocks.items())]

loops = [r for r in rows if r[3]]
logic = [r for r in rows if not r[3]]

print(f"{SRC}: {sum(len(b) for b in by_line.values())} branch records; "
      f"{len(rows)} lines carry a never-taken branch\n")
print(f"LOOP-BOUND RECORDS ({len(loops)} lines) — gcov artefacts, not decisions:")
for ln, miss, tot, _, text in loops:
    print(f"  {ln:5d}  {miss}/{tot} never taken   {text[:70]}")
print(f"\nGENUINE NEVER-TAKEN ARMS ({len(logic)} lines, "
      f"{sum(r[1] for r in logic)} branch records):")
for ln, miss, tot, _, text in logic:
    print(f"  {ln:5d}  {miss}/{tot} never taken   {text[:70]}")
