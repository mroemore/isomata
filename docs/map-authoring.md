# Authoring maps

Isomata loads a voxel map from either of two equivalent sources:

- **ASCII** — a single text file of horizontal slice sections (the original
  format, e.g. `assets/maps/demo.txt`). Fast to hand-edit; the parser contract
  lives at the top of `src/render/voxmap.h`.
- **PNG slices** — a directory of numbered PNGs plus a colour `legend.txt`
  (the authoring pipeline: draw in a pixel-art program, export layers, drop
  them in). This document covers the PNG format.

Both produce exactly the same in-memory `Voxmap` (occupancy, per-voxel
material, lights), so everything downstream — face emission, lighting — is
format-agnostic. The two formats are held equal by the equivalence test
`tests/test_pngmap.c`, which loads `assets/maps/demo/` and asserts it is
identical to `assets/maps/demo.txt`.

## The PNG slice directory

A map directory, e.g. `assets/maps/<name>/`, contains:

```
assets/maps/demo/
  00.png      one PNG per horizontal slice, numbered
  01.png
  ...
  08.png
  legend.txt  the colour legend + optional lights
```

- **`NN.png`** — one horizontal slice per PNG. The file names are ordered by
  **natural sort** (`9.png` sorts before `10.png`), and ascending order is
  **bottom first**: `00.png` is level `y = 0` (the bottom), the next file is
  `y = 1`, and so on. Every PNG must be the **same width x height**; one pixel
  is one voxel:
  - **transparent** (alpha `0`) = **air**;
  - **any other alpha** = **solid**, with its RGB looked up in the legend.
    Use fully opaque colours when drawing; a partial alpha is treated as
    solid, so do not rely on it.
  - PNG column `x` and row `z` map to voxel `(x, y, z)`; the **top row of the
    image is `z = 0`** (the same row order the ASCII slices use).
- **No manifest.** The slice order is the natural sort of the file names; a
  manifest (an explicit file list) is a documented future extension if a
  naming convention is ever not enough.
- **`legend.txt`** — the colour legend and optional lights:
  - `#RRGGBB <material> [shape=full|half|ramp|half-ramp] [dir=north|south|
    east|west]` maps an **exact** RGB colour to a material name from
    `assets/textures/materials.txt` and an optional per-voxel shape (see
    "Sub-voxel shapes"). The hex is case-insensitive. A duplicate colour is a
    diagnostic and the **last** line wins. An opaque pixel whose colour is
    **not** in the legend is a diagnostic, is counted, and is treated as
    **air**. A malformed hex (`#GGGGGG`, wrong length), an unknown material
    name, or an unknown shape/dir value is a diagnostic and the line is
    skipped — a bad legend never fails the load.
  - `$ point ...` / `$ spot ...` — lights, with **exactly** the same grammar
    and semantics as the ASCII format (see `voxmap.h`): a point seeds its cell
    with an RGB colour and optional radius; a spot adds a direction and a
    cone half-angle. A malformed light line is skipped with a diagnostic.

### Colour legend example

```
#00FF00 grass
#808080 stone
#A0522D wood
#0000FF sixface
#FF00FF foliage
$ point 5.5 2.5 10.5 255 220 160 6
$ spot 12 6 12 255 240 200 0 -1 0 40 6
```

### Sub-voxel shapes

Every voxel has a **shape**: `full` (the default), `half` (a bottom slab),
`ramp` or `half-ramp` (a wedge). A ramp/half-ramp also takes a **`dir`** — the
side the slope rises toward (`north` = `-z`, the image's top row; `south` =
`+z`, `east` = `+x`, `west` = `-x`). The attributes are optional and the same
in both formats:

- **PNG** `legend.txt`: `#RRGGBB <material> shape=ramp dir=north`. Distinct
  colours are distinct `(material, shape, dir)` tuples, so two shapes of the
  same material need two colours.
