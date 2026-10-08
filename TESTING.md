# Isomata testing stack (CTOL)

CTOL — **C Test Orchestration Layer** — is a dependency-light, DO-178B-style
testing discipline for C. Its point is that "done" is defined by evidence, not
by opinion: a ladder of oracles, each cheap-first, each able to *fail loudly*.

> Keep this file honest. It is the front door to the stack; if a gate, floor or
> command changes, change it here in the same commit.

## How to run

```sh
./meson/check.sh          # fast tier (~1-2 min): coverage + gate
./meson/check.sh --full   # everything: + matrix + fuzz smoke + mutation
```

Individual tools:

```sh
./meson/coverage.sh --html          # build + suite + browsable coverage report
./meson/coverage.sh --gate          # suite + per-module branch floors
./meson/matrix.sh                   # asan / trapv / uchar / schar / valgrind / m32
./meson/fuzz.sh [target ...] --time 60
./meson/mutate.sh [test_mutate_* ...]
```

`meson/check.sh` is the **single definition of "gates pass"** — CI, the
pre-push hook and a human all call it. Tiers are ordered cheap-first and stop
on the first failure. Install the hook once per clone:

```sh
ln -sf ../../meson/pre-push .git/hooks/pre-push
```

## The oracle ladder

Start at the top; only reach for the next rung when the one above cannot
observe the behaviour.

| Rung | Tool | Observes |
|------|------|----------|
| 1. Unit + boundary | `tests/*.c` + `tests/harness.h` | return values, state, edge inputs |
| 2. Branch coverage | `meson/coverage.sh` | which branches no test ever takes |
| 3. Contract macros | `src/verify.h` (`ALWAYS`/`NEVER`/`testcase`) | "impossible" paths that are taken |
| 4. Fault injection | `tests/support/faultinject.*` (`--wrap`) | allocation / I-O failure paths |
| 5. Fuzzing | `fuzz/` + `meson/fuzz.sh` | untrusted-input memory safety |
| 6. Matrix | `meson/matrix.sh` | memory, UB, signedness, leak/UAF |
| 7. Mutation | `tests/support/mutate.*` + `meson/mutate.sh` | whether the oracle actually notices |

## Phases

| Phase | What |
|-------|------|
| P1 | Coverage spine: build-integrated lcov, branch-aware |
| P2 | Contract macros (`ALWAYS` / `NEVER` / `testcase`) + assert policy |
| P3 | Branch closure: boundary tests + the coverage ratchet |
| P4 | Anomaly / fault injection (OOM, I-O fail-after-N) |
| P5 | Fuzzing + seed corpus |
| P6 | Dynamic-analysis matrix (ASan/UBSan/valgrind/-ftrapv, char signedness) |
| P7 | Mutation testing (input-byte) |
| P8 | Process (regression-per-bug, release checklist, gates) |

## Contract macros (`src/verify.h`)

`ISOMATA_DEBUG` is derived from `NDEBUG`.

| Config | `ALWAYS`/`NEVER` | `testcase` |
|--------|------------------|------------|
| debug (default) | assert | off |
| release (`NDEBUG`) | compile away | off |
| coverage (`-Db_coverage=true`) | compile away | live |

- `assert(...)` — a precondition/postcondition; debug only.
- `ALWAYS(X)` / `NEVER(X)` — X is an *invariant* the author asserts cannot be
  false/true. Do **not** wrap a genuinely reachable bound (caller limits,
  malloc failure) — those are plain `if`s.
- `testcase(X)` — a reachable-but-undesirable edge a test has deliberately
  exercised; X must be side-effect free.

`ALWAYS`/`NEVER` compile away in **coverage** builds on purpose: an asserting
ternary adds an assert edge no test can take (an uncoverable branch at every
site). So run the suite twice — debug to enforce contracts, coverage to measure.

## Coverage ratchet

`meson/coverage-min.txt` is a per-module **branch-coverage floor**. It is a
ratchet: floors only go up. `./meson/coverage.sh --gate` fails if a gated module
drops below its floor. List a module the moment it gets a suite, at its current
level; push it to 100 as the suite closes every branch. A module at 100 with a
green gate is a module whose every branch a test has taken.

## The harness (`tests/harness.h`)

Dependency-free. A failed assertion **returns** from the test function
(non-zero); it does not abort — that is what lets fault-injection tests loop
"fail at index i, assert clean, advance". Every test is
`static int test_<name>(void)`; `main` accumulates:

```c
#include "harness.h"

static int test_accepts_valid_input(void) {
    ASSERT_NOT_NULL(parse("ok"));
    return 0;
}

int main(void) {
    int failed = 0;
    RUN(test_accepts_valid_input);
    HARNESS_SUMMARY("mysuite");
}
```

## Fault injection (`tests/support/faultinject.*`)

Link the support file plus `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,
--wrap=free,--wrap=fopen,--wrap=fread,--wrap=fclose,--wrap=fopen64`. Calls from
the linked objects route through the shims; libc-internal calls are untouched.
The pattern is **loop-until-clean**.

## Fuzzing (`fuzz/`)

One `fuzz/fuzz_<target>.c` per target; list the sources it links in
`fuzz/<target>.srcs`. Reach file loaders through an anonymous memfd
(`/proc/self/fd/N`) so nothing touches disk. Seeds and crashers live in
`fuzz/corpus/<target>/`.

## Process (P8)

The automated tiers prove the code is consistent with what the tests check; they
cannot prove the feature is right. The human wrapper:

- **`meson/check.sh`** — the single definition of "gates pass".
- **`meson/pre-push`** — fast tier, **fails closed**. A gate that cannot fail
  is documentation, not a gate.
- **Release checklist** — `RELEASE-CHECKLIST.md`; the real-app smoke step is
  the one no automation replaces.

### Regression discipline

Every user-reported bug gets a test that **fails on the old code and passes on
the new**, named for the symptom (not the fix). Crashing fuzz inputs are kept
permanently in `fuzz/corpus/<target>/` as `regress*`.

## Anti-patterns (learned the hard way)

- **A gate that cannot fail.** A hook ending in `|| true`, or a step that
  "skips" by exiting 0, is not a gate. Skip *loudly*, never swallow a failure.
- **Assertions that create uncoverable branches.** `assert` and `ALWAYS`/`NEVER`
  must be off in coverage builds, or their edges make 100% unreachable.
- **Mutation that misses the oracle.** Input-byte mutation must provably reach
  the asserted values. "All mutants survive" usually means the mutation never
  reached the oracle — verify that before concluding the suite is weak.
- **`--wrap` on a symbol the compiler did not emit.** `_FILE_OFFSET_BITS=64`
  rewrites `fopen` to `fopen64`; wrap the symbol the linker actually sees
  (`objdump -dr ... | grep PLT32`).
- **A silently-zero coverage capture.** lcov's rc knob name is version-specific;
  branch data can vanish with no error. Verify the tool, not just the number.
- **Guard the slot count as well as the byte count.** A resource usually has two
  limits (bytes *and* slots); guarding one does not guard the other.
