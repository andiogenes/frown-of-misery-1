/* SPDX-License-Identifier: MIT
 *
 * The integer helpers the fixed-point ports share (defined in fom_rings.cc, where Rings' port grew them): stmlib's
 * SemitonesToRatio and SoftLimit without float, float32's rounding, and a 64 / 32 division for a chip without
 * 64-bit division. */
#ifndef FOM_FIXED_H
#define FOM_FIXED_H

#include <stdint.h>

namespace fom {

/* stmlib's SemitonesToRatio (its 1/256-semitone tables, with float's rounding of semitones + 128), Q30 */
uint64_t SemitonesToRatioQ30(int64_t semitones_q24);
/* stmlib's SoftLimit x (27 + x^2) / (27 + 9 x^2), Q24 in and out */
int32_t SoftLimitQ24(int32_t x);
/* x rounded to float32's grid (ties to even), signed */
int64_t FloatGridSigned(int64_t x);
/* n / d for a quotient that fits 32 bits (saturates otherwise) */
uint32_t Udiv64(uint64_t n, uint32_t d);

}  // namespace fom

#endif