- **ASCII**: `@ <char> <height> <material> shape=ramp dir=north`. In a
  heightmap the shape applies to **every** voxel of the column; in slice mode
  it applies per voxel.

An absent `shape` is `full`. A ramp without `dir` defaults to `north`; a `dir`
on a non-ramp is ignored; both print a diagnostic. An unknown shape/dir value
skips that legend entry. Geometry, culling and light semantics are documented
at the top of `src/render/voxmap.h` and specified rigorously (coordinate and
face tables, UV convention, culling and lighting rules, a worked example and
parse pseudocode) for external tools in
[`tile-format-spec.md`](tile-format-spec.md). Artist texturemap templates for
the shapes are in [`../assets/templates/`](../assets/templates/README.md), and
all-sides in-engine captures in [`images/tiles/`](images/tiles/).

## Workflow

1. Draw the map in a pixel-art program (Aseprite, GIMP, …) with one **layer
   per horizontal slice**, sized `width x height` (e.g. 16x16).
2. Give each colour a material in `legend.txt` (`#RRGGBB material`). Leave
   pixels transparent for air.
3. Export the layers as numbered PNGs (`00.png`, `01.png`, …) into
   `assets/maps/<name>/`, bottom slice first, and add `legend.txt` (with any
   `$` lights).
4. Run the game. The level scene **prefers** the PNG directory
   (`maps/demo`) and falls back to the ASCII file (`maps/demo.txt`) when the
   directory cannot be read. The log line states which source loaded.

The desktop build installs the whole `assets/` tree; the Android build packs
it into the APK (see `assets/README.md`).

## Lights

The `$` lines are shared verbatim with the ASCII parser, so the syntax is the
one documented in `src/render/voxmap.h`:

```
$ point x y z r g b [radius]
$ spot  x y z r g b dx dy dz angle [radius]
```

`(x, y, z)` is a **world** position; the seeded cell is `(floor(x), floor(y),
floor(z))`, so the centre of cell `(cx, cy, cz)` is `(cx+0.5, cy+0.5,
cz+0.5)`. `r g b` are `0..255`. `radius` is a world-unit range (a point's
optional cap; a spot's cone length). A spot's direction need not be unit
length and is normalised; a zero-length direction is malformed.

## The ASCII format

`assets/maps/demo.txt` is the source of truth for the demo and the fixture the
PNG version is compared against. Its format (single-section heightmaps and
multi-section `---` slices, `@ char height material` legends, `$` lights) is
documented at the top of `src/render/voxmap.h`. The two formats agree on
occupancy, materials and lights; use whichever is convenient.

## Regenerating the demo PNGs

The committed demo slices are generated from the ASCII map so the two can
never drift:

```
./scripts/gen-map-png.sh
```

It splits `assets/maps/demo.txt` on its `---` sections, maps each character to
a legend colour, writes one committed PNG per section and `legend.txt`
(colours + the same `$` lights). It needs ImageMagick 7 (`magick`). Re-run it
after editing `demo.txt` or the colour map in the script.

The sub-voxel shape specimen map used for the T18 all-sides screenshots is
generated the same way from `assets/maps/tiles.txt`:

```
./scripts/gen-tiles-map.sh
```

It writes `assets/maps/tiles/` and uses the face-labelled `tile` material
(`assets/textures/tile_*.png`, generated by `./scripts/gen-tile-textures.sh`);
see [`images/tiles/`](images/tiles/) and
[`tile-format-spec.md`](tile-format-spec.md).

## Limits

- Each dimension (width, height, slice count) is at most `VOXMAP_MAX_DIM`
  (256), and `width * height * slices` is at most `VOXMAP_MAX_CELLS`
  (`1 << 20`); an over-large map fails cleanly with a diagnostic.
- At most `MAPSOURCE_MAX_COLORS` (64) legend colours and `VOXMAP_MAX_LIGHTS`
  (64) lights are kept; extras are a diagnostic.
