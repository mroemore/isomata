# Isomata tile-format specification (v1)

This document specifies the Isomata voxel-tile map format precisely enough
that a drawing tool or level editor can read and write it **without an Isomata
checkout**. It covers the map-directory format, the tile (voxel) model and the
sub-voxel shape geometry.

Every constant and geometry table here was verified against the implementation
at the revision named below; the geometry tables are additionally pinned by the
tests named in each section.

- **Spec version:** 1 (against Isomata commit `8204c37`).
- **Implementation of record:** `src/render/voxmap.h` (tile model, shapes,
  culling, lighting), `src/render/voxmap.c` (parser + face emission),
  `src/render/mapsource.h` / `src/render/mapsource.c` (PNG slice directory),
  `src/render/materials.h` (six-face material slots), `src/render/map_loader.c`
  (PNG loader glue).

The map has **two equivalent encodings**: a directory of numbered PNG slices
(this document's main subject) and a single ASCII text file (section 8). Both
produce exactly the same tile model; only the bytes on disk differ. The two are
held equal by `tests/test_pngmap.c`, which loads the committed demo both ways
and asserts the results are identical.

---

## 1. Coordinate system

- **x** grows **east**, **y** grows **up**, **z** grows **south**.
- The map's **top row is `z = 0`** (north). Row index `z` increases southward.
- A **cell** `(x, y, z)` is the unit cube `[x, x+1] × [y, y+1) × [z, z+1)` in
  world space. A cell is either **air** or a **solid tile** with a material and
  a shape.
- **Slice index = y.** Slice `0` is the bottom level (`y = 0`); slices stack
  upward.
- Direction names (used by `dir=`):
  - **north** = `-z` (toward the top row of the image),
  - **south** = `+z`,
  - **east** = `+x`,
  - **west** = `-x`.
- For a tile at `(x, y, z)` these **cell-local** coordinates are used throughout
  section 5 (the tile's own `x`, `y`, `z` are the local origin):
  - `X = world_x − tile_x`, i.e. `0..1` across the cell west→east,
  - `Z = world_z − tile_z`, i.e. `0..1` across the cell north→south,
  - `Y = world_y − tile_y`, i.e. `0` at the cell floor.
  A table entry `(cx, cy, cz)` means world point `(tile_x + cx, tile_y + cy, tile_z + cz)`.

The camera is a fixed-pitch (35.264°, true isometric) orbit. Camera **yaw**
`θ` places the camera on the ground-plane direction `(sin θ, cos θ)` **from**
the target (so yaw `0` looks at the map from `+Z`/south, yaw `90` from
`+X`/east, yaw `180` from `-Z`/north, yaw `270` from `-X`/west). Yaw is used
only for face culling (section 5.4) and light sampling; it does not change the
stored model.

---

## 2. Directory format

A map is a directory, e.g. `assets/maps/demo/`:

```
<map-directory>/
  NN.png        one PNG per horizontal slice (the layer PNGs)
  legend.txt    the colour legend + optional lights
```

### 2.1 Slice PNGs

- **One PNG per horizontal slice.** A file is a slice if its name ends in
  `.png` (ASCII-case-insensitive; `src/render/map_loader.c:35`). Other files are
  ignored.
- **Slice order is the *natural sort* of the file names**, ascending:
  `9.png` sorts before `10.png`, and runs of equal numeric value order by
  *fewer* leading zeros first (`1.png` sorts before `01.png`). The comparator
  is `mapSourceNaturalCompare` (`src/render/mapsource.c:89`), pinned by
  `tests/test_mapsource.c`.
- **Ascending order is bottom first:** the first file is level `y = 0`, the
  next is `y = 1`, and so on.
- **Every slice must be the same `width × height`.** A mismatch fails the load
  (`src/render/mapsource.c:363`).
- **Limits** (`src/render/voxmap.h:242,246`, `src/render/mapsource.h:57`,
  `src/render/voxmap.h:252`):
  - `width ≤ 256`, `height ≤ 256`, `slices ≤ 256` (`VOXMAP_MAX_DIM` = 256);
  - `width × height × slices ≤ 1,048,576` (`VOXMAP_MAX_CELLS` = `1 << 20`);
  - at most 64 distinct legend colours (`MAPSOURCE_MAX_COLORS`);
  - at most 64 `$` light lines (`VOXMAP_MAX_LIGHTS`).
  A map over any limit fails to load with a diagnostic (it never allocates the
  huge volume).

### 2.2 Pixel ↔ cell mapping

- One pixel is one cell.
- **Image column `x` → cell `x`.** Column `0` is the west edge.
- **Image row `z` → cell `z`.** Row `0` is the **top** row and is `z = 0`
  (north); row `height-1` is the south edge.
- **Slice index → level `y`** (first slice = `y = 0`, the bottom).

So for slice `y` (0-based, in natural-sort order) the pixel `(px, pz)` is the
cell `(px, y, pz)`.

### 2.3 Pixel semantics (the exact alpha rule)

For each pixel `(px, pz)` of slice `y`:

- If **alpha == 0**, the cell is **air**. Nothing else on the pixel matters.
- If **alpha != 0** (any value 1..255), the cell is **solid**. Its RGB is looked
  up in `legend.txt`:
  - an exact match yields that entry's material and shape;
  - **no match** ⇒ a diagnostic is printed, the count is reported in
    `MapSourceStats.unknownColorVoxels`, and the cell is treated as **air**
    (the cell is *not* created).

The rule is `px[3] == 0` for air and "any other alpha is solid"
(`src/render/mapsource.c:411-424`). Partial alpha is therefore *opaque solid*:
do not rely on it. Authors should use `a = 0` for air and `a = 255` for solid.

### 2.4 `legend.txt`

`legend.txt` is **required**: a directory with no readable `legend.txt` fails
to load (`src/render/map_loader.c:117`). It is a text file of one directive per
line; blank lines and whitespace-only lines are skipped, and a leading run of
spaces/tabs on a line is ignored. Two line kinds are recognised: **colour
lines** (`#…`) and **light lines** (`$…`). Any other non-blank line is a
diagnostic (`badLines`) and is skipped — a bad `legend.txt` never aborts the
map load.

See section 3 for the grammar.

---

## 3. `legend.txt` grammar

### 3.1 Colour lines

```
#RRGGBB <material> [shape=<shape>] [dir=<dir>]
```

- `#RRGGBB` — **exactly** seven characters: `#` followed by six hex digits.
  Hex is **case-insensitive**; the length must be exactly 6 digits. A token of
  the wrong length or with a non-hex character makes the line a diagnostic
  (skipped), it does not fail the load (`src/render/mapsource.c:63`).
- `<material>` — a material name resolved against the material manifest
  (`assets/textures/materials.txt`, see section 5.1). Names are matched
  **exactly** (case-sensitive). A name that resolves to no material makes the
  line a diagnostic (`unknownMaterials`) and the colour is dropped, so any
  pixel of it is treated as air.
- `shape=` — one of `full` (default), `half`, `ramp`, `half-ramp`.
- `dir=` — one of `north`, `east`, `south`, `west`; meaningful only for `ramp`
  and `half-ramp`.
- Token count: `2..8` (the two required tokens plus up to six attribute tokens),
  `src/render/mapsource.c:302`. More is a diagnostic.

**Matching is by exact RGB.** Only the 24-bit RGB value is compared; alpha is
not part of the key. Two colours that differ only in alpha are the same key
(and one is a duplicate).

**Duplicate colours:** the **last** line wins (`duplicateColors++`, a
diagnostic) — `src/render/mapsource.c:217-226`.

**Overflow:** at most 64 distinct colours; extras are a diagnostic
(`overflowColors`) and ignored.

**Validation of `shape=`/`dir=`** (shared with the ASCII parser,
`voxmapParseShapeAttrs`, `src/render/voxmap.c:231`; pinned by
`test_voxmap.c::test_legend_shape_malformed`):

| Situation | Result |
|---|---|
| `shape=` value not one of the four | **entry skipped** (diagnostic; load continues) |
| `dir=` value not one of the four | **entry skipped** (diagnostic; load continues) |
| a token that is neither `shape=…` nor `dir=…` | **line rejected** (malformed line / diagnostic) |
| `dir=` present on a non-ramp shape | `dir` **ignored** (diagnostic); the shape still applies |
| `ramp`/`half-ramp` with no `dir=` | `dir` defaults to **north** (diagnostic) |
| `shape=` and `dir=` in either order, both after the material | accepted |

When an entry is *skipped*, the colour maps to nothing and any pixel using it
is treated as air. When the *line* is rejected (a junk token), that line is a
diagnostic and skipped; the map still loads (the PNG legend never fails the
load). Note the ASCII parser is stricter (a junk legend token there is a load
failure — see section 8).

Because distinct colours are distinct `(material, shape, dir)` tuples, **two
shapes of the same material need two colours**.

### 3.2 Light lines

```
$ point x y z r g b [radius]
$ spot  x y z r g b dx dy dz angle [radius]
```

The `$` grammar is shared byte-for-byte with the ASCII parser
(`voxmapParseLightLine`, `src/render/voxmap.c:409`; pinned by the light tests in
`test_voxmap.c`).

- `(x, y, z)` is a **world** position. The seeded cell is
  `(floor x, floor y, floor z)`; the centre of cell `(cx, cy, cz)` is
  `(cx+0.5, cy+0.5, cz+0.5)`.
- `r g b` are the light colour channels in `0..255`; values are clamped
  (negative/NaN → 0, > 255 → 255).
- `x y z r g b dx dy dz angle radius` are parsed with `strtof` and must be
  fully numeric and finite.
- `radius` is a **world-unit** range:
  - **point**: optional. When present it caps the seeded channel at
    `min(channel, radius × LIGHT_ATTEN)` where `LIGHT_ATTEN = 16`
    (`src/render/lightgrid.h:65`). When omitted the point defaults to `0`
    (full channel; only global attenuation applies).
  - **spot**: the **cone length**. When omitted it defaults to
    `VOXMAP_LIGHT_DEFAULT_RADIUS = 16.0` (`src/render/voxmap.h:254`).
- `dx dy dz` is the spot axis. It need not be unit length; it is **normalised**
  on load. A zero-length (or non-finite) direction is malformed.
- `angle` is the spot **half-angle in degrees** and must be `> 0`.
- A line is malformed (and is **skipped** with a diagnostic — never a load
  failure) when: the token count is wrong (`point` needs 8 or 9, `spot` needs
  12 or 13), any numeric token is bad, the direction is zero, or the angle is
  non-positive.
- At most 64 lights are kept; extras are skipped (diagnostic,
  `overflowLights`).

Lines may be indented with spaces/tabs. A trailing `\r` (CRLF files) is
stripped.

---

## 4. The tile model

A solid tile has:

- a **material** (resolved from `legend.txt` / the ASCII legend), and
- a **shape** (section 5): `FULL` (the default), `HALF`, `RAMP`, `HALF_RAMP`.
  A ramp/half-ramp additionally carries a **direction** (the side the slope
  rises toward).

Shape is a **rendering/query property only**: a non-air shape is still solid
for occupancy and lighting. The light grid, collision and the "is this cell
air?" test all treat any non-air shape as a full cell (`voxmap.h:173`,
`voxmapSolidAt` is true for every shape).

### 4.1 The six material slots

A material names a texture for each of six cube faces (`src/render/materials.h`):

| `FaceId` | Face | Outward normal |
|---|---|---|
| `FACE_TOP` | top | `+Y` |
| `FACE_BOTTOM` | bottom | `-Y` |
| `FACE_NORTH` | north | `-Z` |
| `FACE_SOUTH` | south | `+Z` |
| `FACE_EAST` | east | `+X` |
| `FACE_WEST` | west | `-X` |

The voxmap's four **side directions** index `(0=+Z, 1=+X, 2=-Z, 3=-X)` and map
to face slots through `materialFaceForSideDir` (`src/render/materials.c:296`):
`0→SOUTH`, `1→EAST`, `2→NORTH`, `3→WEST`.

A material may be declared with a single file (filling all six slots), with
per-face overrides, or with a `side=` key filling all four sides. Every face
left unset inherits the material's primary file. See
`assets/textures/materials.txt` and `MATERIAL_NAME_MAX` = 32
(`src/render/materials.h:41`).

Each face also has an **alpha mode**:

- `opaque` / `blend` — composited through the pipeline's fixed alpha blend
  (opaque textures carry `a=255`, so the blend is identity);
- `cutout` — fragments whose texture alpha is below the shader threshold are
  discarded.

### 4.2 UV convention

Every emitted face is a four-corner quad. The **UV quad** is the material
face slot's atlas rectangle, oriented so the texture reads **upright and
unmirrored from outside the cube** (`materialFaceUV`,
`src/render/materials.c:310`):

- **top** and **bottom** quads list corners `(x,z), (x+1,z), (x+1,z+1), (x,z+1)`
  (top) or the same in x/z with bottom orientation; the UV rect corner order is
  texture top-left, top-right, bottom-right, bottom-left (`top`), vertically
  mirrored for `bottom`.
- **side** quads (north/south/east/west) list corners in the order
  **bottom-left, bottom-right, top-right, top-left** as seen from outside the
  cube; the UV rect samples the texture bottom for the bottom corners and the
  top for the top corners. East/west are horizontally mirrored relative to
  north/south so the texture is not reversed (`materialFaceUV`).

`v = 0` is the **top** row of the source texture (SDL_gpu upload
convention, `src/render/textures.h:13`), so a side's bottom corners sample the
larger `v`.

**Triangles.** A ramp's triangular side is emitted as a **degenerate quad**
`[A, B, C, C]`: corners 0-1-2 are the real triangle and the fourth corner
equals the third. Its UV is the side direction's own four-corner side UV with
`uv[3] = uv[2]` (the degenerate corner mirrors corner 2). The GPU splits every
quad into `(0,1,2)` and `(0,2,3)`; the rendered triangle is `(0,1,2)`, which
samples the **(bottom-left, bottom-right, top-right)** half of that side
texture, and the second triangle is zero-area and draws nothing
(`src/render/voxmap.c:1736`, pinned by
`test_voxmap.c::test_triangle_uv_convention`).

### 4.3 Shading (fixed, not part of the stored model)

Per-channel face brightness multiplied onto the material tint
(`src/render/voxmap.h:281`), RGB only (alpha preserved):

| Face | Factor |
|---|---|
| top | `1.00` |
| `+Z` / south side | `0.90` |
| `+X` / east side | `0.80` |
| `-Z` / north side | `0.70` |
| `-X` / west side | `0.62` |
| bottom | `0.55` |

Odd columns (`(x + z) & 1`) are additionally multiplied by
`VOXMAP_CHECKER_BOOST = 1.06`. The final channel is
`material tint × face shade × checker × light factor × AO`, clamped to `0..255`.
A NULL light grid keeps every face at full brightness (`255`). The **flat**
path (one factor per face, no AO) is the historical path; the **smooth** path
(both default in the app) writes four per-corner tints so the GPU interpolates.

---

## 5. Shape geometry

**Shape pack.** A shape byte packs the shape in bits 0-1 and the direction in
bits 2-3 (`src/render/voxmap.h:356`):

| Shape | Value |
|---|---|
| `FULL` | 0 |
| `HALF` | 1 |
| `RAMP` | 2 |
| `HALF_RAMP` | 3 |

| Direction | Value |
|---|---|
| `NORTH` (`-z`) | 0 |
| `EAST` (`+x`) | 1 |
| `SOUTH` (`+z`) | 2 |
| `WEST` (`-x`) | 3 |

The rest of this section uses cell-local coordinates `(X, Y, Z)` from section 1
and describes faces as a list of four corners.

### 5.1 Solid region

For a tile whose cell floor is at `(x, y, z)` (local origin), a point
`(X, Y, Z)` with `0 ≤ X,Z ≤ 1` is **inside the solid** when:

| Shape / dir | Solid region |
|---|---|
| `FULL` | `0 ≤ Y ≤ 1` |
| `HALF` | `0 ≤ Y ≤ 0.5` |
| `RAMP north` | `0 ≤ Y ≤ 1 - Z` |
| `RAMP south` | `0 ≤ Y ≤ Z` |
| `RAMP west` | `0 ≤ Y ≤ 1 - X` |
| `RAMP east` | `0 ≤ Y ≤ X` |
| `HALF_RAMP north` | `0 ≤ Y ≤ 0.5·(1 - Z)` |
| `HALF_RAMP south` | `0 ≤ Y ≤ 0.5·Z` |
| `HALF_RAMP west` | `0 ≤ Y ≤ 0.5·(1 - X)` |
| `HALF_RAMP east` | `0 ≤ Y ≤ 0.5·X` |

`FULL` is unchanged by the shape feature: every existing map loads with every
voxel `FULL` and emits byte-identically to the pre-shape engine.

### 5.2 The material slot each face samples

| Face kind | Slot | Notes |
|---|---|---|
| top (FULL top / HALF top) | `FACE_TOP` | |
| ramp / half-ramp **slope** | `FACE_TOP` | the slope is the shape's top surface |
| bottom | `FACE_BOTTOM` | |
| vertical side (dir 0..3) | side `0→SOUTH`, `1→EAST`, `2→NORTH`, `3→WEST` | |
| ramp **back face** | the **rise direction's** side slot | `N→NORTH`, `E→EAST`, `S→SOUTH`, `W→WEST` |
| ramp **triangle** | that triangle's own side slot | as given in the tables below |

### 5.3 Face tables

Corner lists are the **world quad corner order** the emitter uses; the fourth
corner of a triangle repeats the third. Coordinates are cell-local `(X,Y,Z)`.

#### 5.3.1 `FULL`

- top: `(0,1,0) (1,1,0) (1,1,1) (0,1,1)` — `FACE_TOP`
- bottom: `(0,0,0) (1,0,0) (1,0,1) (0,0,1)` — `FACE_BOTTOM`
- side `+Z` (south): `(0,0,1) (1,0,1) (1,y1,1) (0,y1,1)`
- side `+X` (east): `(1,0,0) (1,0,1) (1,y1,1) (1,y1,0)`
- side `-Z` (north): `(1,0,0) (0,0,0) (0,y1,0) (1,y1,0)`
- side `-X` (west): `(0,0,1) (0,0,0) (0,y1,0) (0,y1,1)`

where `y1 = 1`. Consecutive FULL voxels stack into **one** side quad spanning
`[runStart, runEnd + 1)` (run merging); a shape breaks a run. Pinned by the
single-section/backward-compat tests, e.g.
`test_voxmap.c::test_heightmap_and_slice_emission_identical`.

#### 5.3.2 `HALF`

- top (at `Y = 0.5`): `(0,0.5,0) (1,0.5,0) (1,0.5,1) (0,0.5,1)` — `FACE_TOP`
- bottom (at `Y = 0`): `(0,0,0) (1,0,0) (1,0,1) (0,0,1)` — `FACE_BOTTOM`
- sides: the four side quads from 5.3.1 with `y1 = 0.5`:
  - `+Z`: `(0,0,1) (1,0,1) (1,0.5,1) (0,0.5,1)`
  - `+X`: `(1,0,0) (1,0,1) (1,0.5,1) (1,0.5,0)`
  - `-Z`: `(1,0,0) (0,0,0) (0,0.5,0) (1,0.5,0)`
  - `-X`: `(0,0,1) (0,0,0) (0,0.5,0) (0,0.5,1)`

Pinned by `test_voxmap.c::test_half_exact_corners`.

#### 5.3.3 `RAMP` (rise `r = 1`)

Each row lists: slope, back face (tall edge, `Y` from `0` to `1`), and the two
triangles `[a b c c]`. The back face is the side quad for the rise direction;
its corners are given in the standard side order (BL, BR, TR, TL).

**dir north** (`kDirToSide` side 2 = `-Z`)
- slope: `(0,1,0) (1,1,0) (1,0,1) (0,0,1)` — `FACE_TOP`
- back (`-Z`): `(1,0,0) (0,0,0) (0,1,0) (1,1,0)` — `FACE_NORTH`
- tri west (`-X`): `(0,0,0) (0,1,0) (0,0,1) (0,0,1)` — `FACE_WEST`
- tri east (`+X`): `(1,0,0) (1,1,0) (1,0,1) (1,0,1)` — `FACE_EAST`

**dir south** (side 0 = `+Z`)
- slope: `(0,0,0) (1,0,0) (1,1,1) (0,1,1)` — `FACE_TOP`
- back (`+Z`): `(0,0,1) (1,0,1) (1,1,1) (0,1,1)` — `FACE_SOUTH`
- tri west (`-X`): `(0,0,1) (0,1,1) (0,0,0) (0,0,0)` — `FACE_WEST`
- tri east (`+X`): `(1,0,1) (1,1,1) (1,0,0) (1,0,0)` — `FACE_EAST`

**dir west** (side 3 = `-X`)
- slope: `(0,1,0) (0,1,1) (1,0,1) (1,0,0)` — `FACE_TOP`
- back (`-X`): `(0,0,1) (0,0,0) (0,1,0) (0,1,1)` — `FACE_WEST`
- tri north (`-Z`): `(0,0,0) (0,1,0) (1,0,0) (1,0,0)` — `FACE_NORTH`
- tri south (`+Z`): `(0,0,1) (0,1,1) (1,0,1) (1,0,1)` — `FACE_SOUTH`

**dir east** (side 1 = `+X`)
- slope: `(1,1,0) (1,1,1) (0,0,1) (0,0,0)` — `FACE_TOP`
- back (`+X`): `(1,0,0) (1,0,1) (1,1,1) (1,1,0)` — `FACE_EAST`
- tri north (`-Z`): `(1,0,0) (1,1,0) (0,0,0) (0,0,0)` — `FACE_NORTH`
- tri south (`+Z`): `(1,0,1) (1,1,1) (0,0,1) (0,0,1)` — `FACE_SOUTH`

Every row also has a **bottom** face `(0,0,0) (1,0,0) (1,0,1) (0,0,1)` (slot
`FACE_BOTTOM`) when exposed (see 5.4). Pinned per direction by
`test_voxmap.c::test_ramp_dir_{north,south,east,west}_exact_corners`.

#### 5.3.4 `HALF_RAMP` (rise `r = 0.5`)

Same structure; the tall edge is `0.5` high and triangles taper to `0` at the
low edge.

**dir north**
- slope: `(0,0.5,0) (1,0.5,0) (1,0,1) (0,0,1)` — `FACE_TOP`
- back (`-Z`): `(1,0,0) (0,0,0) (0,0.5,0) (1,0.5,0)` — `FACE_NORTH`
- tri west (`-X`): `(0,0,0) (0,0.5,0) (0,0,1) (0,0,1)` — `FACE_WEST`
- tri east (`+X`): `(1,0,0) (1,0.5,0) (1,0,1) (1,0,1)` — `FACE_EAST`

**dir south**
- slope: `(0,0,0) (1,0,0) (1,0.5,1) (0,0.5,1)` — `FACE_TOP`
- back (`+Z`): `(0,0,1) (1,0,1) (1,0.5,1) (0,0.5,1)` — `FACE_SOUTH`
- tri west (`-X`): `(0,0,1) (0,0.5,1) (0,0,0) (0,0,0)` — `FACE_WEST`
- tri east (`+X`): `(1,0,1) (1,0.5,1) (1,0,0) (1,0,0)` — `FACE_EAST`

**dir west**
- slope: `(0,0.5,0) (0,0.5,1) (1,0,1) (1,0,0)` — `FACE_TOP`
- back (`-X`): `(0,0,1) (0,0,0) (0,0.5,0) (0,0.5,1)` — `FACE_WEST`
- tri north (`-Z`): `(0,0,0) (0,0.5,0) (1,0,0) (1,0,0)` — `FACE_NORTH`
- tri south (`+Z`): `(0,0,1) (0,0.5,1) (1,0,1) (1,0,1)` — `FACE_SOUTH`

**dir east**
- slope: `(1,0.5,0) (1,0.5,1) (0,0,1) (0,0,0)` — `FACE_TOP`
- back (`+X`): `(1,0,0) (1,0,1) (1,0.5,1) (1,0.5,0)` — `FACE_EAST`
- tri north (`-Z`): `(1,0,0) (1,0.5,0) (0,0,0) (0,0,0)` — `FACE_NORTH`
- tri south (`+Z`): `(1,0,1) (1,0.5,1) (0,0,1) (0,0,1)` — `FACE_SOUTH`

Pinned per direction by
`test_voxmap.c::test_half_ramp_dir_{north,south,east,west}_exact_corners`.

### 5.4 Culling rules

Two independent culls apply.

**(a) Neighbour cull (occlusion).** A face is dropped when the neighbour cell
it faces is a **FULL** tile. "FULL" means solid *and* shape == `FULL`; any
non-air shape counts as *not* FULL.

- **Bottom**: dropped when `y > 0` and the cell below `(x, y-1, z)` is FULL.
  (At `y = 0` the underside is the world floor, so no bottom is emitted.)
- **FULL top**: dropped when the cell above `(x, y+1, z)` is FULL.
- **Sides / back face**: dropped when the neighbour at the same level in that
  direction is FULL. For a merged FULL run, the run stops where the neighbour
  is FULL (or a shape/air).
- **Triangles**: dropped when the neighbour in the triangle's own side
  direction at the tile's level is FULL.
- **HALF top**: **never** culled by the neighbour above. A FULL above occupies
  `[y+1, y+2)`, so it can only meet the `y+0.5` surface at a line; below that
  there is a uniform 0.5 gap that is visible from a low angle.
- **Slope (RAMP / HALF_RAMP)**: **never** culled by the neighbour above — a
  FULL above can only meet the slope at its high edge; the wedge below is open
  (up to `1.0` for a ramp, `0.5` for a half-ramp) and culling it would be a
  see-through. (The only cost is from-above overdraw.)

Shape-vs-shape adjacencies may therefore **overdraw**: faces hidden inside a
neighbouring half/ramp are still emitted, but they are always sorted behind the
surface that occludes them. "Visible faces only" is preserved against true
solids and against air.

> This is the final state after the T17 fix round: the two regression tests
> `test_voxmap.c::test_shape_culling_extra` (a HALF top and a HALF_RAMP slope
> with a FULL above are still emitted) and
> `test_voxmap.c::test_shape_culling_full_neighbour` (a RAMP slope with a FULL
> above is still emitted) were RED before the fix. Only a **FULL** top is ever
> culled by the neighbour above.

**(b) Camera cull.** Each axis-aligned side (including a ramp's back face and
its triangles, which use their own outward normals) is dropped when
`dot(n, toCameraGround) <= CAMERA_CULL_EPS`, where `toCameraGround` is the
unit ground-plane direction from the target toward the camera (`(sin yaw,
cos yaw)`) and `CAMERA_CULL_EPS = 1e-4` (`src/render/voxmap.h:249`,
`src/render/voxmap.c:1396`). An edge-on side has dot exactly `0` and is
culled. At an axis-aligned yaw exactly one side emits; at 45° two sides emit;
between them the set changes continuously.

Pinned by `test_voxmap.c::test_faces_cull_set_at_rest_yaws` (every 45° rest
yaw), `test_faces_edge_on_side_culled`, and `test_faces_cull_mid_tween_yaw`.

### 5.5 Light sampling

With a light grid, each face multiplies in the brightness factor of the **air
cell it looks across** (`lightGridFactorAt`). The sample target by face kind:

| Face kind | Sample cell, for a tile at `(x, y, z)` |
|---|---|
| top (FULL or HALF) | `(x, y+1, z)` — directly above the tile's top |
| ramp / half-ramp slope | `(x, y+1, z)` — the cell above the tile |
| bottom | `(x, y-1, z)` — directly below |
| side / back face | the neighbour air cell in that direction at the face's **bottom level** |
| triangle | the neighbour air cell in the triangle's direction at the tile's level `y` |

On the **flat** path a side uses a single sample at the run's bottom level; on
the **smooth** path a side samples the bottom two corners at the bottom level
and the top two at the top level (a single multi-level span is kept so the
painter sort stays correct), and tops/bottoms average the 2×2 column block at
the face's air level, with per-corner ambient occlusion. A `NULL` light grid
keeps every face at full brightness.

Pinned by `test_voxmap.c::test_shape_light_samples_cell_above`,
`test_shape_light_per_face_type`, and `test_bottom_face_light_sampling`.

### 5.6 Heightmap shapes and the height-0 tile

In the ASCII **heightmap** format a legend shape applies to **every** voxel of
the column (a height-3 `ramp` is three stacked ramps). In the ASCII **slice**
format and in the PNG format it applies per voxel.

A **height-0** heightmap ground tile has **no voxel**, so a shape on a height-0
legend entry has **no effect**: the tile stays a full flat top at `y = 0`
(`test_voxmap.c::test_height_zero_shape_ignored`). The PNG format has no
height-0 concept: a solid pixel is always a real voxel.

---

## 6. Worked example

A 2-slice PNG map, **5 wide × 1 deep**:

```
level 0  (00.png) : stone stone stone stone stone     # a full base row
level 1  (01.png) : R     .     H     .     r          # shapes on top
```

with `legend.txt`:

```
#808080 stone
#A8A8A8 stone shape=ramp dir=north
#C8C8C8 stone shape=half
#B8B8B8 stone shape=half-ramp dir=north
```

Here `R` is a `ramp` north at `(0,1,0)`, `H` a `half` at `(2,1,0)`, `r` a
`half-ramp` north at `(4,1,0)`; every level-0 cell is a FULL stone tile.

**Step 1 — pixels → cells.** Slice 0 (bottom) becomes `y = 0`: `(0..4, 0, 0)`
are FULL stone. Slice 1 becomes `y = 1`: `(0,1,0)` is a ramp-north stone,
`(2,1,0)` a half stone, `(4,1,0)` a half-ramp-north stone; `(1,1,0)` and
`(3,1,0)` are air.

**Step 2 — emitted faces** (all neighbours that matter are air, and the
shapes' bottoms are FULL-culled by the level-0 base):

*Ramp north at `(0,1,0)`:*
- slope `(0,2,0) (1,2,0) (1,1,1) (0,1,1)` — `FACE_TOP`;
- back face `-Z` `(1,1,0) (0,1,0) (0,2,0) (1,2,0)` — `FACE_NORTH`;
- tri west `(0,1,0) (0,2,0) (0,1,1) (0,1,1)` — `FACE_WEST`;
- tri east `(1,1,0) (1,2,0) (1,1,1) (1,1,1)` — `FACE_EAST`;
- bottom: culled (FULL at `(0,0,0)`).

*Half at `(2,1,0)`:*
- top `(2,1.5,0) (3,1.5,0) (3,1.5,1) (2,1.5,1)` — `FACE_TOP`;
- four 0.5-high sides at `(2,1,0)..(3,1.5,1)` — side slots;
- bottom: culled.

*Half-ramp north at `(4,1,0)`:*
- slope `(4,1.5,0) (5,1.5,0) (5,1,1) (4,1,1)` — `FACE_TOP`;
- back face `-Z` `(5,1,0) (4,1,0) (4,1.5,0) (5,1.5,0)` — `FACE_NORTH`;
- tri west `(4,1,0) (4,1.5,0) (4,1,1) (4,1,1)` — `FACE_WEST`;
- tri east `(5,1,0) (5,1.5,0) (5,1,1) (5,1,1)` — `FACE_EAST`;
- bottom: culled.

The FULL base tiles at `(0..4, 0, 0)` emit tops `(x,1,z)..`, no bottoms
(level 0), and a merged `+Z` side run `[0,1)` at `z = 1` (their `-Z`/`±X`
neighbours are out of bounds = air, so those runs exist too but are
camera-culled at most yaws).

**Step 3 — camera cull at yaw 0** (camera toward `+Z`/south). The ramp's back
face (`-Z`, dot = −1) and both triangles (`±X`, dot = 0) are culled; the
slope is never camera-culled. So at yaw 0 the visible new geometry is the
ramp slope, the half's `+Z` side and top, and the half-ramp slope. At yaw 180
the north back faces and slopes show; at yaw 90 the east triangles show; at
yaw 270 the west triangles show. (The committed all-sides screenshots in
section 9 demonstrate exactly this.)

---

## 7. Integration checklist and parse pseudocode

A conforming reader should:

1. **Enumerate** the directory; keep names ending in `.png`.
2. **Natural-sort** them; error if there are `0` or `> 256`.
3. **Read `legend.txt`** (required). Parse colour lines and light lines as in
   section 3; a bad colour line is a diagnostic, not fatal.
4. **Decode** every PNG to RGBA8.
5. **Validate**: all slices the same `width × height`; `width,height ≤ 256`;
   `slices ≤ 256`; `width·height·slices ≤ 1,048,576`. Error otherwise.
6. For every slice `y` and pixel `(px, pz)`:
   - `alpha == 0` ⇒ air;
   - else look up the exact RGB; on a miss, warn and treat as air;
   - else create a solid tile with the entry's material + shape byte.
7. **Reject rules (fatal):** a missing/unreadable directory, `legend.txt`, or a
   slice PNG; a size mismatch; an over-limit dimension or volume; zero slices;
   no `*.png`.
8. **Non-fatal (diagnostic + skip):** a malformed/unknown colour line, an
   unknown shape/dir *value*, a duplicate colour (last wins), an unknown
   material, a malformed light line, colour/light overflow.

```text
read_map(dir):
    names = sorted_natural(glob(dir, "*.png"))
    require 1 <= len(names) <= 256
    legend = parse_legend(read(dir + "/legend.txt"))   # required
    slices = [decode_rgba8(dir + "/" + n) for n in names]
    w, h = slices[0].size
    require 1 <= w <= 256 and 1 <= h <= 256
    require len(slices) <= 256
    require w * h * len(slices) <= 1048576
    require all(s.size == (w, h) for s in slices)

    tiles = {}                     # (x,y,z) -> (material, shape_byte)
    for y, img in enumerate(slices):          # y = 0 is the first/bottom
        for pz in 0 .. h-1:
            for px in 0 .. w-1:
                r,g,b,a = img[px][pz]
                if a == 0: continue           # air
                e = legend.color_lookup(rgb(r,g,b))
                if e is None: warn(unknown colour); continue   # -> air
                tiles[(px, y, pz)] = (e.material, e.shape_byte)

parse_legend(text):
    for line in text:
        line = strip_leading_spaces_tabs(strip_trailing_cr(line))
        if line == "": continue
        if line[0] == "$": parse_light(line); continue   # malformed -> warn
        if line[0] == "#":
            toks = split(line)
            if not (2 <= len(toks) <= 8): warn; continue
            rgb = parse_hex_7(toks[0])          # exactly "#RRGGBB", any case
            if rgb is None: warn; continue
            mat = resolve(toks[1])              # unknown -> warn, drop line
            if mat is None: continue
            shape, dir, ok = parse_attrs(toks[2:])   # see section 3.1 table
            if not ok: warn; continue           # junk token OR unknown value
            legend.put(rgb, mat, pack(shape, dir))   # duplicate -> last wins
        else:
            warn(line); continue

parse_attrs(toks):
    shape, dir, dir_seen = FULL, NORTH, false
    for t in toks:
        if t starts "shape=":
            v = {"full":FULL,"half":HALF,"ramp":RAMP,"half-ramp":HALF_RAMP}.get(t[6:])
            if v is None: return skip           # unknown value -> skip entry
            shape = v
        elif t starts "dir=":
            v = {"north":0,"east":1,"south":2,"west":3}.get(t[4:])
            if v is None: return skip
            dir, dir_seen = v, true
        else:
            return bad                          # not an attribute -> reject line
    if dir_seen and shape not in (RAMP, HALF_RAMP): dir = NORTH   # warn
    if not dir_seen and shape in (RAMP, HALF_RAMP): dir = NORTH   # warn
    return ok
```

---

## 8. Relation to the ASCII format

The ASCII format (documented at the top of `src/render/voxmap.h` and in
`docs/map-authoring.md`) encodes the **same tile model**:

- A **single-section** file (no `---` separator) is a **heightmap**: one row per
  line, rows top-to-bottom (`z = 0` first); `'0'..'9'` are column heights, `'.'`
  is void, and a legend char carries its own height. A height-`h` column is
  solid at `y = 0..h-1`.
- A file containing `---` separator lines is a **slice** file: each block between
  separators is one horizontal slice, ascending (`y = 0` first) — the same
  stacking as the PNG directory.
- **Legend:** `@ <char> <height> <material> [shape=…] [dir=…]` (the `@` may be
  attached: `@g 2 grass`). The shape/dir attributes are the same tokens and the
  same validation as section 3.1, **except** the ASCII parser is stricter: a
  token that is neither attribute is a **load failure** (the line is rejected),
  and the older "extra junk tokens are ignored" behaviour is gone since T17.
- **Lights:** `$ …` lines are the identical grammar (section 3.2); the two
  parsers share one implementation (`voxmapParseLightLine`).

The two encodings are held equivalent by `tests/test_pngmap.c`, which loads
`assets/maps/demo/` and asserts it is identical to `assets/maps/demo.txt`
(occupancy, material, shape, dir and lights).

---

## 9. Templates, screenshots and further reading

- **Template texturemaps** — per-shape UV-mapping sheets and blank
  paint-over templates: [`../assets/templates/README.md`](../assets/templates/README.md).
- **All-sides screenshots** — in-engine captures of each shape from yaws
  0/90/180/270: [`images/tiles/`](images/tiles/).
- **Authoring guide** (workflow, ImageMagick helper, limits):
  [`map-authoring.md`](map-authoring.md).
- **Implementation of record:** `src/render/voxmap.h` (tile model, shapes,
  culling, lighting) and `src/render/mapsource.h` (PNG directory).
