# All-sides tile screenshots

In-engine captures of each sub-voxel tile shape (`half`, `ramp`, `half-ramp`)
from the four axis-aligned camera yaws **0 / 90 / 180 / 270 degrees**, taken
under Xvfb from the specimen map `assets/maps/tiles/`.

The tiles sample the face-labelled `tile` material
(`assets/textures/tile_*.png`): the big letter on each face is the slot —
**T**op (green), **B**ottom (purple), **N**orth `-z` (blue), **S**outh `+z`
(orange), **E**ast `+x` (red), **W**est `-x` (teal). A camera at yaw `0` looks
at the tile from the **south (`+Z`)**; yaw `90` from the **east**, `180` from
the **north**, `270` from the **west** (see
[`../../tile-format-spec.md`](../../tile-format-spec.md) §1).

Each tile sits on ground level 1 above a stone floor (level 0), framed at max
zoom. The red billboards are the level scene's three sprite entities (fixed
positions, independent of the map). The map carries a warm point light at
`(8, 2, 8)` — one air layer above the ground, inside the level's light grid
(whose height is `levels + 1`) — which warms the tiles.

## Files

| File pattern | Shape / rise direction |
|---|---|
| `ramp-north-yaw{000,090,180,270}.png` | `ramp` dir=`north` (rises toward `-z`) |
| `ramp-east-yaw{000,090,180,270}.png` | `ramp` dir=`east` (rises toward `+x`) |
| `half-yaw{000,090,180,270}.png` | `half` (bottom slab, surface at `y+0.5`) |
| `half-ramp-north-yaw{000,090,180,270}.png` | `half-ramp` dir=`north` (rise 0.5) |
| `<shape>-montage.png` | that shape's four yaws in one row |
| `all-shapes-montage.png` | all 16 captures in a 4x4 grid |

## What each capture shows

- **ramp-north**
  - yaw 000 — the **slope** faces the camera (`TOP` slot, green "T"); the low
    edge is nearest.
  - yaw 090 — slope seen from the side; the **east triangle** (`EAST` slot, red
    "E") is visible under it.
  - yaw 180 — the full-height **back face** (`NORTH` slot, blue "N") faces the
    camera; the slope is occluded behind it.
  - yaw 270 — slope from the other side; the **west triangle** (`WEST` slot,
    teal "W").
- **ramp-east**
  - yaw 000 — slope plus the **south triangle** (`SOUTH` slot, orange "S").
  - yaw 090 — the **back face** (`EAST` slot, red "E") faces the camera.
  - yaw 180 — slope plus the **north triangle** (`NORTH` slot, blue "N").
  - yaw 270 — the **slope faces the camera nearly face-on** (its normal points
    up-west): a full green "T" quad. The east back face is culled (dot < 0) and
    both triangles are edge-on (dot ≈ 0); nothing else is visible.
- **half**
  - yaw 000 / 090 / 180 / 270 — the flat **top** (`TOP`, green "T") plus the
    camera-facing 0.5-high side: south (`S`), east (`E`), north (`N`), west
    (`W`) respectively.
- **half-ramp-north** — as `ramp-north` but the slope drops from `y+0.5`:
  - yaw 000 slope; yaw 090 slope + east triangle; yaw 180 back face (blue "N");
    yaw 270 slope + west triangle.

Regenerate the specimen map with `./scripts/gen-tiles-map.sh`; the captures
were produced by swapping that map into the staged build tree and driving the
app under Xvfb (see the T18 report). Exact geometry, material slots, culling
and lighting: [`../../tile-format-spec.md`](../../tile-format-spec.md) §4–5.
