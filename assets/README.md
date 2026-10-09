Runtime assets live here (fonts, maps, shaders, textures, audio). The desktop build installs this tree next to the binary via meson `install_subdir`; the Android build packs the same tree into the APK through the Gradle source set `assets.srcDirs = ['../../assets']` (`android/app/build.gradle`), not by copying into a checked-in `android/src/main/assets` directory.

## Fonts

- `fonts/KiwiSoda.ttf` — copied from the Spectrax project's
  `bin/resources/fonts/KiwiSoda.ttf` (sha256
  `cdd68272c443104d8f34abc7520a541cab5428431a7eaf553ff75b652498a252`).
  Used by the UI text tier (`src/ui/ui_font.c`) and exercised by the
  `ISO_SMOKE_MS` app-smoke path.

## Audio

- `audio/menu.wav` — short two-tone blip (menu activation).
- `audio/rotate.wav` — upward sweep (camera rotation step).
- `audio/achievement.wav` — rising arpeggio jingle (Orienteer unlock).

All three are 16-bit mono 44100 Hz, generated reproducibly by
`scripts/gen-audio.py` (Python 3 stdlib `wave` only) and loaded by the SDL3
audio tier (`src/audio/audio.c`). Re-run `python3 scripts/gen-audio.py` to
regenerate them; the WAVs are committed so a normal build never runs it.

## Maps

- `maps/demo.txt` — the ASCII source of truth for the demo scene (16x16, 9
  horizontal slices, room/roof/skylight/doorway/lamp; see `voxmap.h`).
- `maps/demo/` — the same scene as PNG slices (`00.png`..`08.png`) plus
  `legend.txt`, generated from `demo.txt` by `scripts/gen-map-png.sh`. The app
  prefers this directory and falls back to `demo.txt`; the two are proven
  identical by `tests/test_pngmap.c`. See `docs/map-authoring.md`.

