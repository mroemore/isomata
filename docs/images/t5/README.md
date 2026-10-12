# T5 evidence — the customer as an errand

Desktop frames are Xvfb-only (`TESTING.md` "GUI runs: Xvfb only"), captured with
`SDL_AUDIODRIVER=dummy` and `ISO_LOG=debug`, never on the live display. The phone
frames are the headless `emulator-5554`. The capture script is
`.tmp_files/capture5.sh` (scratch, gitignored — the recipe is at the bottom of
this file). See the task report
`../../../.superpowers/sdd/2026-10-11-isomata-agent-framework/task-5-report.md`.

The APK published for this task: **`isomata_20261012_005329_387409.apk`** —
download page `https://appk.lynsand.tricky.click/isomata/` — built from HEAD
`d33283c` with `JAVA_HOME=~/.local/opt/jdk-17 ./gradlew assembleDebug` (the box's
default JDK is 11 and the Android Gradle Plugin needs 17).

## What the story is, in log lines

The whole thing is one customer adopting one errand template and working down its
seven steps. `run.log` is the whole capture run (357 lines); `log-excerpt.txt` is
just the customer half.

```
t=2552ms  customer #0 adopted errand LAUNDRY_CLEAN (band 0)
t=2552ms  customer #0 chose machine 0 (dist 2, score 2, policy closest)
t=3375ms  customer #0 paid 3 to machine 0
t=3375ms  customer #0 loading 5s in place at machine 0
t=8392ms  customer #0 started machine 0; walking away while it runs
t=38417ms customer #0: machine 0 run done, walking back to collect
t=39270ms customer #0 collecting 5s in place at machine 0
t=51298ms customer #3 waiting on a machine; filling time with NAP (band 6)
t=55638ms customer #2 waiting on a machine; filling time with NAP (band 6)
t=88815ms customer #0 left (clean dry)
t=90906ms customer #1 left (clean dry)
t=99701ms SCRIPTED BREAK: washer 0 broke mid-run
```

Five beats the task was asked to show, and where each is:

| beat | where |
|---|---|
| arrive and adopt the wash errand, with the urgency that chose it | `01-arrival-and-adoption.png` + the first log line |
| queue behind each other | `02-divergence-pair-arrives.png` (customers #1/#2/#3 arrive at 3.4 s while #0 holds washer 0) |
| **fill time while a machine runs** | `05-fill-time-a-nap-while-waiting.png` / `06-…second-customer.png` + the two `filling time with NAP (band 6)` lines at t=51.3 s and t=55.6 s |
| the scripted break and the re-plan | `08-scripted-break-and-replan.png`, `09-replanned-onto-another-washer.png` + `SCRIPTED BREAK` at t=99.7 s |
| leave clean | `07-first-departures-clean-dry.png` + `left (clean dry)` at t=88.8 s / t=90.9 s |

## Frames

- `01-arrival-and-adoption.png` — the machine row (three cool washers, two warm
  dryers, the isolated washer east) with customer #0 at the door having just
  adopted `LAUNDRY_CLEAN`.
- `02-divergence-pair-arrives.png` — the scripted divergence pair. Customer #2
  (policy `spread_k`) picks the isolated washer 5 at distance 15, while the
  `closest` customers take the cluster — the heuristic divergence the errand
  migration had to preserve, visible in the log two seconds in.
- `03-first-runs-owner-away.png`, `04-washing-in-progress.png` — runs started,
  bodies walked away to the wait spot (the owner-away behaviour, now a job).
- `05-fill-time-a-nap-while-waiting.png`, `06-fill-time-second-customer.png` —
  **the moment this task exists for.** Both dryers are busy, so customers #3 and
  #2 have nothing to dry in and the framework gives them something else to do:
  a NAP at band 6, announced once per fill. Neither abandons the errand; both
  take the dryer when it frees (#3 chooses machine 3 at t=87.9 s, pays, loads).
- `07-first-departures-clean-dry.png` — #0 and #1 walk out clean dry.
- `08-scripted-break-and-replan.png` — washer 0 breaks mid-run (the leftmost
  washer reads dark against the grass) and the customer re-plans onto another.
- `09-replanned-onto-another-washer.png`, `10-steady-state.png` — the floor
  carries on with the spawner on.
- `android-01-menu.png`, `android-02-level.png`, `android-03-fill-time.png` —
  the phone build of the same build. `android-log-excerpt.txt` is the same story
  from logcat: four adoptions at t=11-12 s, both fills at t=59.7 s and t=64.0 s.

**Honest about legibility**: the customers are 1.5-unit billboards in a
1280x720 view centred on the machine row, and the demo's camera does not follow
them — the frames show the laundromat's *state* (which machine is running, which
is broken) and the log carries the story. This is the same limitation the T23
report recorded, not a new one, and no frame here should be read as evidence of
what a given customer is doing at that instant.

## The recipe

```sh
Xvfb :77 -screen 0 1280x720x24 -nolisten tcp &
DISPLAY=:77 SDL_AUDIODRIVER=dummy ISO_LOG=debug ISO_SMOKE_MS=150000 build/isomata &
sleep 2.5; DISPLAY=:77 xdotool key Return        # menu -> level
DISPLAY=:77 import -window root shot.png          # xwd/scrot also available
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.isomata.app/org.libsdl.app.SDLActivity
adb shell input keyevent 66                        # menu -> level
adb exec-out screencap -p > phone.png
```
