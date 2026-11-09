/* SPDX-License-Identifier: MIT
 * Freestanding stand-in for <math.h>, for Marbles (dsp/mi/mb) alone: the toolchain has no libc headers. Marbles runs
 * in software floating point (compiler-rt's, linked for it); only what its code paths call is here, written so that
 * no libm is needed. */
#ifndef FOM_MB_SHIM_MATH_H
#define FOM_MB_SHIM_MATH_H

static inline float fabsf(float x) { return x < 0.0f ? -x : x; }
static inline double fabs(double x) { return x < 0.0 ? -x : x; }
#ifdef __cplusplus
static inline float fabs(float x) { return x < 0.0f ? -x : x; }
#endif
/* declared, never defined: Marbles' paths do not call it (stmlib's Sqrt under TEST names it); a use would fail to
 * link, loudly */
float sqrtf(float x);

#endif
