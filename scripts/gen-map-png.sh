#!/usr/bin/env bash
# scripts/gen-map-png.sh — regenerate the PNG slice version of the ASCII demo
# map (assets/maps/demo.txt) into assets/maps/demo/.
#
# The ASCII map is the source of truth: this script splits it into its `---`
# sections (ascending = bottom slice first), converts each character to the
# legend colour and writes one PNG per section, plus legend.txt (the colour
# legend and the same `$` light lines). The committed PNGs are the equivalence
# fixture (tests/test_pngmap.c) and the app's demo map.
#
# Requires ImageMagick 7 (`magick`; override with MAGICK=...). Re-run after
# editing demo.txt or the colour map below.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/assets/maps/demo.txt"
OUTDIR="$ROOT/assets/maps/demo"
TMP="$ROOT/.tmp_files/gen-map-png"
MAGICK="${MAGICK:-magick}"

command -v "$MAGICK" >/dev/null 2>&1 || {
  echo "gen-map-png: ImageMagick 'magick' not found (set MAGICK=)" >&2
  exit 1
}
[ -f "$SRC" ] || { echo "gen-map-png: missing $SRC" >&2; exit 1; }

mkdir -p "$OUTDIR" "$TMP"
rm -f "$TMP"/slice*.txt "$TMP"/legend-lights.txt "$OUTDIR"/*.png "$OUTDIR/legend.txt"

# Character -> colour map. '.' (and any unmapped char) is transparent air.
# Keep this in sync with the legend.txt colours written below.
awk -v tmp="$TMP" '
function tuple(ch,   r,g,b,a) {
  if (ch=="g")      { r=0;   g=255; b=0;   a=255 }   # grass
  else if (ch=="s") { r=128; g=128; b=128; a=255 }   # stone
  else if (ch=="w") { r=160; g=82;  b=45;  a=255 }   # wood
  else if (ch=="f") { r=0;   g=0;   b=255; a=255 }   # sixface
  else if (ch=="a") { r=255; g=0;   b=255; a=255 }   # foliage
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
  echo "#00FF00 grass"
  echo "#808080 stone"
  echo "#A0522D wood"
  echo "#0000FF sixface"
  echo "#FF00FF foliage"
  cat "$TMP/legend-lights.txt"
} > "$OUTDIR/legend.txt"

echo "gen-map-png: wrote $n slice PNG(s) + legend.txt to $OUTDIR"
