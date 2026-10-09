#!/usr/bin/env bash
# gen-templates.sh — generate the artist-facing tile texturemap templates.
#
# For each sub-voxel shape (half, ramp, half-ramp) this writes two PNGs into
# assets/templates/:
#
#   <shape>-uv-sheet.png  the six material slots laid out in a labelled 3x2
#                         grid, annotated with which slot each face of the
#                         shape samples (and the triangle UV region).
#   <shape>-paint.png     the same six slots as blank empty cells at the real
#                         material texture size (16x16 per face, see
#                         assets/textures/) for art to be painted over.
#
# Requires ImageMagick 7 (`magick`; override with MAGICK=...) and a font.
# Font override with FONT=/path/to.ttf.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/assets/templates"
MAGICK="${MAGICK:-magick}"
FONT="${FONT:-/usr/share/fonts/TTF/DejaVuSansMono.ttf}"
FONT_BOLD="${FONT_BOLD:-/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf}"
FONT_SANS="${FONT_SANS:-/usr/share/fonts/TTF/DejaVuSans.ttf}"
FONT_SANS_BOLD="${FONT_SANS_BOLD:-/usr/share/fonts/TTF/DejaVuSans-Bold.ttf}"

command -v "$MAGICK" >/dev/null 2>&1 || {
  echo "gen-templates: ImageMagick 'magick' not found (set MAGICK=)" >&2
  exit 1
}
[ -f "$FONT" ] || { echo "gen-templates: font not found: $FONT (set FONT=)" >&2; exit 1; }
mkdir -p "$OUT"

# One UV-mapping sheet. Cell colours encode the face-use:
#   green  = top / slope,  purple = bottom,  blue = a side (back face or triangle).
gen_uv_sheet() {
  local shape="$1" title="$2"
  local top_tag="$3" bottom_tag="$4" north_tag="$5" south_tag="$6"
  local east_tag="$7" west_tag="$8"
  local n1="${9:-}" n2="${10:-}" n3="${11:-}" n4="${12:-}" n5="${13:-}"
  local CW=96 X0=40 Y0=96 COLSTEP=136 ROWSTEP=136 W=980 H=470
  local -a letters=(T B N S E W)
  local -a names=("TOP (+Y)" "BOTTOM (-Y)" "NORTH (-Z)" "SOUTH (+Z)" "EAST (+X)" "WEST (-X)")
  local -a tags=("$top_tag" "$bottom_tag" "$north_tag" "$south_tag" "$east_tag" "$west_tag")
  local -a colr=(0 190 0   150 60 200   60 110 210   60 110 210   60 110 210   60 110 210)
  local args=(-size "${W}x${H}" "xc:rgb(246,246,246)")
  local i col row cx cy r g b

  # Title + subtitle.
  args+=(-fill 'rgb(18,18,18)' -stroke none -font "$FONT_SANS_BOLD" -pointsize 27
         -gravity northwest -annotate "+40+26" "$title")
  args+=(-fill 'rgb(95,95,95)' -font "$FONT_SANS" -pointsize 15 -gravity northwest
         -annotate "+40+62" "Six material face slots (16x16 each, assets/textures/); one file may fill all six, or override per face.")

  for ((i = 0; i < 6; i++)); do
    col=$((i % 3)); row=$((i / 3))
    cx=$((X0 + col * COLSTEP)); cy=$((Y0 + row * ROWSTEP))
    r=${colr[$((i * 3))]}; g=${colr[$((i * 3 + 1))]}; b=${colr[$((i * 3 + 2))]}
    args+=(-fill "rgb($r,$g,$b)" -stroke 'rgb(25,25,25)' -strokewidth 2
           -draw "rectangle $((cx)),$((cy)) $((cx + CW - 1)),$((cy + CW - 1))")
    args+=(-fill 'rgba(0,0,0,0.85)' -stroke none -font "$FONT_BOLD" -pointsize 54
           -gravity northwest -annotate "+$((cx + 24))+$((cy + 20))" "${letters[$i]}")
    args+=(-fill 'rgb(18,18,18)' -font "$FONT_SANS_BOLD" -pointsize 15 -gravity northwest
           -annotate "+$cx+$((cy - 20))" "${names[$i]}")
    args+=(-fill 'rgb(55,55,55)' -font "$FONT_SANS" -pointsize 13 -gravity northwest
           -annotate "+$cx+$((cy + CW + 6))" "${tags[$i]}")
  done

  # Annotation column on the right.
  local ax=$((X0 + 3 * COLSTEP + 14))
  args+=(-fill 'rgb(18,18,18)' -font "$FONT_SANS_BOLD" -pointsize 16 -gravity northwest
         -annotate "+$ax+$Y0" "Which slot each face samples")
  local yy=$((Y0 + 32))
  for line in "$n1" "$n2" "$n3" "$n4" "$n5"; do
    [ -n "$line" ] || continue
    args+=(-fill 'rgb(40,40,40)' -font "$FONT_SANS" -pointsize 14 -gravity northwest
           -annotate "+$ax+$yy" "$line")
    yy=$((yy + 32))
  done
  args+=("$OUT/${shape}-uv-sheet.png")
  "$MAGICK" "${args[@]}"
}

