/* SPDX-License-Identifier: MIT
 *
 * Rings' modal resonator in fixed point, for a chip without an FPU.
 *
 * Written from Mutable Instruments' Rings (Emilie Gillet, MIT): rings/dsp/resonator.cc, the modal path of
 * rings/dsp/part.cc and rings/dsp/limiter.h, the float originals in dsp/ref/eurorack. The algorithm is theirs, step
 * for step; the numbers are integers, at 44.1 kHz instead of Rings' 48 kHz, with times kept equal in seconds.
 * dsp/tests/rings_vs_ref.sh measures it against the float original (dsp/ref/compare.py).
 *
 * Formats: audio Q24 inside the resonators (1.0 = 2^24, 42 dB of headroom), Q15 at the output. Frequencies are
 * f / fs in Q32. A resonator's coefficients are recomputed only when its note or a knob changes.
 */
#ifndef FOM_RINGS_H
#define FOM_RINGS_H

#include <stddef.h>
#include <stdint.h>

namespace fom {

const int32_t kMaxModes = 64;
const int32_t kMaxPolyphony = 4;

struct RingsPatch {               /* Rings' four knobs, 0..65535 */
    uint32_t structure, brightness, damping, position;
};

/* One voice: the excitation filter and a bank of up to 64 band-pass modes (stmlib's Svf, as Rings uses it). */
class ModalVoice {
 public:
    void Init();
    void set_resolution(int32_t modes) { resolution_ = modes > kMaxModes ? kMaxModes : modes; dirty_ = true; }
    void set_frequency(uint32_t f_q32) { if (f_q32 != frequency_) { frequency_ = f_q32; dirty_ = true; } }
    /* The internal exciter's pulse on this voice, at level Q15 (Rings: 32767). */
    void Strike(int32_t level_q15) { strike_ = level_q15; }
    bool idle() const { return idle_; }
    /* Adds n samples of the odd and the even modes (Q24) to odd[] and even[]. active: this voice takes the exciter
     * (the others' excitation filters close to 10 Hz, as in Rings). */
    void Render(const RingsPatch& patch, bool active, int32_t* odd, int32_t* even, size_t n);

 private:
    int32_t ComputeFilters(const RingsPatch& patch);

    /* the modes: g = tan(pi f) Q26, rg = r + g Q26, h = 1 / (1 + r g + g^2) Q31; states Q24 */
    int32_t g_[kMaxModes], rg_[kMaxModes], h_[kMaxModes];
    int32_t s1_[kMaxModes], s2_[kMaxModes];
    int32_t num_modes_, resolution_;
    uint32_t frequency_;
    RingsPatch computed_;          /* the knobs the coefficients are for */
    bool dirty_, idle_;
    /* the excitation filter (a 2-pole low-pass, Q 1.5): coefficients Q24, states 64-bit Q24 */
    int64_t ex_s1_, ex_s2_;
    int32_t strike_;
    int32_t pending_;              /* the struck sample's filter output, the resonators' next input */
    bool has_pending_;
    uint32_t position_now_;        /* Q16, the position the amplitudes were last made for */
    int16_t amp_[kMaxModes];       /* the modes' pickup amplitudes (CosineOscillator, approximate), Q15 */
    uint32_t silent_;              /* samples since the output was last above the noise floor */
};

/* Rings' Part, modal model: 1, 2 or 4 voices taking the strikes in turn, a limiter on the pair of outputs. */
class RingsModal {
 public:
    void Init();
    void set_polyphony(int32_t p);                 /* 1, 2 or 4 */
    /* A note-on: the next voice (Rings' round robin) takes MIDI note `note` (+ fine, 1/128 semitone) and is struck
     * at velocity 1..127. */
    void Strike(uint32_t note, int32_t fine, uint32_t velocity);
    void set_fine(int32_t fine) { fine_ = fine; }
    bool sounding() const;
    /* Adds n stereo frames: L the odd modes, R the even (Rings' OUT and AUX), Q15 times level Q15. */
    void Render(const RingsPatch& patch, int32_t level, int32_t* out, size_t n);

 private:
    ModalVoice voice_[kMaxPolyphony];
    uint8_t note_[kMaxPolyphony];
    int32_t polyphony_, active_, fine_;
    int32_t peak_;                 /* the limiter's envelope, Q24 */
    int32_t odd_[32], even_[32];   /* one sub-block */
};

}  // namespace fom

#endif
