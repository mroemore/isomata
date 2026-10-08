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

### GUI runs: Xvfb only (user directive)

**Every automated GUI/input run goes under Xvfb — never on the live display.**
Popping windows onto the user's desktop and hardware beeps from test audio are
unacceptable. Always set `SDL_AUDIODRIVER=dummy` for automated runs.

```sh
Xvfb :77 -screen 0 1280x720x24 -nolisten tcp &   # pick a free display (:99 is taken)
DISPLAY=:77 SDL_AUDIODRIVER=dummy ISO_SMOKE_MS=2500 build/isomata &
sleep 1.6
DISPLAY=:77 import -window root shot.png          # xwd/scrot also available
DISPLAY=:77 xdotool key Return / xdotool click N  # input injection
kill %1 %2
```

Verified on this box: `gpu_backend: ready (driver vulkan)` under Xvfb and the
menu renders in the captured PNG. Interactive hand-testing on the real display
is the user's own; agents do not use it.

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

Wired by `tests/test_faultinject.c`: built with that wrap recipe in
`tests/meson.build` and compiled with `-DISOMATA_HARNESS_FAULTINJECT` (the
`tests/harness.h` opt-in), it sweeps the failure index across every allocation
of a module and asserts a clean NULL with no leak. Modules covered: `events.c`
(create/publish/subscribe), `input.c` (create), `scene.c`
(createScene/createSceneStack), `drawlist.c` (init), `achievement.c` (create).

## Fuzzing (`fuzz/`)

One `fuzz/fuzz_<target>.c` per target; list the sources it links in
`fuzz/<target>.srcs`. Reach file loaders through an anonymous memfd
(`/proc/self/fd/N`) so nothing touches disk. Seeds and crashers live in
`fuzz/corpus/<target>/`.

The in-tree target is `fuzz/fuzz_parsevoxmap.c` (`parseVoxmapText`, the
length-bounded parser that consumes untrusted map bytes); its linked sources
are in `fuzz/parsevoxmap.srcs` and its seeds in `fuzz/corpus/parsevoxmap/`.
`meson/fuzz.sh` exits non-zero when it finds no target — no-target is a
configuration error, never a PASS.

## Mutation (`tests/test_mutate_*`)

Input-byte mutation (`tests/support/mutate.*`): each target is a standalone
executable that flips bits of the input a parser consumes and checks the code
under test against an independent oracle, reporting a `killed=/survived=`
count. `meson/mutate.sh` discovers `tests/test_mutate_*.c`, builds them with
`-DISOMATA_MUTATE`, and fails on a survivor or a missing target — no-target is
a configuration error, never a PASS. The in-tree target is
`tests/test_mutate_parsevoxmap.c`; it is registered as a normal pure test too,
so the same oracle guards `parseVoxmapText` on every build.

## Dynamic-analysis matrix (P6)

`meson/matrix.sh [config ...]` builds the suite under each configuration and
runs it; `./meson/check.sh --full` includes it.

| Config | Tool | Notes |
|--------|------|-------|
| `asan` | clang `-Db_sanitize=address,undefined` | GCC on this box ships **no** `libasan`/`libubsan`, so the sanitizer build uses clang (`/usr/lib/llvm/21/bin/clang`, or `FUZZ_CC`). The `sdl3-ttf` wrap is configured `default_options : ['b_sanitize=none']` in `meson.build`: its cmake-method shared-library link does not receive the sanitizer runtime flags, so instrumenting the vendored subproject fails to link (`undefined reference to __asan_*`). First-party targets are instrumented normally. |
| `trapv` / `uchar` / `schar` | gcc `-ftrapv` / `-funsigned-char` / `-fsigned-char` | |
| `valgrind` | plain build, every test under valgrind | the leak/UAF rung that substitutes for ASan's leak checker on this host (memcheck errors gate; leaks are reported, not gated). |
| `m32` | gcc `-m32` | skipped without 32-bit multilib (skipped here). |

Leak detection is deliberately **off** for the asan config
(`ASAN_OPTIONS=detect_leaks=0`): third-party libs leak and first-party leaks are
covered by the valgrind rung.

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