# The blank paint-over sheet: six empty 16x16 cells (3 cols x 2 rows) with a
# 1 px gap, matching the face texture size exactly.
gen_paint() {
  local shape="$1" CW=16 GAP=1 W H
  W=$(( 3 * CW + 2 * GAP ))
  H=$(( 2 * CW + 1 * GAP ))
  "$MAGICK" -size "${W}x${H}" xc:none \
    -fill none -stroke 'rgba(95,95,95,0.75)' -strokewidth 1 \
    -draw "rectangle 0,0 $((W - 1)),$((H - 1))" \
    -draw "line $((CW)),0 $((CW)),$((H - 1))" \
    -draw "line $((2 * CW + GAP)),0 $((2 * CW + GAP)),$((H - 1))" \
    -draw "line 0,$((CW)) $((W - 1)),$((CW))" \
    "$OUT/${shape}-paint.png"
}

# --- half -----------------------------------------------------------------
gen_uv_sheet half "Isomata tile - HALF (bottom slab)" \
  "top face (y+0.5)" "bottom face" "side" "side" "side" "side" \
  "top   (1x1 at y+0.5)   -> TOP slot" \
  "bottom (1x1 at y)      -> BOTTOM slot" \
  "four sides 0.5 high    -> N/S/E/W slots" \
  "no slope, no triangles"
gen_paint half

# --- ramp -----------------------------------------------------------------
gen_uv_sheet ramp "Isomata tile - RAMP (full-height wedge, rise 1.0)" \
  "slope" "bottom face" "back/tri by dir" "back/tri by dir" "back/tri by dir" "back/tri by dir" \
  "slope (tilted 1x1)      -> TOP slot (never culled)" \
  "bottom (1x1 at y)       -> BOTTOM slot" \
  "back face (rise dir)    -> that side slot, height 1.0" \
  "2 triangles -> the 2 side slots _perpendicular_ to the rise" \
  "triangle UV: rendered tri samples BL,BR,TR of its side slot"
gen_paint ramp

# --- half-ramp ------------------------------------------------------------
gen_uv_sheet half-ramp "Isomata tile - HALF_RAMP (half-height wedge, rise 0.5)" \
  "slope" "bottom face" "back/tri by dir" "back/tri by dir" "back/tri by dir" "back/tri by dir" \
  "slope (tilted, rise 0.5) -> TOP slot (never culled)" \
  "bottom (1x1 at y)        -> BOTTOM slot" \
  "back face (rise dir)     -> that side slot, height 0.5" \
  "2 triangles -> the 2 side slots _perpendicular_ to the rise" \
  "the side opposite the rise is unused (low edge is open)"
gen_paint half-ramp

echo "gen-templates: wrote to $OUT:"
ls -1 "$OUT"/*.png
