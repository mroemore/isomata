#!/usr/bin/env python3
"""Print every branch record gcov read zero for one source file.

The open-arm lists in meson/coverage-min.txt are produced by this script, so a
reader can re-derive them instead of trusting the prose:

    python3 meson/open-arms.py                       # src/sim/brain.c
    python3 meson/open-arms.py src/agents/errand.c

Paths are resolved from the repository root, so it can be run from anywhere;
with no argument it uses build-cov/coverage.info under that root.

It reads the lcov artifact (straight out of gcov) rather than a line-number
approximation, and it sorts the records gcov did not count as hits into three
buckets:

  NEVER-TAKEN ARM      taken == "0" on a line that is not a loop condition —
                       a decision the suite does not make. These are the arms.
  LOOP-BOUND RECORD    taken == "0" on a `for`/`while` condition. gcov emits an
                       extra branch record per loop, so a loop that never
                       reaches its natural exit (its body breaks out, returns,
                       or runs the counter out first) reads as an exit side
                       nobody skipped. Whether a given loop is one of those is
                       a property of the loop, not something this script
                       checks, so it prints the lines and leaves the judgement
                       to the reader.
  LCOV-EXCLUDED        taken == "-", which lcov uses for records it will not
                       count. The gate's denominator (BRF) includes them and the
                       numerator never can, so they are printed rather than
                       dropped: that is what makes the floor's arithmetic
                       reconcile.

A bucket's note is printed only when that bucket has something in it.

The line shown under each record is the one lcov attributes the record to,
which for a condition wrapped over several lines is often the statement AFTER
it. Read it as a pointer, not as the condition itself.

Arguments: [source.c] [coverage.info]
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.normpath(os.path.join(
    ROOT, sys.argv[1] if len(sys.argv) > 1 else "src/sim/brain.c"))
ART = os.path.normpath(os.path.join(
    ROOT, sys.argv[2] if len(sys.argv) > 2 else "build-cov/coverage.info"))
REL = os.path.relpath(SRC, ROOT)

# Keyed on (block, branch): lcov allows two blocks on one line to reuse a
# branch index, so keying on the index alone would silently drop records.
by_line = defaultdict(dict)
cur = None
for line in open(ART):
    line = line.rstrip("\n")
    if line.startswith("SF:"):
        cur = line[3:]
    elif line.startswith("BRDA:") and cur and cur.endswith(SRC):
        ln, blk, br, taken = line[5:].split(",")
        by_line[int(ln)][(blk, br)] = taken

# The line as WRITTEN, not as munged: lcov attributes a branch to the line the
# condition sits on, and a reader checking this list needs to see that line.
src = [t.strip() for t in open(SRC).read().split("\n")]


def is_loop(ln):
    return re.match(r"\s*(for|while)\s*\(", src[ln - 1]) is not None


total = sum(len(b) for b in by_line.values())
missed = sum(1 for b in by_line.values() for t in b.values() if t == "0")
dropped = sum(1 for b in by_line.values() for t in b.values() if t == "-")
hit = total - missed - dropped

arms, loops, excluded = [], [], []
for ln, blocks in sorted(by_line.items()):
    zeros = [k for k, t in blocks.items() if t == "0"]
    dashes = [k for k, t in blocks.items() if t == "-"]
    if zeros:
        (loops if is_loop(ln) else arms).append((ln, len(zeros),
                                                  len(blocks), src[ln - 1]))
    if dashes:
        excluded.append((ln, len(dashes), len(blocks), src[ln - 1]))


def show(title, rows, note):
    # An empty bucket prints its header and nothing else: a note under an
    # empty bucket would describe a bucket that is not there.
    if not rows:
        print(f"{title} (0 lines)\n")
        return
    plural = "line" if len(rows) == 1 else "lines"
    print(f"{title} ({len(rows)} {plural}):")
    for ln, miss, tot, text in rows:
        print(f"  {ln:5d}  {miss}/{tot}   {text[:66]}")
    print(f"  {note}\n")


print(f"{REL}: {total} branch records, {hit} hit, {missed} never taken, "
      f"{dropped} lcov-excluded\n")
show("NEVER-TAKEN ARMS — decisions the suite does not make",
     arms, "(hit count 0; the line lcov attributes the record to)")
show("LOOP-BOUND RECORDS — gcov's extra per-loop record, listed so it is "
     "not read as an arm",
     loops, "(the script does not claim why each loop misses its exit)")
show("LCOV-EXCLUDED RECORDS — taken == '-', counted in BRF and never in BRH",
     excluded, "(these are why BRF is larger than hit + never-taken)")
