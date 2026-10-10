# Isomata texture generation pipeline (`texgen`) — design

**Status:** approved 2026-10-10. The user chose the script-pipeline shape first
(approach B in review) and will move toward a native bench (A) or a
ComfyUI-native flow (C) afterwards; this design is built so that move is not a
rewrite.

- **Scope:** host-side authoring tooling for `assets/textures/` in this repo.
- **Related:** `docs/superpowers/plans/2026-10-09-isomata-phase2.md` (Item 1,
  T12 — material/alpha/atlas decisions), `docs/tile-format-spec.md` (§4.1 six
  material slots, §5 shape geometry),
  `assets/templates/README.md` (artist templates), `scripts/gen-map-png.sh`
  (script conventions).

---

## 1. Purpose

Turn a text prompt into an isomata-ready face texture through single-key
approvals:

```
new → gen → pick → inpaint (optional) → crop → pixelate → palette → assign → export (optional)
```

The reviewer (the user) answers with one key per stage (`Enter` accept, `r`
redo, `q` quit; `1`–`4` to pick a candidate) while looking at a picture, never
by editing masks by hand or driving a graph editor.

---

## 2. Consumer contract (verified in-repo, 2026-10-10)

- Face textures are square PNGs, 16 (stock) / 32 / 64 px
  (`docs/superpowers/plans/2026-10-09-isomata-phase2.md` D4).
- A material names textures for the six cube faces `top bottom north south east
  west`; one file may fill all six, individual faces may be overridden with
  `key=file`, and `side=` fills all four vertical sides
  (`assets/textures/materials.txt`, `src/render/materials.h`).
- Every material carries an alpha mode: `opaque | blend | cutout`.
- Textures are committed artifacts; the desktop build installs `assets/` next to
  the binary and the Android build packs the tree into the APK.
- **Merged vertical runs sample the face texture once across the run**, i.e. the
  texture stretches over a stacked run (`docs/tile-format-spec.md` §5.3.1 run
  merging, `src/render/voxmap.c` emission). Consequences for generation:
  no seamless-tiling requirement, and textures with vertically forgiving
  content read best.
- The engine has no palette concept. Palette is a **host-side authoring
  contract only**: textures arrive already quantised.

---

## 3. Architecture — the file-based stage contract (D1)

Every state transition is a file inside a run directory. No database, no
daemon-side state. This is the core decision: it is what lets a future bench (A)
or ComfyUI-native flow (C) reuse everything except the terminal loop.

**Run directory** — `.tmp_files/texgen/<slug>/` (`.tmp_files/` is gitignored;
this matches `scripts/gen-map-png.sh`, which already uses
`.tmp_files/gen-map-png`):

```
.tmp_files/texgen/<slug>/
  run.json                 # the record (schema in §9)
  00-prompt.txt            # effective prompt after the template wrapper
  10-candidates/c1..c4.png # N candidates from gen (default 4)
  10-candidates/sheet.png  # 2x2 montage for one-glance picking
  11-chosen.png            # pick
  20-inpainted.png         # inpaint (optional)
  20-mask.png              # inpaint (optional, mask mode)
  30-cropped.png           # crop
  40-pixelated.png         # pixelate
  40-preview-x16.png       # nearest-neighbour x16 preview (pixelate)
  50-final.png             # palette (the assignable artifact)
  50-preview-x16.png       # nearest-neighbour x16 preview (palette)
```

**Invariants (D2)**

- Stages are CLI subcommands: `new`, `gen`, `pick`, `inpaint`, `crop`,
  `pixelate`, `palette`, `assign`, `export`, plus service helpers
  (`service start|stop|status`) and a maintenance command (`requantise`).
- A stage is a function of (run directory, its own flags). Re-running a stage
  overwrites its output file and **invalidates downstream stage outputs**, which
  `run.json` records as stale.
- Stages are resumable: the run directory is the only state, so `texgen pixelate
  <slug>` works minutes or weeks later.
- **Deterministic given inputs**: same seed + same palette + same ops ⇒
  byte-identical `50-final.png`. This is the reproducibility contract.
