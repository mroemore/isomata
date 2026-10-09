# Isomata Phase 2 — Textures, Lighting, Entities, Particles

**Status: draft for review** (2026-10-09). The build order is agreed with the
user; user notes are captured below. The texture item is detailed; lighting,
entities and particles get detailed task plans as their notes land.

## Agreed order

1. **Textures** — small, unblocks the visual identity of everything after.
2. **Lighting** — the most invasive render change; land it before entities and
   particles add more render consumers, so they are built on the lit pipeline.
3. **Entities** — the gameplay core, mostly pure logic; inherits textures and
   lighting for free.
4. **Particles** — polish on the settled blend/render pipeline.

(Alternative: swap 2 and 3 if gameplay should come first; particles stay last
either way.)

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

### Design decisions (proposed — veto any by number)

- **D1 — N→6 expansion (per set)**: 1 = all six faces; 2 = top + sides (bottom
  uses the sides texture); 3 = top + sides + bottom; 6 = per-face. Same rules
  for the inside set. A tile that omits the inside set defaults to the outside
  set (so "12 squares" is the maximum, not the common case).
- **D2 — Inside vs outside selection**: classify air cells as **exterior**
  (connected to open sky) or **interior** (enclosed) with a flood fill; a face
  uses the outside set when its exposed side is exterior air, the inside set
  when interior. Pure and testable; the same pass later seeds lighting's sky
  term.
- **D3 — Map format**: cells stay one char; the map file gains an optional
  legend mapping each char → height + material (e.g. `g = height 2, grass`);
  digits default to a built-in legend (digit = height, default material) so
  existing maps keep working. *Alternative if preferred: two-char cells
  (`2g`).*
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
2. Material model: 12-slot sets + the D1 expansion (pure, tests).
3. Map legend + per-cell material (pure parser, tests; fixtures updated).
4. Interior/exterior flood fill (pure, tests).
5. Emission: material-aware UVs + inside/outside selection (pure, tests).
6. Sprite texture ids through the same atlas.
7. UV-convention flip fix (pin with a test).
8. Alpha modes (opaque/blend wired; cutout shader path).
9. Demo content: a few materials (grass/stone/wood) + one alpha tile + a
   hollow that shows interior faces.
10. Verification: pure suite + valgrind + floors; Xvfb screenshots (materials,
    alpha tile, interior faces); emulator phone config; APK publish.

---

## Item 2 — Lighting (notes pending)

Scope sketch (to be detailed): per-voxel RGB light grid + flood-fill
propagation (sky + block light, per-channel attenuation); emitters — spherical
(radial falloff) and conical (direction, half-angle, falloff) — mixing colours
per channel with saturation; per-face/corner sampling into `vColor` on top of
ambient + the existing directional shading; AO from neighbour occupancy; a
debug light view; incremental updates for moving emitters. "GI" is scoped to
the voxel flood fill that bends around corners — ray-traced GI / radiosity /
lightmaps are out of scope.

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
