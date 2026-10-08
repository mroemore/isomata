#!/usr/bin/env bash
# scripts/make-placeholder-texture.sh — regenerate the committed placeholder
# texture (assets/textures/placeholder.png) used by the Task 7 static quad.
#
# The PNG is committed, so a normal build never runs this. It is kept here
# so the asset is reproducible. Needs ImageMagick (magick or convert).
#
# The image is 64x64: four solid colour quadrants (red TL, green TR, blue
# BL, yellow BR) plus a black dot near the top-left — enough to see the
# texture orientation and that nearest sampling works.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/assets/textures/placeholder.png"

MAGICK=""
if command -v magick >/dev/null 2>&1; then
	MAGICK=magick
elif command -v convert >/dev/null 2>&1; then
	MAGICK=convert
else
	echo "make-placeholder-texture: ImageMagick (magick/convert) not found" >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")"
"$MAGICK" -size 64x64 xc:none \
	-fill '#e04040' -draw 'rectangle 0,0 31,31' \
	-fill '#40e040' -draw 'rectangle 32,0 63,31' \
	-fill '#4040e0' -draw 'rectangle 0,32 31,63' \
	-fill '#e0e040' -draw 'rectangle 32,32 63,63' \
	-fill '#000000' -draw 'rectangle 4,4 11,11' \
	"$OUT"
echo "wrote $OUT"