- Nothing outside the run directory, the tool directory, and the repo's
  `assets/` is written. The tool never commits.

---

## 4. Backend — local ComfyUI (D4)

- ComfyUI lives outside the repo at `~/proj/comfyui/` (repo clone + its own uv
  venv with Python 3.13; installed via `bootstrap.sh`, idempotent). This
  mirrors the existing local-service pattern (e.g. `~/proj/ocrq`) and keeps
  multi-GB weights and Python packages out of the isomata tree.
- `texgen service start|stop|status` manages it. v1 has **no boot service**;
  `gen` auto-starts it on demand and waits for `GET /system_stats`.
- API surface used: `POST /prompt` (workflow JSON with injected params),
  `GET /history/{id}` polling (no websocket client — v1 polls every 2 s),
  `GET /view` to fetch result images. urllib only; no `requests`, no `websocket`.
- **Models** (Phase 0 brings them up; klein is the default):
  - `FLUX.2 klein 4B` — Apache-2.0 weights, the default generator and editor.
  - `Z-Image-Turbo` — Apache-2.0, fast alternative.
  - Licensing rule for the project: prefer Apache-2.0 weights; the pipeline must
    not hard-depend on any model whose licence restricts use.
- VRAM: 8 GB (RTX 3070 Laptop). klein 4B runs in its quantised form; ComfyUI
  offloads the text encoder. `service start --lowvram` is available if OOM is
  observed; default is stock start.
- **Workflows are versioned JSON** in `tools/texgen/workflows/`, exported in
  ComfyUI API format:
  - `txt2img.json` — prompt, seed, size, steps, model selection.
  - `inpaint.json` — image + mask in, image out.
  - `edit.json` — prompt-directed edit (klein reference editing), no mask.
  The client injects parameters by node id; the workflow files stay readable
  and diffable in git, and a future C or ComfyUI-native driver reuses them
  verbatim.

---

## 5. Stage semantics (D5)

| Stage | Inputs (defaults) | Output | Notes |
|---|---|---|---|
| `new "<prompt>"` | prompt text; `--slug`, `--model` | `run.json`, `00-prompt.txt` | wraps the prompt in a texture-biased template (flat orthographic material study, limited palette, no perspective); `--template` overrides |
| `gen` | `--n 4`, `--size 1024`, `--seed`, `--steps`, `--model` | `10-candidates/*`, `sheet.png` | one ComfyUI `txt2img` job per candidate; same seed family, distinct seeds |
| `pick <1..4>` | candidate index | `11-chosen.png` | also accepts `--index` for non-interactive use |
| `inpaint` | `--mask` or `--edit "<text>"` | `20-inpainted.png` | mask mode: writes `20-mask.png`, waits for the artist to paint (black = keep), then runs `inpaint.json`; edit mode runs `edit.json` |
| `crop` | `--mode center` or `--rect x,y,w,h` | `30-cropped.png` | v1 is center-square by default; explicit rect for deliberate framing |
| `pixelate` | `--size 16`, `--filter box\|nearest` | `40-pixelated.png`, `40-preview-x16.png` | box-average downscale is the default because nearest drops detail at 1024→16 |
| `palette` | palette file (default `tools/texgen/palette.json`), `--dither none\|bayer4` | `50-final.png`, `50-preview-x16.png` | nearest-colour quantisation on RGB only (§7) |
| `assign` | `--material NAME`, `--faces all\|top,bottom,…\|side`, `--alpha opaque\|blend\|cutout`, `--file NAME.png` | `assets/textures/NAME.png` (+ per-face files), `materials.txt` line, provenance sidecar | grammar-validated upsert, atomic write (§8) |
| `export` | `--to PATH` | copy of `50-final.png` | for use outside isomata |
| `requantise` | `--material NAME…` or `--all` | rewritten committed textures | re-runs the palette stage over already-committed textures (§7) |
| `service` | `start\|stop\|status`, `--lowvram` | — | ComfyUI lifecycle |

Stage-independent flags: `--run <slug>` (default: newest run), `--yes`
(non-interactive: accept every stage without the review gate, for scripted use
and CI), `--dry-run` on `assign` (print the diff, write nothing).

