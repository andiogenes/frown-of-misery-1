#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The fixed-point Rings (dsp/engine/fom_rings.cc) against Mutable's float original, over the knobs' range.

Two questions, two references:
  arithmetic  the port vs Rings compiled at 44.1 kHz (ref_rings44): the same algorithm at the same rate, so what
              differs is the port's fixed point. Measured as SNR below 16 kHz: at least 30 dB. (Above, the port leaves
              out modes between 0.485 and 0.49 fs, 21.4-21.6 kHz, which Rings keeps: inaudible, but at STRUCTURE 1
              with BRIGHTNESS 1 they hold enough energy to sink a full-band SNR.)
  sound       the port vs Rings as it is, at 48 kHz (ref_rings): the change of rate moves the highest partials (Rings'
              tan approximation and its modes up to 0.49 fs are rate-dependent), so a waveform match is not the
              question there; the tone colour (third-octave spectrum) and the envelope are: within 1 dB each.
              Except at STRUCTURE >= 0.9: there the partials are stretched so far apart that which of them fall
              below Nyquist, and where Rings' tan approximation puts them, depends on the rate; the 5-16 kHz bands
              then differ by up to ~3.5 dB from the 48 kHz module (and the envelope, whose energy those partials
              carry, by up to ~2 dB) while the port still matches Rings at 44.1 kHz.
The string models (1 sympathetic strings, 2 string) are different: Rings' own string path depends on the rate (a
per-sample damping FIR, per-sample noise, a 24-sample block grid), and Rings compiled at 44.1 kHz differs from the
48 kHz module by as much as 4-10 dB in colour and envelope, exactly as the port does. What the port can promise there
is to be Rings at 44.1 kHz, so for them the sound is measured against ref_rings44 too (lsd44, env44: within 1 dB);
lsd48 / env48 are printed for information. The string with negative dispersion (STRUCTURE < 0.24) has a nonlinear
"curved bridge" in its loop: a waveform match decays there by design, so its SNR is not asked and its colour and
envelope may differ by up to 2 dB (with BRIGHTNESS high and long DAMPING the loop goes chaotic).

Each case strikes five notes, C2 to C6, 0.6 s apart. The first 24 samples are left out: Rings glides its POSITION
in from 0 over its first block after power-on, the port does not.

  rings_matrix.py [--quick]          (from dsp/ref, after `make port`)"""
import subprocess
import sys

import numpy as np

import compare

SNR_MIN, LSD_MAX, ENV_MAX = 30.0, 1.0, 1.0
SNR_MIN_STRINGS = 20.0                  # five notes ringing into each other: long tails, see above
LSD_MAX_STRETCHED, ENV_MAX_STRETCHED = 3.5, 2.0   # STRUCTURE >= 0.9: see above
SNR_BAND = 16000.0
NOTES = ["notes=36,48,60,72,84", "every=0.6", "seconds=3"]
SKIP = 24


def cases(quick):
    yield from model_cases(0, quick)
    yield from model_cases(1, quick)
    yield from model_cases(2, quick)


def model_cases(model, quick):
    base = dict(model=model, structure=0.25, brightness=0.5, damping=0.5, position=0.3, poly=1)
    yield dict(base)
    sweep = {"structure": [0.0, 0.5, 0.75, 1.0], "brightness": [0.0, 0.25, 0.75, 1.0],
             "damping": [0.0, 0.25, 0.75, 1.0], "position": [0.0, 0.6, 1.0], "poly": [2, 4]}
    if quick:
        sweep = {k: [v[0], v[-1]] for k, v in sweep.items()}
    for k, vals in sweep.items():
        for v in vals:
            yield dict(base, **{k: v})
    if not quick:
        for s in (0.0, 1.0):
            for b in (0.0, 1.0):
                yield dict(base, structure=s, brightness=b, damping=0.9)


def lowpass(x, rate, cutoff, taps=255):
    """A windowed-sinc FIR (Blackman): its ringing stays local, unlike a brick wall over the whole file, which
    smears the files' abrupt starts across every sample."""
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / rate * n) * np.blackman(taps)
    return np.convolve(x, h / h.sum(), mode="same")


def pair(ref, port, rate, args, band=None):
    subprocess.run([ref, "build/m_ref.wav", *args], check=True, capture_output=True)
    subprocess.run([port, "build/m_port.wav", f"refrate={rate}", *args], check=True, capture_output=True)
    x, xr = compare.read("build/m_ref.wav")
    y, yr = compare.read("build/m_port.wav")
    x, y = x[SKIP:], y[SKIP * yr // xr:]
    if band:
        x, y = lowpass(x, xr, band), lowpass(y, yr, band)
    return compare.measure(x, xr, y, yr, 3.0)


def main():
    quick = "--quick" in sys.argv
    bad = 0
    print(f"{'case':66} {'snr44':>6} {'lsd44':>6} {'env44':>6} {'lsd48':>6} {'env48':>6}")
    for c in cases(quick):
        args = [f"{k}={v}" for k, v in c.items()] + NOTES
        a = pair("build/ref_rings44", "build/port_rings44", 44100, args, SNR_BAND)
        s = pair("build/ref_rings", "build/port_rings", 48000, args)
        if c["model"] == 0:
            stretched = c["structure"] >= 0.9
            lsd_max = LSD_MAX_STRETCHED if stretched else LSD_MAX
            env_max = ENV_MAX_STRETCHED if stretched else ENV_MAX
            ok = a["snr"] >= SNR_MIN and s["lsd"] <= lsd_max and s["env"] <= env_max
            same = None
        else:
            same = pair("build/ref_rings44", "build/port_rings44", 44100, args)
            nonlinear = c["model"] == 2 and c["structure"] < 0.24
            lim = 2.0 if nonlinear else LSD_MAX
            ok = (nonlinear or a["snr"] >= SNR_MIN_STRINGS) and same["lsd"] <= lim and same["env"] <= lim
        bad += not ok
        name = " ".join(f"{k}={v}" for k, v in c.items())
        l44 = f"{same['lsd']:6.2f} {same['env']:6.2f}" if same else f"{'':6} {'':6}"
        print(f"{name:66} {a['snr']:6.1f} {l44} {s['lsd']:6.2f} {s['env']:6.2f}  {'ok' if ok else 'FAILED'}")
    print(f"{bad} case(s) outside snr44 >= {SNR_MIN}, lsd48 <= {LSD_MAX}, env48 <= {ENV_MAX} dB "
          f"({LSD_MAX_STRETCHED}, {ENV_MAX_STRETCHED} at structure >= 0.9)" if bad
          else "Rings (modal, sympathetic strings, string): the port matches the reference")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
