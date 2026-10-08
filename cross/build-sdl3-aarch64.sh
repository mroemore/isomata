#!/usr/bin/env bash
# Build an aarch64 target sysroot with SDL3 + SDL3_ttf (vendored FreeType),
# enough to cross-build AND run the full Isomata app under qemu-aarch64.
#
# Usage:
#   cross/build-sdl3-aarch64.sh
#   ISO_ARM_SYSROOT=/some/sysroot cross/build-sdl3-aarch64.sh
#
# Defaults: <repo>/.tmp_files/arm-sdl/sysroot, scratch under
# <repo>/.tmp_files/arm-sdl/src (both gitignored). Requires the
# aarch64-linux-gnu toolchain, cmake, ninja, git, curl.
#
# After it prints "sysroot ready", build the app with:
#   PKG_CONFIG_LIBDIR=$ISO_ARM_SYSROOT/lib/pkgconfig \
#       meson setup build-aarch64 --cross-file cross/aarch64.ini
#   ninja -C build-aarch64
#   qemu-aarch64-static -L /usr/aarch64-linux-gnu \
#       -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E ISO_SMOKE_MS=300 \
#       build-aarch64/isomata
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${ISO_ARM_WORK:-$ROOT/.tmp_files/arm-sdl}"
SYSROOT="${ISO_ARM_SYSROOT:-$WORK/sysroot}"
SRC="$WORK/src"
SDL3_VER=3.4.14
SDLTTF_VER=3.2.2
JOBS="$(nproc)"

for tool in aarch64-linux-gnu-gcc cmake ninja git curl; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "build-sdl3-aarch64: missing tool: $tool" >&2
		exit 2
	}
done

mkdir -p "$SRC"
cd "$SRC"

fetch() { # <url> <filename>
	[ -f "$2" ] || curl -fsSL -o "$2" "$1"
}

SDL3_TARBALL="SDL3-$SDL3_VER.tar.gz"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VER/$SDL3_TARBALL" "$SDL3_TARBALL"
[ -d "SDL3-$SDL3_VER" ] || tar xzf "$SDL3_TARBALL"

TTF_TARBALL="SDL3_ttf-$SDLTTF_VER.tar.gz"
fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$SDLTTF_VER/$TTF_TARBALL" "$TTF_TARBALL"
[ -d "SDL3_ttf-$SDLTTF_VER" ] || tar xzf "$TTF_TARBALL"

cat > "$SRC/toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

echo ">> SDL3 $SDL3_VER -> $SYSROOT"
cmake -S "SDL3-$SDL3_VER" -B sdl3-build -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$SRC/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$SYSROOT" \
	-DSDL_SHARED=ON -DSDL_STATIC=ON \
	-DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
	-DSDL_UNIX_CONSOLE_BUILD=ON \
	-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OFFSCREEN=OFF \
	-DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
	-DSDL_JACK=OFF -DSDL_SNDIO=OFF \
	-DSDL_VULKAN=OFF -DSDL_RPI=OFF -DSDL_ROCKCHIP=OFF
cmake --build sdl3-build -j"$JOBS"
cmake --install sdl3-build

echo ">> SDL3_ttf $SDLTTF_VER (vendored FreeType) -> $SYSROOT"
( cd "SDL3_ttf-$SDLTTF_VER" && ./external/download.sh >/dev/null )
cmake -S "SDL3_ttf-$SDLTTF_VER" -B ttf-build -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$SRC/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$SYSROOT" \
	-DSDL3_DIR="$SYSROOT/lib/cmake/SDL3" \
	-DSDLTTF_VENDORED=ON -DSDLTTF_HARFBUZZ=OFF -DSDLTTF_PLUTOSVG=OFF \
	-DSDLTTF_SAMPLES=OFF -DSDLTTF_TESTS=OFF
cmake --build ttf-build -j"$JOBS"
cmake --install ttf-build

echo "sysroot ready: $SYSROOT"
echo "  PKG_CONFIG_LIBDIR=$SYSROOT/lib/pkgconfig meson setup build-aarch64 --cross-file cross/aarch64.ini"
echo "  ninja -C build-aarch64"
echo "  qemu-aarch64-static -L /usr/aarch64-linux-gnu -E SDL_VIDEODRIVER=dummy -E SDL_AUDIODRIVER=dummy -E ISO_SMOKE_MS=300 build-aarch64/isomata"
