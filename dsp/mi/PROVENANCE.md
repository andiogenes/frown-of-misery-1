# Vendored Mutable Instruments source

Taken into Frown of Misery from [MD-One](https://github.com/d0pe1/md-one) `6db198515da0`, together with its 44.1 kHz
`braids/resources.cc`, the adapter `fm1_braids.*` and `shim/`.

Upstream: https://github.com/pichenettes/eurorack
Commit:   f4cfa7697095a28d292bff758beb51deb49b5a86
Vendored: 2026-10-16 (UTC)
Licence:  MIT, (c) Emilie Gillet -- header retained in every file

Copied unmodified:

| File | SHA-256 |
| --- | --- |
| `braids/RESOURCES_GENERATED.md` | `208a442389016d46…` |
| `braids/analog_oscillator.cc` | `dc7fbfe3b34314a2…` |
| `braids/analog_oscillator.h` | `6870f34dd3d6c67e…` |
| `braids/digital_oscillator.cc` | `2b46fd3d702c4570…` |
| `braids/digital_oscillator.h` | `f9a523bee6cfc04c…` |
| `braids/envelope.h` | `388f6b4920b1560e…` |
| `braids/excitation.h` | `e9555383effc411f…` |
| `braids/macro_oscillator.cc` | `715ee728aaae8dfa…` |
| `braids/macro_oscillator.h` | `8e34d2ef5a8914c4…` |
| `braids/parameter_interpolation.h` | `4be46037164cb309…` |
| `braids/quantizer.h` | `4ffd730968c85bdf…` |
| `braids/quantizer_scales.h` | `4a034ae06bd93297…` |
| `braids/settings.h` | `5e2c771a4b29c23f…` |
| `braids/signature_waveshaper.h` | `25080ba2994802a0…` |
| `braids/svf.h` | `17fe7788f2b0da20…` |
| `braids/ui.h` | `bb9e25234407c463…` |
| `braids/vco_jitter_source.h` | `6d12b234c40bccac…` |
| `stmlib/stmlib.h` | `b43df7beed29a7d4…` |
| `stmlib/utils/dsp.h` | `c135555d347df19e…` |
| `stmlib/utils/random.cc` | `303e80784f073a02…` |
| `stmlib/utils/random.h` | `cb9eaabb6a68767b…` |

**NOT copied:** `braids/resources.cc` and `braids/resources.h`.
Upstream ships those generated for 96 kHz. Run
`scripts/gen-braids-resources.sh 44100` to generate them for the
FM-1's sample rate; see `braids/RESOURCES_GENERATED.md`.
