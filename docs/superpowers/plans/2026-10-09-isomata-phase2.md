# Isomata Phase 2 — Textures, Lighting, Entities, Particles

**Status: in progress** (2026-10-09). The build order is agreed with the
user; user notes are captured below. Item 1 (textures) is **done** (T12,
reviewed, published). Item 2 (lighting) is detailed and proposed below. Task
T13 (PNG layer map pipeline) is deferred until after lighting (user).
Entities and particles get detailed task plans as their notes land.

## Agreed order

1. **Textures** — done (T12).
2. **Lighting** — detailed below (T14/T15); the most invasive render change,
   landed before entities and particles add more render consumers.
3. **Deferred: T13 (PNG layer map pipeline)** — immediately after lighting,
   per the user.
4. **Entities** — the gameplay core, mostly pure logic; inherits textures and
   lighting for free.
5. **Particles** — polish on the settled blend/render pipeline.

---

## Item 1 — Textures

### User requirements (captured 2026-10-09)

- Support the **alpha channel**.
- Work for voxels/tiles "in a somewhat optimal manner": **tiles should only
  draw visible faces**.
- Per tile: **1, 2, 3 or 6 square textures for the outside**, and the same for
  the **inside** — **12 squares total** potentially. Most tiles are 1 square
  repeated 6 times.

### Verified current state

