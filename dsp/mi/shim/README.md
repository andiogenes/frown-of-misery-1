# dsp/mi/shim/

Freestanding stand-ins for the handful of C++ library headers Mutable
Instruments' DSP includes.

**Target builds only.** The host has a real libstdc++ and must not use these;
`dsp/Makefile` puts them on the include path for the pi32v2 build and nowhere
else.

## Why they are needed

JieLi's pi32v2 toolchain is strictly freestanding. It ships the compiler's own
headers (`stdint.h`, `stddef.h`) and nothing more — no libc headers, no
libstdc++. Dinghy's firmware carries its own `libc.c` for the same reason.

MI's sources include `<inttypes.h>`, `<cstring>`, `<algorithm>` and `<cstdlib>`,
but use almost nothing from them:

| Header | What MI actually uses |
| --- | --- |
| `inttypes.h` | the `stdint.h` typedefs only |
| `cstring` | `memcpy`, `memset`, `memmove` |
| `algorithm` | `std::min`, `std::max`, `std::swap`, `std::fill`, `std::fill_n` |
| `cstdlib` | `abs` |
| `cmath`, `cstdio` | nothing on the paths we compile |

So these are a few dozen lines rather than a C++ runtime.

`memcpy`/`memset`/`memmove` are *declared* here and linked from the firmware's
own `libc.c`. `std::fill` and friends are templates and compile inline.

## Scope

These exist to support the vendored Braids subset. If a future module needs
more of the standard library, add the minimum it uses and note it above —
do not reach for a full libstdc++ port.
