# Frown of Misery

**Mutable Instruments' Braids, Rings and Clouds as a firmware for the M-VAVE FM-1.**

> **Status: early development.** Braids (48 shapes, 4 voices or mono with glide) and Rings (its three resonators) have
> been played on an FM-1. Clouds' granular mode is in, after either of them: rewritten in fixed point and checked
> against Mutable's float code (`dsp/ref`); it runs in the emulator but has not been played on a device yet (see
> [Roadmap](#roadmap)).

It is built on [Dinghy](https://github.com/zvenson/dinghy) (zvenson), the small template firmware that sits on
Felucca's and SLOOP's FM-1 platform. Dinghy provides updates, the USB rescue, MIDI (USB and TRS), USB audio, the panel,
the screen and flash storage. The Braids code and its 44.1 kHz adapter come from
[MD-One](https://github.com/d0pe1/md-one) (d0pe1), which has measured the same Braids code running on the FM-1:
**1.1–3.2 % of the audio budget per voice**.

## Playing it

| Control | What it does |
| --- | --- |
| keys | play: 4 voices sharing one shape, each with its own pitch, or MONO (legato, last note wins). Notes also go out over USB-MIDI. |
| SELECT | the engine: BRAIDS, RINGS, or BRAIDS → RINGS (Braids into Rings' audio input instead of its exciter: a note plays Braids and picks Rings' voice and note; Braids' LEVEL drives Rings, Rings' PART LEVEL is its output) |
| FX | Clouds after the engine, or not. The plates and cables under the parameters show the route. MIDI program change 1–6: BRAIDS, RINGS, BRAIDS → CLOUDS, RINGS → CLOUDS, BRAIDS → RINGS, BRAIDS → RINGS → CLOUDS |
| REC | Clouds' FREEZE: the buffer stops recording and the grains keep playing what it holds (the CLOUDS box fills) |
| ALGORITHM | BRAIDS: the shape, 48 of them, from CSAW to QUESTION MARK. RINGS: the model, MODAL RESONATOR, SYMPATHETIC STRINGS or STRING |
| EDIT | the next page of parameters, through the route's modules in order; KNOB 1–4 turn the four shown on screen. ALGORITHM turns Rings' model on Rings' pages, Braids' shape elsewhere |
| page OSC | TIMBRE, COLOR (what they do depends on the shape, and the screen says), FINE tune ±50 cents, LEVEL |
| page AMP | ATTACK, RELEASE (0 ms … 24 s), GLIDE (off … 24 s), VOICES (POLY 4 / MONO) |
| RINGS page RES | STRUCTURE, BRIGHTNESS, DAMPING, POSITION: Rings' four knobs. STRUCTURE tunes the sympathetic strings and sets the string's dispersion. |
| RINGS page PART | POLY (1, 2 or 4 voices taking the strikes in turn), FINE tune, LEVEL. Rings has no note-off: a key strikes, DAMPING decides how long it rings. |
| CLOUDS page GRAIN | POSITION, SIZE, DENSITY, TEXTURE: Clouds' granular mode, its knobs as on the module (DENSITY: regular grains left of noon, random right of it, none at noon) |
| CLOUDS page MIX | BLEND (dry/wet), SPREAD, FEEDBACK, REVERB: the four on Clouds' BLEND knob |
| CLOUDS page PITCH | PITCH, −24 … +24 semitones; QUALITY: HI (16 bits, 32 kHz) or LO (Clouds' low fidelity: 8-bit μ-law buffers of 4 s, 16 kHz, less CPU). After the sympathetic strings it is always LO (shown as LO (AUTO)). |
| OCT- / OCT+ | octave |
| PRESETS | MIDI channel: OMNI, or 1–16 |
| MASTER | volume |
| MIDI in (USB, TRS) | notes with velocity, pitch bend ±2 semitones, a clock display |
| screen | the faceplate of the module whose page is shown, after Mutable's panels: its name and subtitle, Braids' four-character display, Rings' polyphony and model LEDs, Clouds' FREEZE; the page's four knobs; the last note (and Clouds' own share of the CPU); the route as small plates joined by patch cables; the audio interrupt's CPU load (peak over 0.5 s; also the thin bar along the bottom) |

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
dsp/engine/fom_engine.*  the sound engine behind a small C API (C++, MIT): fom_rings.*, fom_clouds.* the ports
dsp/mi/                  Mutable Instruments' Braids + stmlib (MIT), the 44.1 kHz adapter, the freestanding shim
dsp/ref/                 the float references: Rings and Clouds unmodified, host only; compare.py measures a port
firmware/, tools/, tests/ Dinghy's platform, build and host mode
site/, tools/make_site.py the GitHub Pages site and the web installer; tests/emu_smoke.mjs, tests/web_install_test.mjs
web/                     the FM-1 update protocol over Web MIDI (Felucca, genkigenki)
```

## Roadmap

The FM-1's chip has **no FPU**: a single `float` makes the link fail. Braids was written for a chip without one, so
it ports almost unchanged. Rings and Clouds were written for chips with floating-point hardware. On this chip each
needs a **fixed-point rewrite**, not a port.

1. **Braids on the device**: build, flash, check pitch, CPU load and every shape by ear.
2. **Rings**, rewritten in fixed point: the modal resonator, the sympathetic strings and the string are done
   (`dsp/engine/fom_rings.cc`). `make -C dsp/ref port` checks all three against the float original: the modal at
   40-60 dB SNR at the same rate, the strings at 20-60 dB over a five-note sequence, with colour and envelope
   within 0.1-0.4 dB of Rings at 44.1 kHz. (Rings' string models depend on the sample rate; the port is Rings at
   44.1 kHz.) Left: the FM voice and the "string and reverb" model.
3. **Clouds**, rewritten in fixed point: the granular mode is done (`dsp/engine/fom_clouds.cc`), with Clouds'
   feedback, diffuser and reverb, at its own 32 kHz, stereo, 16-bit, with its 128 KB of buffer (in `.pool`). Its
   grains fall on the same samples as the float original's: `make -C dsp/ref port` measures 73-87 dB SNR for the
   grains, 46-65 dB with the reverb, whose 12-bit memory rounds as Clouds' does but feeds its own roundings back.
   It runs after Braids or Rings (FX), resampled from 44.1 kHz and back. Its cost in the emulator at its densest
   (32 grains): 41 % of the audio budget, 19 % with no grains; so after Rings it plays at most 16 grains at once
   (8 after the sympathetic strings), and fewer for a while if the audio interrupt goes over 85 %. Left: the
   stretch, looping delay and spectral modes, and Clouds' low-fidelity quality (half the rate: half the CPU).
4. Routes like an FM synth's algorithms: Braids → Clouds, Rings → Clouds, Braids → Rings and Braids → Rings → Clouds.
   Braids → Rings is Rings' own external input (no pulse or pluck, its input excitation filter, every string driven):
   `make -C dsp/ref port` measures it against Rings fed the same input, 48-72 dB SNR. After Braids → Rings, Clouds
   runs LO with 8 grains at most (HI does not fit).

## Licence

GPL-3.0-only, like Dinghy. `dsp/` is MIT. [LICENSING.md](LICENSING.md) says who made what.
Not made or endorsed by Mutable Instruments, M-VAVE or Hügelton Instruments.
