# dsp/mi/mb: Marbles

Mutable Instruments' Marbles (the random sampler), its generators only: `marbles/random/`, `marbles/ramp/` and
`marbles/resources.*`, with the stmlib they were written against (`stmlib/`: `dsp/dsp.h`, `dsp/units.*`,
`dsp/parameter_interpolator.h`, `dsp/hysteresis_quantizer.h`, `utils/gate_flags.h`, `utils/random.h`,
`utils/ring_buffer.h`, `stmlib.h`). All MIT, Copyright Emilie Gillet; unmodified.

Upstream: https://github.com/pichenettes/eurorack `f4cfa7697095a28d292bff758beb51deb49b5a86`, stmlib
`e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` (the commits `dsp/ref` and `dsp/mi/braids` come from).

`shim/` stands in for the C++ headers the freestanding toolchain lacks (`math.h`, `cmath`, `algorithm`); it is
ours (MIT). The scales, the loop lengths and the Y dividers, which Marbles keeps in `settings.cc` and `marbles.cc`,
are copied into `dsp/engine/fom_marbles.cc`, which runs these generators as `marbles.cc` does.

This is the one part of the firmware in floating point: on the FM-1 it runs in software (compiler-rt's soft-float,
from the JieLi toolchain, linked by `tools/build.py`), every 32 samples. `tools/build.py` fails the build if any
other object needs a soft-float routine. It is built with `-DTEST` (stmlib's portable C instead of ARM assembly) and
this tree first on the include path (its stmlib is newer than Braids').