---

## 6. Review UX (D6)

- After each stage: print a one-line summary of what changed, open the result
  with `feh --scale-down` (blocks), then a single-key prompt.
  `Enter` accept, `r` redo (same stage, fresh randomness where applicable),
  `q` quit (everything stays resumable).
- Candidate picking shows the 2x2 montage (`sheet.png`), keys `1`–`4`.
- Pixel-sized outputs (`16`–`64`) are previewed through a nearest-neighbour
  x16 upscale file so feh shows crisp pixels rather than a blurry scale.
- `feh` is the viewer on this workstation today; the command is configurable
  (`viewer` in `config.json`) so a future bench or a different host can swap it.
- `--yes` runs the whole pipeline unattended — that is how the automated
  verification in the plan reproduces a texture.

---

## 7. Palette (D7)

- Format: the **dotty palette JSON** (`~/proj/dotty/palettes/*.json`, the
  loader is `~/proj/dotty/src/palette.h`):

  ```json
  { "name": "sweetie-16", "author": "…", "colors": [[r, g, b, a], …] }
  ```
- isomata has **one fixed global palette**. The chosen palette is committed at
  `tools/texgen/palette.json` (same schema, so it can be copied verbatim out of
  the dotty palette collection).
- `palette` stage: nearest colour in RGB; the **source alpha channel is
  preserved** (cutout materials keep their authored alpha); palette alpha
  values are ignored (all are 255 in the collection).
- Dither: `none` (default — pixel art stays crisp) or `bayer4` (4x4 ordered).
- Swapping the palette = replacing `tools/texgen/palette.json` and re-running
  the stage; `requantise` applies the current palette to already-committed
  textures so the whole set stays coherent.
- Choosing *which* palette is a deliberate step in Phase 1 (the default is
  `sweetie-16`, a 16-colour palette in the collection; any of the ~68 files
  can be dropped in).

---

## 8. Assign and the material manifest (D8)

- Naming: `--faces all` writes `assets/textures/<material>.png` and emits
  `material <name> <file> [alpha=…]`; `--faces top,bottom,…` writes
  `<material>_<face>.png` per face and emits
  `material <name> face top=… bottom=… [alpha=…]`.
- The manifest upsert:
  - if a `material <name>` line exists, it is **replaced**, not appended;
  - otherwise the line is appended at the end of the file;
  - the new line is validated against the grammar in
    `assets/textures/materials.txt` and `src/render/materials.h` before any
    write; an invalid line aborts the stage;
  - writes are atomic (temp file + rename), so a crash cannot leave a
    half-written manifest.
- `assign` prints the exact `git add` command for review; it **never** commits.
- Provenance sidecar: `assets/textures/provenance/<material>.json` (§9).

---

## 9. Records — `run.json` and the provenance sidecar (D9)

`run.json` (scratch, gitignored) tracks: schema version, slug, created date,
model, effective prompt, template, per-candidate seeds, sampler params, stage
outputs with sha256, and staleness markers for invalidated downstream stages.

`assets/textures/provenance/<material>.json` (committed) records the
reproducibility minimum: prompt, template, model + weights filename, seed,
sampler params, palette file + sha256, crop rect, pixelate size/filter,
dither, alpha mode, per-face file names, creation date, and the source run
slug. This mirrors how `assets/README.md` already documents the provenance of
the font and audio assets.

---

## 10. Tool layout, dependencies, configuration (D10)

```
tools/texgen/
  README.md
  config.json          # defaults: comfy url, model, sizes, palette path, viewer
  texgen               # executable entry point (python shebang, argparse);
                       # the package is a directory named texgen/, so the entry
                       # point must NOT be texgen.py (import collision)
  texgen/              # package
    __init__.py
    run.py             # run directory, run.json, staleness
    comfy.py           # ComfyUI client + service lifecycle
    stages.py          # gen/inpaint/crop/pixelate/palette/assign/export
    palette.py         # dotty JSON palette reader
    imops.py           # Pillow ops: crop, resize, quantise, preview, montage
    manifest.py        # materials.txt parse/validate/upsert
    prov.py            # provenance sidecar writer
    review.py          # feh gate + single-key prompt loop
  workflows/
    txt2img.json
    inpaint.json
    edit.json
  tests/               # stdlib unittest, pure stages only
```

