# Licensing

Frown of Misery is a fork of [Dinghy](https://github.com/zvenson/dinghy) (zvenson) at `f77bf03`, and like Dinghy it
is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit) |
| The TRS MIDI IN `firmware/src/midi_uart.c` | Felucca's, as sloopDX carries it, with the ring read by content from Felucca [Salt] 0.9-salt14 by ChanceTheMaker (found by keremimo) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (from OberVave, the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| From OberVave: the storage (blobs, slots), `LCD_IDLE`, the SMK-37 wiring of `platform.c`, `store.h`, the pad event queue in `smk_input.h`, `tests/smk_flash_test.c`, `tests/run_tests.sh`, the CI workflow | Copyright (C) 2026 zvenson |
| The wiring `platform.c`, the host mode `tests/`, the docs in `docs/` (Dinghy's README is `docs/DINGHY.md`) | zvenson |
| The app `firmware/src/frown.c` (begun from Dinghy's `dinghy.c`), the C++ build in `tools/build.py` and `tests/run_host.sh`, `tests/scripts/braids.txt` | Frown of Misery contributors, GPL-3.0-only |

| The web update protocol `web/fm1ota.js`, `web/fm1pkg.js` (unchanged) | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca) `web/`), with M. Geisler's (genkigenki) SMK-37 changes, from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` at `cab2110` |
| The site: `site/` (landing page, web installer `install.html` / `install.js`), `tools/make_site.py`, `tests/emu_smoke.mjs`, `tests/web_install_test.mjs` | Frown of Misery contributors, GPL-3.0-only |

## The sound engine: `dsp/` (MIT)

`dsp/` builds without the firmware and carries `SPDX-License-Identifier: MIT`. MIT is GPL-3.0 compatible, so it links
into the GPL-3.0-only firmware and stays reusable elsewhere. Nothing copied or adapted from a GPL source goes into
`dsp/`.

| What | Licence | Where from |
| --- | --- | --- |
| `dsp/mi/braids/`, `dsp/mi/stmlib/`: Mutable Instruments' Braids and stmlib, unmodified except `braids/resources.cc`, regenerated for 44.1 kHz | MIT, Copyright (C) Emilie Gillet | [pichenettes/eurorack](https://github.com/pichenettes/eurorack) `f4cfa76`, see `dsp/mi/PROVENANCE.md` |
| `dsp/mi/fm1_braids.*` (the 44.1 kHz adapter), `dsp/mi/shim/`, `dsp/engine/fm1_shapes.h`, the regenerated resources | MIT | [MD-One](https://github.com/d0pe1/md-one) by d0pe1, `6db1985` |
| `dsp/engine/fom_engine.*`, `dsp/ref/*.cc`, `dsp/ref/wav.h`, `dsp/ref/*.py`, `tools/gen_rings_tables.py` | MIT | Frown of Misery contributors |
| `dsp/engine/fom_rings.*`: Rings' modal resonator, strings, plucker, part and limiter rewritten in fixed point; `fom_rings_tables.h` carries Rings' own table values | MIT | Frown of Misery contributors, after Rings by Emilie Gillet (MIT) |
| `dsp/ref/eurorack/`: Rings, Clouds and stmlib, unmodified, built on the host only as the float references | MIT, Copyright (C) Emilie Gillet | [pichenettes/eurorack](https://github.com/pichenettes/eurorack) `f4cfa76`, stmlib `e3bd7c9`, see `dsp/ref/PROVENANCE.md` |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

Dinghy's logo (the designburg signet, all rights reserved) is not part of this fork.

## Marbles and compiler-rt

`dsp/mi/mb/` is Mutable Instruments' Marbles (its generators) and the stmlib it uses, unmodified, MIT, Copyright
Emilie Gillet (see `dsp/mi/mb/README.md`). It runs in software floating point: the firmware links the soft-float
routines of compiler-rt, LLVM's runtime library, as the JieLi toolchain ships it (`pi32v2/lib/libcompiler_rt.a`).
compiler-rt is dual licensed under the University of Illinois/NCSA Open Source License and the MIT license
(Copyright the LLVM Team and its contributors).

## The screen

The screen (`firmware/src/frown.c`, GPL-3.0-only) is drawn after the look of Mutable Instruments' panels: their
light aluminium, dark lettering, teal and pink label pills, the lowercase subtitles ("macro oscillator", "resonator",
"texture synthesizer"), Braids' four-character display (its shape codes from `braids/settings.cc`, MIT) and Rings'
LEDs. Mutable's panel files are published under CC-BY-SA 3.0 (pichenettes/eurorack, `*/hardware_design/panel/`), and
VCV Rack's Audible Instruments draws them too; Frown of Misery copies no artwork from either. Its knobs, plates,
patch cables and the band's diamond chain are drawn anew in code. No Mutable Instruments logo appears.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37" and "MKE-P37"
belong to their owners. "Mutable Instruments", "Braids", "Rings" and "Clouds"
name Emilie Gillet's modules. Frown of Misery is not made or endorsed by any of them.
