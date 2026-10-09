#!/usr/bin/env bash
# gen-tile-textures.sh — write the face-labelled "tile" material textures.
#
# Six 16x16 PNGs under assets/textures/, one per cube face, each a big letter
# (T/B/N/S/E/W) so a shape's orientation is obvious in-engine and in the
# committed all-sides screenshots. Referenced by the `tile` material line in
# assets/textures/materials.txt.
#
# Requires ImageMagick 7 (`magick`; override with MAGICK=...).
set -euo pipefail

ROOT="$(cd "$(dirname "$(dirname "${BASH_SOURCE[0]}")")" && pwd)"
OUT="$ROOT/assets/textures"
MAGICK="${MAGICK:-magick}"
FONT="${FONT:-/usr/share/fonts/TTF/DejaVuSans-Bold.ttf}"

command -v "$MAGICK" >/dev/null 2>&1 || {
  echo "gen-tile-textures: ImageMagick 'magick' not found (set MAGICK=)" >&2
  exit 1
}

# letter file r g b
face() {
  "$MAGICK" -size 16x16 "xc:rgb($3,$4,$5)" \
    -fill none -stroke 'rgba(0,0,0,0.55)' -strokewidth 1 \
    -draw "rectangle 0,0 15,15" \
    -stroke none -fill white -font "$FONT" -pointsize 14 -gravity center \
    -annotate +0+1 "$1" "$OUT/$2"
}

face T tile_top.png    0 170 40
face B tile_bottom.png 150 60 200
face N tile_north.png  40 90 220
face S tile_south.png  235 140 20
face E tile_east.png   210 45 45
face W tile_west.png   0 165 165

echo "gen-tile-textures: wrote:"
ls -1 "$OUT"/tile_*.png
