#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The fixed-point Clouds (dsp/engine/fom_clouds.cc) against Mutable's float original, over the knobs' range.

The input is Rings' string (ref_rings, five notes), the module's input as the FM-1 will feed it. Two questions:
  arithmetic  port_clouds vs ref_clouds, both at Clouds' 32 kHz with the same 16-bit knob values: the same algorithm
              step for step, the same random generator, so the grains fall on the same samples and the waveforms
              match. SNR at least 40 dB. (The grains alone come out at 73-87 dB; the feedback and the reverb, whose
              12-bit memory rounds as Clouds' does but feeds its own roundings back, lower it to 46-65 dB.)
  the FM-1's path   CloudsFx at 44.1 kHz (port_clouds fx=1: resampled in and out, the dry signal mixed at 44.1 kHz)
              vs the reference: the third-octave colour within 1.5 dB (the resamplers roll off above 11 kHz), the
              envelope within 2.5 dB (the wet signal is 2 ms late there, so a frozen buffer, or grains reading just
              behind the record head, hold the notes' attacks 2 ms elsewhere). DENSITY near noon
              leaves a grain or two in four seconds: only the arithmetic is measured there.

  clouds_matrix.py [--quick]          (from dsp/ref, after `make port`)"""
import subprocess
import sys

import numpy as np

import compare

SNR_MIN, LSD_MAX, ENV_MAX = 40.0, 1.5, 2.5
SECONDS = "seconds=4"


def cases(quick):
    base = dict(position=0.5, size=0.5, density=0.6, texture=0.5, pitch=0, blend=1, spread=0.5, feedback=0, reverb=0.3)
    yield dict(base)
    sweep = {"position": [0.0, 1.0], "size": [0.0, 0.25, 0.9], "density": [0.0, 0.2, 0.45, 0.55, 0.8, 1.0],
             "texture": [0.0, 0.3, 0.8, 1.0], "pitch": [-24, -12, 7, 12, 24], "spread": [0.0, 1.0],
             "feedback": [0.5, 0.9], "reverb": [0.0, 0.7]}
    if quick:
        sweep = {k: [v[0], v[-1]] for k, v in sweep.items()}
    for k, vals in sweep.items():
        for v in vals:
            yield dict(base, **{k: v})
    yield dict(base, density=0.7, freeze=1.5, reverb=0)
    yield dict(base, density=0.3, pitch=12, size=1.0, reverb=0)
    yield dict(base, blend=0.5)


def run(prog, *args):
    subprocess.run([prog, *args], check=True, capture_output=True)


def main():
    quick = "--quick" in sys.argv
    run("build/ref_rings", "build/cm_in.wav", "model=2", "notes=36,48,60,67,72", "every=0.5", "seconds=4")
    bad = 0
    print(f"{'case':78} {'snr':>6} {'lsdfx':>6} {'envfx':>6}")
    for c in cases(quick):
        args = [f"{k}={v}" for k, v in c.items()] + [SECONDS]
        run("build/ref_clouds", "build/cm_in.wav", "build/cm_ref.wav", *args)
        run("build/port_clouds", "build/cm_in.wav", "build/cm_port.wav", *args)
        run("build/port_clouds", "build/cm_in.wav", "build/cm_fx.wav", "fx=1", *args)
        x, xr = compare.read("build/cm_ref.wav")
        y, yr = compare.read("build/cm_port.wav")
        z, zr = compare.read("build/cm_fx.wav")
        f = compare.measure(x, xr, z, zr, 10.0)
        a = compare.measure(x, xr, y, yr, 3.0)
        if float(np.sqrt(np.mean(x * x))) < 1e-3:          # (nearly) no grains: a colour means nothing there
            ok = a["snr"] >= SNR_MIN and float(np.sqrt(np.mean(z * z))) < 2e-3
            print(f"{' '.join(f'{k}={v}' for k, v in c.items()):78} {a['snr']:6.1f} sparse  {'ok' if ok else 'FAILED'}")
            bad += not ok
            continue
        ok = a["snr"] >= SNR_MIN and a["lag"] == 0 and f["lsd"] <= LSD_MAX and f["env"] <= ENV_MAX
        bad += not ok
        name = " ".join(f"{k}={v}" for k, v in c.items())
        print(f"{name:78} {a['snr']:6.1f} {f['lsd']:6.2f} {f['env']:6.2f}  {'ok' if ok else 'FAILED'}")
    print(f"{bad} case(s) outside snr >= {SNR_MIN} dB, fx lsd <= {LSD_MAX}, env <= {ENV_MAX} dB" if bad
          else "Clouds (granular): the port matches the reference")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