- **Alpha already composites end to end**: the single world pipeline blends
  `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, and `world.frag` outputs
  `texture(uTexture, vUV) * vColor` (alpha included). Imported textures only
  need RGBA preserved through the atlas.
- **Visible faces only, already**: face emission culls at the source — a side
  face is emitted only when the neighbour column is lower, tops only on the top
  of a stack. There are no hidden faces in the draw list to optimise away.
- **One texture bind per frame**: the world and grid share one atlas + one
  pipeline; textured tiles keep that (per-face UVs into a runtime atlas).

### Design decisions (agreed 2026-10-09)

- **D1 — 6 per-face slots, outside only** (user simplification): each material
  defines textures for the six faces (top, bottom, north, south, east, west).
  Fill rule: a material specifies at least one texture — **one file fills all
  six** (the common case); individual faces can be overridden by name. No
  1/2/3 shorthand, no inside set for now (12 → 6).
- **D2 — inside set deferred**: interior/exterior air classification is not
  needed without the inside set; the same flood fill returns later as
  lighting's sky term.
- **D3 — Map format**: cells stay one char; the map gains an optional legend
  mapping each char → height + material (e.g. `g = height 2, grass`); digits
  default to a built-in legend so existing maps keep working. Plus the PNG
  layer pipeline (T13, below).
- **D4 — Textures and import**: square power-of-two sources (16/32/64), packed
  at load into **one runtime atlas** (RGBA); per-face UVs index the atlas. A
  manifest maps material ids → files (e.g. `grass top=… side=… …`). Loading via
  `SDL_LoadPNG` + the existing asset-path machinery — no new dependency, the
  Android path is already proven.
- **D5 — Alpha modes per material**: `opaque` / `blend` / `cutout` (cutout =
  shader discard below a threshold, for foliage-style tiles). Blend works
  already; cutout is a small `world.frag` change recompiled through
  `scripts/compile-shaders.sh`.
- **D6 — Sprites**: sprite definitions gain a texture id resolved through the
  same manifest/atlas. This task also fixes the ledgered UV-convention flip
  (invisible with the solid placeholder, wrong with real art).

### Task T12 — textured tiles and sprites (one task, implementer + review)

1. Atlas packer: pure packing math (slot layout, UV rects) + glue load
   (`SDL_LoadPNG` → GPU texture), tests for the packer.
2. Material model: 6 per-face slots + the one-file fill rule (pure, tests).
3. Map legend + per-cell material (pure parser, tests; fixtures updated).
4. Emission: material-aware UVs (pure, tests).
5. Sprite texture ids through the same atlas.
6. UV-convention flip fix (pin with a test).
7. Alpha modes (opaque/blend wired; cutout shader path).
8. Demo content: a few materials (grass/stone/wood) + one alpha tile.
9. Verification: pure suite + valgrind + floors; Xvfb screenshots (materials,
   alpha tile); emulator phone config; APK publish.

---

## Item 2 — Lighting

### User requirements (captured)

Global illumination, ambient light, spherical and conical light projections,
coloured light mixture from multiple sources.

### Design (proposed 2026-10-09 — veto any numbered point)

- **L1 — Model**: a per-voxel **RGB light grid** (3 × uint8 per cell) over the
  map volume, propagated by **BFS flood fill** with per-channel max-merge and a
  tunable per-step attenuation (default −16/255 per step → ~16-cell range).
  This is the genre-standard voxel light engine: it bends around corners (the
  "GI" feel), mixes colours stably (red + green overlap → yellow, per-channel
  max), and is pure and testable. Sky seeds white light from the topmost air
  cells (full strength downward until blocked, then attenuated spread);
  ambient is a constant floor added at sampling. *Alternative if preferred:
  direct per-emitter evaluation with line-of-sight rays and inverse-square
  falloff (smoother, pricier, no corner-bending) — BFS is the recommendation.*
  True ray-traced GI / radiosity / lightmaps stay out of scope.
- **L2 — Emitters**: `$ point x y z r g b [radius]` and
  `$ spot x y z r g b dir=x,y,z angle=deg [radius]` lines in the map file.
  Points seed their cell; spots seed the cone volume (line-of-sight checked,
  angle falloff) and BFS softens the edges. Static in v1 (computed at load);
  the API supports re-seeding + incremental BFS so moving/pulsing lights land
  as a follow-up.
- **L3 — Sampling**: each emitted face reads the light of the air cell it
  faces, with per-corner smoothing (average of the adjacent air cells) — the
  classic voxel smooth-light look; multiplied by the existing directional
  shading and material tint; per-corner AO (neighbour occupancy) applied here
  too. The result bakes into the vertex colour — **no shader change**.
- **L4 — Debug view**: a key toggles a light-only render (faces show their
  light colour, no texture) for tuning.
- **L5 — Tasks**: T14 = light grid + propagation (sky + block) + emitters
  (point/spot) + pure golden tests; T15 = sampling + AO + lights parsing +
  demo lights + debug view + Xvfb/emulator evidence.

---

## Deferred — Task T13: PNG layer map pipeline (after lighting — user, 2026-10-09)

Authoring model requested by the user: draw the map in a pixel-art program
(Aseprite/GIMP), **export layers as PNGs, numbered**, and a **legend converts
colours → tile types**.

Design:

- A map directory (or a small map manifest listing the layers in order)
  supplies numbered layer PNGs (`01.png`, `02.png`, …). Order: the manifest
  when present; otherwise natural-sort auto-discovery of `*.png` in the
  directory.
- Layers **composite bottom-up** (later layers override where opaque; fully
  transparent pixels leave the layer below; alpha = no cell in the base layer).
- Each final pixel's RGB is looked up in a **colour legend** → tile type
  (height + material) — the same table as the char legend, keyed by colour
  (`#RRGGBB = height + material`).
- Output: the same in-memory Voxmap the ASCII parser produces — everything
  downstream (emission, lighting) is format-agnostic.
- Pure parts: compositing rules, natural sort, colour→tile lookup (tests);
  glue: PNG load via `SDL_LoadPNG` + asset path.
- Demo: author one small map as PNG layers (generated with ImageMagick for the
  test fixture), screenshot-verified in-app.

---

## Item 3 — Entities

### User notes (captured 2026-10-09)

- AI movement is **tile based on the backend**.
- At least the **option to animate walk paths**; start with **easing
  interpolation functions between tile movements**.

### Sketch

Entity registry + tile-stepped movement (walkability: height difference/void);
a movement animation layer with a pure, tested **easing function library**
(linear, ease-in-out quad/cubic, smoothstep, …) and a path follower (waypoint
queue) with a per-entity *animated* flag — snap or ease between tiles; a
table-driven FSM (state × event → next + action); A* on the tile grid + path
following; demo integration (wanderers/patrols) with events.

## Item 4 — Particles (notes pending)

Sketch: fixed-capacity pool (pure SoA), emitter definitions (rate/burst,
lifetime, velocity cone, gravity/drag, size/colour/alpha over life,
alpha/additive blend), billboard render after the world, hooks into events and
entities.
