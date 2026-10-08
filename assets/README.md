Runtime assets live here (fonts, etc.), installed next to the desktop binary by meson (install_subdir) and packed into the APK android/src/main/assets as needed.

## Fonts

- `fonts/KiwiSoda.ttf` — copied from the Spectrax project's
  `bin/resources/fonts/KiwiSoda.ttf` (sha256
  `cdd68272c443104d8f34abc7520a541cab5428431a7eaf553ff75b652498a252`).
  Used by the UI text tier (`src/ui/ui_font.c`) and exercised by the
  `ISO_SMOKE_MS` app-smoke path.

## Audio

- `audio/menu.wav` — short two-tone blip (menu activation).
- `audio/rotate.wav` — upward sweep (camera quarter turn).
- `audio/achievement.wav` — rising arpeggio jingle (Orienteer unlock).

All three are 16-bit mono 44100 Hz, generated reproducibly by
`scripts/gen-audio.py` (Python 3 stdlib `wave` only) and loaded by the SDL3
audio tier (`src/audio/audio.c`). Re-run `python3 scripts/gen-audio.py` to
regenerate them; the WAVs are committed so a normal build never runs it.

