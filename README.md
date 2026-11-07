# Frown of Misery

**Mutable Instruments' Braids, and later Rings and Clouds, as a firmware for the M-VAVE FM-1.**

> **Status: 0.0, a starting point.** Braids is in: 48 shapes, 4 voices sharing one shape, playable from the keys and
> MIDI. The code compiles on a computer but has **not yet been built for the FM-1 or played on one**. Rings and Clouds
> are still to come (see [Roadmap](#roadmap)).

It is built on [Dinghy](https://github.com/zvenson/dinghy) (zvenson), the small template firmware that sits on
Felucca's and SLOOP's FM-1 platform. Dinghy provides updates, the USB rescue, MIDI (USB and TRS), USB audio, the panel,
the screen and flash storage. The Braids code and its 44.1 kHz adapter come from
[MD-One](https://github.com/d0pe1/md-one) (d0pe1), which has measured the same Braids code running on the FM-1:
**1.1–3.2 % of the audio budget per voice**.

## Playing it

| Control | What it does |
| --- | --- |
| keys | play, 4 voices (one shape, one TIMBRE and COLOR, each voice its own pitch); the notes also go out over USB-MIDI |
| ALGORITHM | the shape: 48 of them, from CSAW to QUESTION MARK |
| KNOB 1 / KNOB 2 | TIMBRE / COLOR; what they do depends on the shape, and the screen says |
| KNOB 3 / KNOB 4 | attack / release, 0 ms … 24 s |
| OCT- / OCT+ | octave |
| PRESETS | MIDI channel: OMNI, or 1–16 |
| MASTER | volume |
| MIDI in (USB, TRS) | notes with velocity, pitch bend ±2 semitones, a clock display |

All settings are saved to flash 2 s after the last change, once nothing is sounding.
SMK-37 builds also work (`--board smk37`), as they do in Dinghy.

## Building

The setup is the same as Dinghy's ([docs/DINGHY.md](docs/DINGHY.md) has every step): Linux x86-64 (or Docker),
Python 3 with Pillow, the JieLi toolchain (`tools/get_toolchain.sh`) and the JieLi AC79 SDK.

```sh
tests/run_host.sh tests/scripts/braids.txt   # on your computer: build/host/frown.wav and screen PNGs
./build.sh                                   # -> build/frown.fwsc
python3 tools/fm1_install.py build/frown.fwsc
```

Or install from the browser: **[the web installer](https://andiogenes.github.io/frown-of-misery-1/install.html)** works in
Chrome, Edge and other Chromium browsers (they support Web MIDI with SysEx). It uses Felucca's update code, with
genkigenki's SMK-37 changes, from `web/`. If an install is cut off, it resumes. It refuses the wrong board and other
firmwares' update modes before anything is written; `tests/web_install_test.mjs` checks all of this against a
simulated device.

To get back to any other firmware, use the [FM-1 Firmware Hub](https://thefirmwarehub.com). If a build does not
start, hold **OCT-** while switching on to reach the USB rescue. Installing firmware is at your own risk.

## In the browser

**[andiogenes.github.io/frown-of-misery-1](https://andiogenes.github.io/frown-of-misery-1/)** runs the newest build
from `main` in [fm1-emu](https://github.com/joakimeriksson/fm1-emu) (Joakim Eriksson, MIT). fm1-emu emulates the
FM-1's chip, so the page runs the same `frown.fwsc` you would install. `/emu/smk37/` has the SMK-37 build.

`.github/workflows/pages.yml` builds it on every push to `main`. The steps are the same on a computer:

```sh
BOARD=smk37 ./build.sh && ./build.sh && tests/run_host.sh tests/scripts/braids.txt
git clone https://github.com/joakimeriksson/fm1-emu && fm1-emu/web/build.sh "$PWD/emu-dist"   # Rust + wasm32
python3 tools/make_site.py emu-dist _site && node tests/emu_smoke.mjs _site/emu _site/frown.fwsc
(cd _site && python3 -m http.server 8000)                                                   # http://localhost:8000
```

The workflow pins fm1-emu to one commit (`FM1_EMU_REV`). Change it deliberately to update the emulator.

## Layout

```
firmware/src/frown.c     the app: voices, panel, screen, settings
dsp/engine/fom_engine.*  the sound engine behind a small C API (C++, MIT)
dsp/mi/                  Mutable Instruments' Braids + stmlib (MIT), the 44.1 kHz adapter, the freestanding shim
firmware/, tools/, tests/ Dinghy's platform, build and host mode
site/, tools/make_site.py the GitHub Pages site and the web installer; tests/emu_smoke.mjs, tests/web_install_test.mjs
web/                     the FM-1 update protocol over Web MIDI (Felucca, genkigenki)
```

## Roadmap

The FM-1's chip has **no FPU**: a single `float` makes the link fail. Braids was written for a chip without one, so
it ports almost unchanged. Rings and Clouds were written for chips with floating-point hardware. On this chip each
needs a **fixed-point rewrite**, not a port.

1. **Braids on the device**: build, flash, check pitch, CPU load and every shape by ear.
2. **Rings**, rewritten in fixed point: the modal resonator first (it is the cheapest of its models), then
   sympathetic strings and the string model, with polyphony as the CPU budget allows.
3. **Clouds**, rewritten in fixed point: the granular mode first, using a buffer of about 118 KB in `.pool`
   (336 KB is free), then pitch shifter, looping delay and spectral as the budget allows. It takes the Braids or
   Rings voice as its input.
4. An engine switch on the SELECT encoder: Braids, Rings, Braids → Clouds, Rings → Clouds.

## Licence

GPL-3.0-only, like Dinghy. `dsp/` is MIT. [LICENSING.md](LICENSING.md) says who made what.
Not made or endorsed by Mutable Instruments, M-VAVE or Hügelton Instruments.
