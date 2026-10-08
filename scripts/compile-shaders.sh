#!/usr/bin/env bash
# scripts/compile-shaders.sh — compile the GLSL shaders to Vulkan SPIR-V.
#
# Task 7 chose GLSL + glslangValidator because this host has no shadercross
# and no dxc (verified: glslangValidator, spirv-as, spirv-dis present;
# shadercross/dxc absent). The compiled .spv files are COMMITTED under
# assets/shaders/ as the checked-in fallback, so a normal build never needs
# this script. Run it only when a .glsl source changes.
#
# Future option: when SDL_shadercross is available, the same GLSL (or HLSL)
# sources can be cross-compiled to SPIR-V/DXIL/MSL/MSL and this script can
# emit every backend's artifact; today Vulkan SPIR-V is the only target.
#
# Usage: scripts/compile-shaders.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLSLANG="${GLSLANG_VALIDATOR:-glslangValidator}"

if ! command -v "$GLSLANG" >/dev/null 2>&1; then
	echo "compile-shaders: glslangValidator not found (looked for '$GLSLANG')." >&2
	echo "  Install glslang (Void: xbps-install glslang) or set GLSLANG_VALIDATOR." >&2
	exit 1
fi

OUT_DIR="$ROOT/assets/shaders"
mkdir -p "$OUT_DIR"

for stage in vert frag; do
	src="$ROOT/shaders/world.$stage.glsl"
	out="$OUT_DIR/world.$stage.spv"
	[ -f "$src" ] || { echo "compile-shaders: missing $src" >&2; exit 1; }
	# -V: Vulkan SPIR-V; --target-env vulkan1.0 matches SDL_gpu's Vulkan floor.
	"$GLSLANG" -V --target-env vulkan1.0 "$src" -o "$out"
	echo "compiled $src -> $out"
done
