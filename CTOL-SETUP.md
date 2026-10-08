# CTOL setup — isomata

- root: `/home/krang/proj/isomata`
- symbol prefix: `ISOMATA` (macros are `ISOMATA_DEBUG`, `ISOMATA_COVERAGE_TEST`, `ISOMATA_MUTATE`)
- coverage build dir: `build-cov`

## Files added (19)
- `.clang-format`
- `RELEASE-CHECKLIST.md`
- `TESTING.md`
- `fuzz/corpus/.gitignore`
- `meson/check.sh`
- `meson/coverage-gate.sh`
- `meson/coverage-min.txt`
- `meson/coverage.sh`
- `meson/fuzz.sh`
- `meson/matrix.sh`
- `meson/mutate.sh`
- `meson/pre-push`
- `src/verify.h`
- `tests/harness.h`
- `tests/support/faultinject.c`
- `tests/support/faultinject.h`
- `tests/support/mutate.c`
- `tests/support/mutate.h`
- `valgrind.supp`

## Skipped — already present (1)
- `tests/meson.build`

## Wire the build (manual — meson.build is project-specific)

In the parent build, before `subdir('tests')`:
```meson
ctol_lib  = static_library(...)      # the objects under test
ctol_inc  = include_directories('include', 'src')
ctol_deps = [m]                      # system/vendored deps
subdir('tests')
```

Coverage builds must define the test config (top-level `meson.build`):
```meson
if get_option('b_coverage')
  add_project_arguments('-DISOMATA_COVERAGE_TEST=1', '-DNDEBUG', language: 'c')
endif
```

Install the fast pre-push gate once per clone:
```sh
ln -sf ../../meson/pre-push .git/hooks/pre-push
```

Then: `./meson/check.sh` (fast tier) and `./meson/check.sh --full`.