- Runtime dependencies: **Python 3 + Pillow only** (both present on the
  workstation; Pillow 12.2.0). Everything else is stdlib (`urllib`, `json`,
  `argparse`, `hashlib`). No `requests`, no numpy.
- ComfyUI's own environment (torch etc.) is its uv venv, never the system
  Python.
- `config.json` holds the defaults so flags stay optional; flags always win.

---

## 11. Testing (D11)

- `tools/texgen/tests/` with stdlib `unittest`; run with
  `python3 -m unittest discover tools/texgen/tests`.
- Pure units under test: palette JSON parsing, crop geometry, box/nearest
  resize, quantisation (including alpha preservation and bayer dither),
  `materials.txt` parse/validate/upsert (including the "no corruption on
  invalid input" case), `run.json` round-trip and staleness, provenance
  writing, and the ComfyUI client against a local stub HTTP server.
- Image fixtures are generated by the tests themselves (deterministic Pillow
  patterns) — no binary fixtures in git.
- End-to-end (`gen` → `assign`) is verified by hand with `--yes`, since it
  needs the GPU service; the pure pipeline `pick → crop → pixelate → palette →
  assign` is verified automatically with a fixture run directory.

---

## 12. Tasks

| Task | Content | Done when |
|---|---|---|
| T1 | ComfyUI bootstrap at `~/proj/comfyui` (uv venv, torch, requirements) | `GET /system_stats` answers on 127.0.0.1:8188 |
| T2 | Model acquisition (klein 4B primary, Z-Image-Turbo optional) | a `txt2img` job returns an image |
| T3 | ComfyUI client + service lifecycle | stub-server tests pass; `service start/stop/status` work |
| T4 | Workflow JSONs (`txt2img`, `inpaint`, `edit`) + param injection | each workflow runs once through the client |
| T5 | `run.py` + `config.json` + CLI skeleton | `new` creates a valid run dir |
| T6 | `palette.py` + `imops.py` (crop/resize/quantise/preview/montage) with tests | unit tests green |
| T7 | `manifest.py` upsert with tests | unit tests green, no-corruption case pinned |
| T8 | `prov.py` + `review.py` | sidecar written on `assign`; `--yes` path runs unattended |
| T9 | Stages wired end to end (`gen`→`assign`) | one real texture in `assets/textures/` + manifest line + sidecar |
| T10 | Palette selection + `requantise` of committed textures | whole texture set shares one palette |
| T11 | Tool README + reproducibility check | same seed ⇒ identical `50-final.png` sha256 |

---

## 13. Success criteria

1. One command chain takes a prompt to a committed-ready 16x16 material
   texture with only single-key interactions.
2. `materials.txt` is always valid and never hand-edited by the flow.
3. Every committed texture carries a provenance sidecar that reproduces it.
4. The pure pipeline is unit-tested; the ComfyUI-dependent path is verified by
   scripted runs with `--yes`.
5. Moving to approach A or C later touches only the driver layer.

---

## 14. Evolution path (why this shape)

- **A — native bench (later):** a C11/SDL3 window drives the same run
  directories and the same stage functions (initially by shelling out to
  `texgen`, later with the image ops ported to C). Nothing about the artifacts,
  the manifest handling, or the workflows changes.
- **C — ComfyUI-native (later):** the same three workflow JSONs driven from
  ComfyUI's own UI, with `imops.py`/`manifest.py` reusable as a CLI. Again the
  run directory and `50-final.png` stay the contract.
- The stage contract (D1/D2) is deliberately UI-agnostic so both moves are
  additive.

---

## 15. Non-goals (v1)

- No GUI (that is approach A).
- No batching/queueing of many materials in one run.
- No automatic `git add`/`commit` of results (the user reviews and commits).
- No map/slice, sprite, or audio generation — textures only.
- No model training or LoRA work.
- No seamless-tiling requirement (merged runs stretch the face texture; §2).