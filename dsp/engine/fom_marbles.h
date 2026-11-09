/* SPDX-License-Identifier: MIT
 *
 * Marbles (Mutable Instruments' random sampler) at control rate: its own code (dsp/mi/mb/marbles, MIT, Emilie
 * Gillet), unmodified, run by the audio interrupt every 32 samples (1378 Hz) in software floating point. It is the
 * one part of the firmware that uses float: Marbles thinks in volts and probability distributions, does little of
 * it per step, and so it stays the module's code, sample for sample. The interface here is integers only.
 *
 * Out: the three T gates (t1, t2: the master clock, t3), the three X voltages as semitones (1 V per octave) and Y.
 */
#ifndef FOM_MARBLES_H
#define FOM_MARBLES_H

#include <stdint.h>

namespace fom {

const uint32_t kMarblesStep = 32;                  /* samples at 44.1 kHz a step */

struct MarblesPatch {              /* the knobs Q16 (0..65535), the switches as indices */
    uint32_t t_rate, t_bias, t_jitter;
    uint32_t deja_vu, length;      /* DEJA VU (noon: the loop locked), its LENGTH (1..16 steps) */
    uint32_t x_spread, x_bias, x_steps;
    uint32_t y_rate;               /* Y's clock divider: Marbles' hidden y_divider */
    uint8_t t_model;               /* 0..6: complementary Bernoulli, clusters, drums, independent Bernoulli, divider,
                                      three states, Markov */
    uint8_t t_range;               /* 0 x1/4, 1 x1, 2 x4 */
    uint8_t x_range;               /* 0 +2 V, 1 +5 V, 2 +-5 V */
    uint8_t x_scale;               /* 0..5: major, minor, pentatonic, pelog, raag Bhairav, raag Shri */
    uint8_t x_mode;                /* X1 X2 X3: 0 identical, 1 bump, 2 tilt */
    bool t_deja_vu, x_deja_vu;     /* the two DEJA VU buttons */
};

struct MarblesOut {
    uint8_t gates;                 /* bit 0 t1, 1 t2, 2 t3: high */
    int32_t x[3];                  /* X1..X3, semitones Q8 (12 * 256 a volt) */
    int32_t y;                     /* Y, -5..+5 V as -32768..32767 */
};

void marbles_init(uint32_t seed);
void marbles_entropy(uint32_t word);              /* stir the random generator (a key's timing, say) */
/* one step: clock -1 Marbles' own (RATE); else the external clock's level (0, 1): its edges drive T, as a patched
 * T clock input does */
void marbles_step(const MarblesPatch& patch, int clock, MarblesOut* out);

}  // namespace fom

#endif
