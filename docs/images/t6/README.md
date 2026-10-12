# T6 evidence — the staff half: shared board, competencies, world-seeded faults

Desktop frames are Xvfb-only (`TESTING.md` "GUI runs: Xvfb only"), captured with
`SDL_AUDIODRIVER=dummy` and `ISO_LOG=debug`, never on the live display. The
recipe is at the bottom of this file; `log-excerpt.txt` is the whole staffing
story out of the capture run.

## What the story is, in log lines

A machine faults. The world posts the repair on the ONE shared board the floor
shares, offers the fault errand to every staffer at its declared floor, and the
claim pass hands the job to exactly one of the two hires — who walks over,
repairs it, and lets it go. Nobody else touches it.

```
t=100099ms  SCRIPTED BREAK: washer 0 broke mid-run
t=100099ms  REPAIR POSTED on the shared board: slot 0, machine 0 at (1,1), TIDY for 4.0s
t=100099ms  world-seeded ERR_FIX_MACHINE offered to 2 staff (floor JOB_PRIO_ASSIGNED,
            adopted at that tier and not above it)
t=100130ms  1 staff claimed shared work
            staff FITTER (agent 3) took shared job 0 (TIDY band 2, repair 3 speed 2
            clumsy 0) -> errand FIX_MACHINE
t=103500ms  REPAIR DONE: machine 0 free again (slot 0)
            staff FITTER (agent 3) released job 0
```

The APPRENTICE appears in the log twice at the top (its hire line, and one
self-generated fill early on) and never again: it was offered the same errand
at the same floor, it was in the same claim pass, and it did not get the job.
That is the contention rule doing its work — the one posting went to one agent,
and the one-job-per-agent rule meant the other had nothing to fall back on.

**One repair took 3.4 s because of the competencies, not the clock.** The
posting's declared runtime is 4.0 s; the FITTER's `SKILL_REPAIR` of 3 divides
it by 4, so the activity itself is 1.0 s, and the rest is the walk to (1,1).
The apprentice's own figure would have been 4.0 s and then some — `FLAW_PENALTY`
doubles it — which is what "a bad hire lengthens the work" means here, and
neither hire is refused the job.

## The frames

| frame | beat | what to look at |
|---|---|---|
| `01-before-the-fault.png` | the floor before anything breaks | washer 0 (leftmost) reads blue like its neighbours |
| `02-fault-posted-claimed-by-one.png` | the fault, and the one claim | washer 0 reads **dark**; the **violet FITTER** is standing at it; the **teal APPRENTICE** is still out on the floor, two tiles away and not coming — that is the no-double-claim |
| `03-repair-done.png` | the repair landed | washer 0 reads blue again, 3.4 s after the break |

`02` and `03` are crops (640x170 of the 1280x720 frame, scaled 2x) of the
machine row; `01` is the whole frame. The crop is the honest way to show the
tint difference at this camera distance — the staff are 1.8-unit billboards in
a view centred on the machine row, and the demo's camera does not follow them.

**Honest about legibility**: violet is the FITTER and teal is the APPRENTICE
by tint alone; at the full-frame distance a one-tile difference is hard to
read, which is why the crops exist. The log carries the precise story (which
agent, which slot, which band, which errand), and the frames carry the state
(a machine dark, then not). No frame here should be read as evidence of what a
given agent is doing at some other instant.

## The recipe

```sh
Xvfb :79 -screen 0 1280x720x24 -nolisten tcp &
DISPLAY=:79 SDL_AUDIODRIVER=dummy ISO_LOG=debug ISO_SMOKE_MS=170000 build/isomata &
sleep 3; DISPLAY=:79 xdotool key Return        # menu -> level, as any session starts
sleep 8;  DISPLAY=:79 import -window root 01-before-the-fault.png
sleep 82                                            # the scripted break lands ~t=100 s
for i in 0 2 4 6 8 10; do
    sleep 2; DISPLAY=:79 import -window root frame-$i.png
done
```