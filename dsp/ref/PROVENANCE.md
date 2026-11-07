# The float references

`eurorack/` is Mutable Instruments' original code: the files Rings' `Part`, Clouds' `GranularProcessor` and their
stmlib need, copied **unmodified**. It is built on the host only (`make -C dsp/ref`), never for the FM-1: the chip has
no FPU. It is what the fixed-point ports in `dsp/` are measured against (`compare.py`).

Upstream: https://github.com/pichenettes/eurorack `f4cfa7697095a28d292bff758beb51deb49b5a86` (the same commit as
`dsp/mi/braids`), its submodule https://github.com/pichenettes/stmlib `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec`.
Licence: MIT, Copyright Emilie Gillet: the notice is in every file and in `eurorack/LICENSE` (stmlib's).

`ref_rings.cc`, `ref_clouds.cc`, `wav.h`, `compare.py` and the `Makefile` are ours (MIT).

| Reference | Rate | Drives it like the module |
| --- | --- | --- |
| `ref_rings OUT.wav model= poly= structure= brightness= damping= position= notes= every= seconds= [in=]` | 48 kHz | `Part::Process` in 24-sample blocks, internal exciter, a strum per note |
| `ref_clouds IN.wav OUT.wav mode= quality= position= size= density= texture= pitch= blend= spread= feedback= reverb= freeze=` | 32 kHz | `GranularProcessor::Process` in 32-frame blocks, `Prepare()` after each |
