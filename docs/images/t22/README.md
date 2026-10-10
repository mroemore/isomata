# T22 evidence — interactable machines

Desktop frames are Xvfb-only (TESTING.md); the phone frame is the headless
`roecounter_test` emulator. See the task report
`../../../.superpowers/sdd/2026-10-09-isomata-phase2/t22-interactables-report.md`.

## The machine row

Five machines in a "laundromat corner" on the north grass at `(x=1..5, z=1)`,
clear of the room `(x4-8, z9-12)`, the tower `(x6-7, z3-6)` and every T21
critter lane:

| handle | kind   | tile  | fee | runSecs | tint              |
|--------|--------|-------|-----|---------|-------------------|
| 0      | WASHER | (1,1) | 3   | 30 s    | cool `150,200,255` |
| 1      | WASHER | (2,1) | 3   | 30 s    | cool              |
| 2      | WASHER | (3,1) | 3   | 30 s    | cool              |
| 3      | DRYER  | (4,1) | 2   | 30 s    | warm `255,190,130` |
| 4      | DRYER  | (5,1) | 2   | 30 s    | warm              |

Each is a `1.2 x 1.4` billboard through the sprite path. The kinds read apart
because the billboard samples a new flat-white `machine` material, so the tint
is the object's colour: three blue washers then two warm dryers.

## Frames

- `machines-row.png` — Xvfb, default level view: the five-machine row is the
  blue/warm strip on the north grass, with the primary smoke critter (green
  tint) walking up to washer 0.
- `run-phase.png` — Xvfb, the same run rotated (`Q`): the row reads clearly and
  the primary is away at home during washer 0's 30 s RUN (the empty tile at the
  run's "owner away" moment); the T21 demo critters roam the plateau.
- `android-machines.png` — headless emulator phone frame after Start, panned so
  the row is fully in frame: three blue washers + two tan dryers.
- `log-excerpt.txt` — the `ISO_LOG=debug` timeline: claim → load(5 s) → run
  started → PHASE_DONE wake → walk back → collect(5 s) → release → cycle 2,
  with the challenger's claim refused (state RUNNING, owner 3) throughout.
