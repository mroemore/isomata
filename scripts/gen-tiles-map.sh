#!/usr/bin/env bash
# gen-tiles-map.sh — regenerate the specimen PNG slice map used for the T18
# all-sides screenshots (assets/maps/tiles/) from its ASCII source of truth
# assets/maps/tiles.txt.
#
# The specimen map is a 16x16 ground of stone (level 0) with four sub-voxel
# tiles on top (level 1), one per shape, spaced to the corners so each is
# isolated:
#   (2,2)  ramp north      (13,2)  ramp east
#   (2,13) half            (13,13) half-ramp north
# A warm point light at (8,2,8) sits one air layer above the ground (inside the
# level's light grid, whose height is levels+1 = 3) and lights the specimen.
# The tiles sample the face-labelled "tile" material (assets/textures/tile_*.png)
# so orientation is obvious in the captures.
#
# Requires ImageMagick 7 (`magick`; override with MAGICK=...).
set -euo pipefail

ROOT="$(cd "$(dirname "$(dirname "${BASH_SOURCE[0]}")")" && pwd)"
SRC="$ROOT/assets/maps/tiles.txt"
OUTDIR="$ROOT/assets/maps/tiles"
TMP="$ROOT/.tmp_files/gen-tiles-map"
MAGICK="${MAGICK:-magick}"

command -v "$MAGICK" >/dev/null 2>&1 || {
  echo "gen-tiles-map: ImageMagick 'magick' not found (set MAGICK=)" >&2
  exit 1
}
[ -f "$SRC" ] || { echo "gen-tiles-map: missing $SRC" >&2; exit 1; }

mkdir -p "$OUTDIR" "$TMP"
rm -f "$TMP"/slice*.txt "$TMP"/legend-lights.txt "$OUTDIR"/*.png "$OUTDIR/legend.txt"

# Character -> RGBA. '.' (and any unmapped char) is transparent air. Keep in
# sync with the legend colours written below.
awk -v tmp="$TMP" '
function tuple(ch,   r,g,b,a) {
  if (ch=="s")      { r=128; g=128; b=128; a=255 }   # stone
  else if (ch=="R") { r=168; g=168; b=168; a=255 }   # tile ramp north
  else if (ch=="G") { r=152; g=152; b=152; a=255 }   # tile ramp east
  else if (ch=="H") { r=200; g=200; b=200; a=255 }   # tile half
  else if (ch=="r") { r=184; g=184; b=184; a=255 }   # tile half-ramp north
  else              { r=0;   g=0;   b=0;   a=0 }     # "." = air
  return sprintf("%d,%d,%d,%d", r, g, b, a)
}
function hex(ch,   t,s) {
  t = tuple(ch)
  split(t, s, ",")
  return sprintf("#%02X%02X%02X%02X", s[1], s[2], s[3], s[4])
}
function flush(   x,y,ch,file,row) {
  if (nrows == 0) return
  file = sprintf("%s/slice%02d.txt", tmp, sec)
  printf "# ImageMagick pixel enumeration: %d,%d,255,srgba\n", width, nrows > file
  for (y = 0; y < nrows; y++) {
    row = rows[y]
    for (x = 0; x < width; x++) {
      ch = substr(row, x + 1, 1)
      printf "%d,%d: (%s)  %s\n", x, y, tuple(ch), hex(ch) >> file
    }
  }
  close(file)
  sec++
  nrows = 0
  width = 0
}
/^---[[:space:]]*$/ { flush(); next }
/^@/ { next }
/^\$/ { if (nrows > 0) flush(); print >> (tmp "/legend-lights.txt"); next }
/^[[:space:]]*$/ { next }
{
  if (width == 0) width = length($0)
  rows[nrows++] = $0
}
END { flush() }
' "$SRC"

n=0
for f in "$TMP"/slice*.txt; do
  out="$(printf '%s/%02d.png' "$OUTDIR" "$n")"
  "$MAGICK" "txt:$f" -strip "PNG32:$out"
  n=$((n + 1))
done

{
  echo "#808080 stone"
  echo "#A8A8A8 tile shape=ramp dir=north"
  echo "#989898 tile shape=ramp dir=east"
  echo "#C8C8C8 tile shape=half"
  echo "#B8B8B8 tile shape=half-ramp dir=north"
  cat "$TMP/legend-lights.txt"
} > "$OUTDIR/legend.txt"

echo "gen-tiles-map: wrote $n slice PNG(s) + legend.txt to $OUTDIR"
