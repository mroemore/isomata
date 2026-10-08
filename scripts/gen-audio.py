#!/usr/bin/env python3
"""Regenerate the committed UI/event sound effects under assets/audio/.

The WAVs are committed, so a normal build never runs this. It is kept here
so the assets are reproducible from source. Python 3 standard library only
(`wave` + `math`), no third-party packages.

All output is 16-bit signed PCM, mono, 44100 Hz — the format audio.c loads
and plays (src/audio/audio.c). The generator is deterministic: the one noise
source is a seeded random.Random, so re-running reproduces the same bytes.

Usage: python3 scripts/gen-audio.py
"""

import array
import math
import os
import random
import wave

RATE = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "audio")

TAU = 2.0 * math.pi


def clamp_i16(x):
    v = int(round(x * 32767.0))
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def write_wav(name, samples):
    path = os.path.join(OUT_DIR, name)
    frames = array.array("h", (clamp_i16(s) for s in samples))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames.tobytes())
    print("wrote %s (%d samples, %.3fs)" % (path, len(samples), len(samples) / RATE))


def attack(t, attack_s):
    """Linear attack from 0 to 1 over attack_s, 1 afterwards."""
    if attack_s <= 0.0 or t >= attack_s:
        return 1.0
    return t / attack_s


def gen_menu():
    """A short two-tone blip (~0.14 s)."""
    dur = 0.14
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        env = attack(t, 0.004) * math.exp(-t * 30.0)
        v = 0.60 * math.sin(TAU * 880.0 * t) + 0.25 * math.sin(TAU * 1760.0 * t)
        out.append(v * env * 0.9)
    return out


def gen_rotate():
    """A whoosh-ish upward sweep (~0.32 s) with a hint of noise."""
    dur = 0.32
    n = int(dur * RATE)
    f0, f1 = 220.0, 1500.0
    rng = random.Random(0x1507A7E)
    out = []
    for i in range(n):
        t = i / RATE
        # Instantaneous phase for a linear frequency sweep.
        phi = TAU * (f0 * t + (f1 - f0) / (2.0 * dur) * t * t)
        swell = attack(t, 0.02) * math.exp(-t * 5.0)
        noise = rng.uniform(-1.0, 1.0) * 0.18
        v = 0.55 * math.sin(phi) + noise * math.sin(TAU * 600.0 * t)
        out.append(v * swell * 0.9)
    return out


def gen_achievement():
    """A rising arpeggio jingle (~0.62 s): C5 E5 G5 C6."""
    notes = [523.25, 659.25, 783.99, 1046.50]
    step = 0.14
    dur = 0.62
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        v = 0.0
        for k, freq in enumerate(notes):
            start = k * step
            if t < start:
                continue
            local = t - start
            env = attack(local, 0.008) * math.exp(-local * 7.0)
            v += 0.32 * math.sin(TAU * freq * local) * env
        out.append(v * 0.9)
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    write_wav("menu.wav", gen_menu())
    write_wav("rotate.wav", gen_rotate())
    write_wav("achievement.wav", gen_achievement())


if __name__ == "__main__":
    main()
