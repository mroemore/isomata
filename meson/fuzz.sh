#!/usr/bin/env bash
# meson/fuzz.sh [target ...] [--time SECONDS] [--build-only]
#
# Build and run the libFuzzer harnesses in fuzz/ with ASan+UBSan.
# Targets are discovered from fuzz/fuzz_<target>.c, or named explicitly.
#
# Per-target link recipe: list the extra source files to compile alongside the
# harness in fuzz/<target>.srcs (one path per line, relative to the repo root,
# '#' comments allowed). That is where the code under test is pulled in. The
# include path / link library shape for the whole project lives in the
# -I"$ROOT/src" / -lm substitutions below — edit them once.
#
# Requires a clang with the libFuzzer runtime (compiler-rt). Override with FUZZ_CC.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build-fuzz"
TIME=60
BUILD_ONLY=0
TARGETS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --time) TIME="$2"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    -*) echo "fuzz.sh: unknown option: $1" >&2; exit 2 ;;
    *) TARGETS+=("$1"); shift ;;
  esac
done

if [ "${#TARGETS[@]}" -eq 0 ]; then
  while IFS= read -r f; do
    TARGETS+=("$(basename "$f" .c | sed 's/^fuzz_//')")
  done < <(find "$ROOT/fuzz" -maxdepth 1 -name 'fuzz_*.c' 2>/dev/null | sort)
fi
if [ "${#TARGETS[@]}" -eq 0 ]; then
  echo "fuzz.sh: no targets (add fuzz/fuzz_<target>.c); skipping"
  exit 0
fi

have_fuzzer() {
  printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){return (int)(d?s:0);}\n' \
    | "$1" -x c -fsanitize=fuzzer -o /dev/null - >/dev/null 2>&1
}

find_clang() {
  for c in ${FUZZ_CC:-} /usr/lib/llvm/21/bin/clang /usr/lib/llvm/20/bin/clang \
           /usr/lib/llvm/19/bin/clang clang; do
    [ -n "$c" ] || continue
    command -v "$c" >/dev/null 2>&1 || continue
    if have_fuzzer "$c"; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

CC="$(find_clang)" || {
  echo "fuzz.sh: no clang with libFuzzer runtime found (set FUZZ_CC)" >&2
  exit 2
}
echo ">> compiler: $CC"

CFLAGS=(-g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -I"$ROOT/src")

mkdir -p "$OUT"

for t in "${TARGETS[@]}"; do
  echo ">> build $t"
  extra=()
  if [ -f "$ROOT/fuzz/$t.srcs" ]; then
    while IFS= read -r s; do
      [ -n "$s" ] && extra+=("$ROOT/$s")
    done < <(grep -vE '^[[:space:]]*(#|$)' "$ROOT/fuzz/$t.srcs")
  fi
  "$CC" "${CFLAGS[@]}" "$ROOT/fuzz/fuzz_$t.c" "${extra[@]}" -lm -o "$OUT/fuzz_$t"
  if [ "$BUILD_ONLY" = 1 ]; then
    continue
  fi
  corpus="$ROOT/fuzz/corpus/$t"
  mkdir -p "$corpus"
  echo ">> fuzz $t for ${TIME}s"
  ASAN_SYMBOLIZER_PATH="$("$CC" --print-prog-name=llvm-symbolizer 2>/dev/null)" \
    "$OUT/fuzz_$t" -max_total_time="$TIME" -artifact_prefix="$OUT/" "$corpus"
done
