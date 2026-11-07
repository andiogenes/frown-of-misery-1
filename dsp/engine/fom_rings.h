/* SPDX-License-Identifier: MIT
 *
 * Rings in fixed point, for a chip without an FPU: the modal resonator, the sympathetic strings and the string.
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

/* ---- Rings' string models: sympathetic strings and the string (Karplus-Strong with dispersion) ----------------
 * After rings/dsp/string.cc, plucker.h and the string path of part.cc. These run on Rings' own 24-sample block grid
 * (parameters, glides and the sympathetic feed are per block there), and draw from one copy of stmlib's Random in
 * Rings' order, so a render matches the float original sample for sample. */

const int32_t kNumStrings = 2 * kMaxPolyphony;     /* 8 */
const int32_t kStringDelay = 2048;                 /* Rings' kDelayLineSize */
const int32_t kRingsBlock = 24;                    /* Rings' kMaxBlockSize */

/* One string: a delay line read with Hermite interpolation, the FIR and IIR damping filters, a comb tap for the
 * pickup position; with dispersion, an allpass on a second line, noise on the delay and the curved bridge. */
class RingsString {
 public:
    void set_lines(int32_t* line, int32_t* stretch) { line_ = line; stretch_ = stretch; }   /* before Init */
    void Init(bool dispersion);
    void set_frequency(uint32_t f_q32) { frequency_ = f_q32; }
    /* frequency_ += glide (f - frequency_), glide Q30: the sympathetic strings' slew towards their notes */
    void set_frequency(uint32_t f_q32, int32_t glide_q30);
    /* dispersion -1..1, the others 0..1: Q16 (dispersion signed) */
    void set_params(int32_t dispersion, uint32_t brightness, uint32_t damping, uint32_t position) {
        dispersion_ = dispersion; brightness_ = brightness; damping_ = damping; position_ = position;
    }
    /* Adds a block (n <= 24) to out[] and aux[], Q24; in[] Q24. */
    void Process(const int32_t* in, int32_t* out, int32_t* aux, int32_t n);

 private:
    template <bool kDispersion> void ProcessInternal(const int32_t* in, int32_t* out, int32_t* aux, int32_t n);
    int32_t ReadHermite(uint32_t delay_q16) const;
    int32_t Read(uint32_t delay_q16) const;

    uint32_t frequency_;           /* Q32 */
    int32_t dispersion_;           /* Q16, -1..1 */
    uint32_t brightness_, damping_, position_;   /* Q16 */
    /* the per-sample interpolations' states (ParameterInterpolator), with 8 more fraction bits than the values:
     * an integer step truncated by the division left them short of their targets, for good */
    int64_t delay_;                /* Q24 (samples) */
    int64_t clamped_position_;     /* Q24 */
    int64_t previous_dispersion_;  /* Q24 */
    int64_t previous_damping_compensation_;      /* Q30 */
    bool dispersion_on_;
    int32_t dispersion_noise_;     /* Q24 */
    uint64_t src_phase_;           /* Q32, float32's grid: notes under fs / 2044 run the string slower */
    int32_t out_sample_[2], aux_sample_[2];      /* Q24 */
    int32_t curved_bridge_;        /* Q24 */
    /* DampingFilter: damping and brightness glide over the block, Q30 */
    int32_t fir_x_, fir_x__, fir_brightness_, fir_brightness_inc_, fir_damping_, fir_damping_inc_;
    /* the IIR damping low-pass (stmlib Svf, Q 0.5): g, r+g Q19, h Q30; states Q24 */
    int32_t iir_g_, iir_rg_, iir_h_, iir_s1_, iir_s2_;
    int32_t dc_x_;                 /* the dispersion path's DC blocker: input Q24, state Q40 (no deadband) */
    int64_t dc_y_;
    uint32_t write_;               /* the delay lines' write position */
    int32_t* line_;                /* kStringDelay samples, Q24 (in .pool, given by the part) */
    int32_t* stretch_;             /* kStringDelay / 2 samples, Q24, or null (no dispersion) */
    uint32_t stretch_write_;
};

/* The internal exciter of the string models: a burst of noise as long as the comb's period, through a comb (the
 * pluck's position) and a low-pass. */
class RingsPlucker {
 public:
    void Init();
    void Trigger(uint32_t f_q32, uint32_t cutoff_q32, uint32_t position_q16, int32_t level_q15);
    void Process(int32_t* out, int32_t n);        /* overwrites out[] (n <= 24), Q24 */
 private:
    int32_t comb_[256];            /* Q24 */
    uint32_t comb_write_;
    uint32_t remaining_;           /* samples of noise left */
    uint32_t comb_period_;         /* Q16 */
    int32_t comb_gain_;            /* Q30 */
    int32_t level_;                /* Q15 */
    int32_t g_, h_;                /* the low-pass (FREQUENCY_DIRTY, Q 1), Q24 */
    int64_t s1_, s2_;
};

/* Part for the string models: polyphony 1, 2 or 4 (8, 4 or 2 strings a voice for the sympathetic model, 1 for the
 * string), the round robin, the sympathetic tuning from STRUCTURE, the limiter. */
class RingsStrings {
 public:
    enum Model { SYMPATHETIC = 1, STRING = 2 };   /* Rings' RESONATOR_MODEL_* */
    void Init(int32_t* lines, int32_t* stretch);   /* kNumStrings * kStringDelay, kMaxPolyphony * kStringDelay / 2 */
    void set_model(int32_t model);
    void set_polyphony(int32_t p);
    void set_fine(int32_t fine) { fine_ = fine; }
    /* A note-on, taken at the start of the next 24-sample block (as Rings takes a strum). */
    void Strike(uint32_t note, int32_t fine, uint32_t velocity);
    bool sounding() const { return silent_ < 44100u; }
    /* Adds n stereo frames, Q15 times level Q15 (any n: the 24-sample grid is kept across calls). */
    void Render(const RingsPatch& patch, int32_t level, int32_t* out, size_t n);

 private:
    void Configure();
    void RenderBlock(const RingsPatch& patch);
    void ComputeSympatheticNotes(int32_t note_q16, uint32_t parameter_q16, int32_t* dest, int32_t n) const;

    RingsString string_[kNumStrings];
    RingsPlucker plucker_[kMaxPolyphony];
    int32_t lfo_c_[kNumStrings];                                                 /* CosineOscillator: c, Q29 */
    int64_t lfo_y0_[kNumStrings], lfo_y1_[kNumStrings];                          /* its state, Q48, float's grid */
    int32_t dc_x_[kMaxPolyphony];                                                /* the input's DC blocker, */
    int64_t dc_y_[kMaxPolyphony];                                                /* its state Q40 */
    int32_t* lines_;
    int32_t* stretch_;
    int32_t model_, polyphony_, active_, fine_;
    bool dirty_;
    int32_t note_[kMaxPolyphony];  /* MIDI, Q16: idle voices keep Rings' defaults (C0 + 0.05 a voice) */
    bool strum_pending_;
    uint8_t strum_note_, strum_vel_;
    int32_t peak_;                 /* the limiter, Q24 */
    int32_t out_[kRingsBlock * 2]; /* the current block, Q15, L R */
    int32_t pos_;                  /* frames of it already handed out (kRingsBlock: none left) */
    uint32_t silent_;              /* samples since the output was last above -96 dB */
};

}  // namespace fom

#endif
