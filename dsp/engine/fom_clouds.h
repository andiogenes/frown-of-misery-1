/* SPDX-License-Identifier: MIT
 *
 * Clouds in fixed point, for a chip without an FPU: the granular mode, with Clouds' feedback, diffuser and reverb.
 *
 * Written from Mutable Instruments' Clouds (Emilie Gillet, MIT): clouds/dsp/granular_processor.cc, grain.h,
 * granular_sample_player.h, audio_buffer.h, fx/diffuser.h and fx/reverb.h, the float originals in dsp/ref/eurorack.
 * The algorithm is theirs, step for step, at Clouds' own 32 kHz, stereo, 16-bit (its default quality), with its buffer
 * sizes; CloudsFx carries it to and from the FM-1's 44.1 kHz. dsp/ref/clouds_matrix.py measures it against the float
 * original (dsp/ref/compare.py).
 *
 * Formats: audio Q24 (1.0 = 2^24) inside, as Clouds' floats; the recording buffers 16-bit, as Clouds'; knobs Q16.
 *
 * Where it parts from Clouds, for the CPU (the chip does Clouds' work in about 40 % of its audio time at the densest):
 * a grain's envelope is computed every 4 samples and drawn straight between (within 1e-4); the feedback's high-pass,
 * the diffuser and the reverb rest while they would change nothing (FEEDBACK 0, TEXTURE under 3/4, no reverb) and
 * start again from silence; set_max_grains() can hold the grains under Clouds' 32.
 */
#ifndef FOM_CLOUDS_H
#define FOM_CLOUDS_H

#include <stddef.h>
#include <stdint.h>

namespace fom {

const int32_t kCloudsBlock = 32;                   /* Clouds' block, at 32 kHz */
const int32_t kCloudsMaxGrainsHi = 32;             /* stereo at 16 bits: Clouds' count */
const int32_t kCloudsMaxGrainsLo = 46;             /* stereo at low fidelity: Clouds' 32 * 23 / 16 */
const int32_t kCloudsMaxGrains = kCloudsMaxGrainsLo;
const int32_t kCloudsBufferSamples = 32704;        /* a channel's buffer (Clouds' 65408 bytes), with the 8 of tail */
const int32_t kCloudsDiffuserSize = 2048;          /* Clouds' FxEngine sizes */
const int32_t kCloudsReverbSize = 16384;

struct CloudsPatch {               /* Clouds' knobs: 0..65535 (Q16), PITCH in semitones Q16 */
    uint32_t position, size, density, texture;
    uint32_t blend, spread, feedback, reverb;     /* BLEND's four: dry/wet, stereo spread, feedback, reverb */
    int32_t pitch;
    bool freeze;
};

struct CloudsFloat { int64_t m; int32_t e; };   /* a float32 emulated: m 2^e, |m| < 2^24 */

class CloudsGrain {
 public:
    void Init() { active_ = false; }
    bool active() const { return active_; }
    bool high() const { return high_; }
    void Start(int32_t pre_delay, int32_t buffer_size, int32_t start, int32_t width, int32_t phase_increment,
               uint32_t window_shape_q30, int32_t gain_l_q30, int32_t gain_r_q30, bool high);
    /* adds its part of a block (Q48, L R) */
    void OverlapAdd(const void* buf, bool ulaw, int32_t buffer_size, int64_t* acc, int32_t n);

 private:
    int32_t first_sample_, phase_, phase_increment_, pre_delay_;
    uint32_t env_phase_, env_increment_;           /* Q30: 2.0 = 2^31 */
    uint32_t remaining_;                           /* samples to go (float32's count of the phase's steps) */
    int32_t smoothness_;                           /* Q30 */
    uint32_t slope_;                               /* Q16 */
    int32_t gain_l_, gain_r_, cross_l_, cross_r_;  /* Q30: l -> L, r -> R, r -> L (1 - gain_r), l -> R (1 - gain_l) */
    bool active_, high_;                           /* high: Hermite and the smooth window; else linear */
};

class Clouds {
 public:
    /* buffer: 2 * kCloudsBufferSamples (L R interleaved); diffuser: kCloudsDiffuserSize; reverb: kCloudsReverbSize */
    void Init(int16_t* buffer, int32_t* diffuser, uint16_t* reverb);
    /* A block (n <= 32 frames at 32 kHz) of the module's input, Q24 L R: writes wet[] (Q24 L R), Clouds' output
     * before its dry/wet mix, times its post gain (1.2) and the wet side of the crossfade at patch.blend, and
     * SoftLimit'ed as its output stage does: what the module puts out, minus the dry part. */
    void Process(const CloudsPatch& patch, const int32_t* in, int32_t* wet, int32_t n);
    /* The module's whole Process (n <= 32): 16-bit in and out, the dry/wet mix and SoftConvert. The reference
     * harness (dsp/ref/port_clouds.cc). */
    void ProcessShort(const CloudsPatch& patch, const int16_t* in, int16_t* out, int32_t n);
    /* The most grains at once: Clouds' 32 (the default), fewer where the CPU is short (4..32). The density scales
     * with it, as Clouds' own count changes with its quality setting. */
    void set_max_grains(int32_t n);
    /* Clouds' low fidelity (LO): 8-bit mu-law buffers (4 s) and the grains at 16 kHz, as on the module; here the
     * diffuser and the reverb run at 16 kHz too (their lines halved: the same times), so all of it costs about half.
     * Blocks are then 16 samples at 16 kHz. Changing it starts from silence, as Clouds resets its buffers. */
    void set_lofi(bool lofi);
    bool lofi() const { return lofi_; }

 private:
    void Reset();
    int32_t max_grains_for_quality() const;
    int32_t buffer_size() const { return lofi_ ? 2 * kCloudsBufferSamples - 8 : kCloudsBufferSamples - 8; }
    void Run(const CloudsPatch& patch, const int32_t* in, int32_t n);   /* out_[]: Clouds' out_ after the reverb */
    void WriteBuffer(const int32_t* in, int32_t n, bool write);
    void Play(const CloudsPatch& patch, int32_t n);
    void Diffuse(int32_t amount_q30, int32_t n);
    void Reverb(int32_t amount_q30, int32_t time_q30, int32_t lp_q30, int32_t n);
    int32_t ReverbLfo(int32_t i, bool next);

    int16_t* buf_;                                 /* the recording, L R interleaved (Clouds keeps two) */
    int32_t* diffuser_;
    uint16_t* reverb_;
    /* the recording (AudioBuffer) */
    int32_t write_head_, crossfade_counter_;
    int16_t tail_[2][256];
    /* the grains (GranularSamplePlayer) */
    CloudsGrain grains_[kCloudsMaxGrains];
    int32_t max_grains_;
    bool lofi_;
    CloudsFloat num_grains_;                       /* the smoothed count */
    int32_t gain_normalization_;                   /* Q29 */
    CloudsFloat hint_;                             /* the grain size hint, a float32 as Clouds keeps it */
    struct {                                       /* Play's float32 arithmetic, kept while its inputs hold */
        uint32_t density, texture;
        int32_t max;
        CloudsFloat hint, ov, ws, target, xfade;
        uint64_t prob_ceil, space_ceil;
        uint32_t window_shape;
    } pc_;
    int32_t grain_rate_phasor_;
    uint32_t rng_;
    /* the processor */
    int32_t freeze_lp_;                            /* Q30 */
    int32_t dry_wet_;                              /* Q30 */
    int32_t fb_g_, fb_h_;                          /* the feedback's high-pass (Svf, Q 1): g Q31, h Q30 */
    uint32_t fb_feedback_;                         /* the knob they are for */
    int32_t fb_s1_[2], fb_s2_[2];                  /* Q24 */
    int32_t fb_[kCloudsBlock * 2];                 /* last block's output, fed back */
    int32_t in_[kCloudsBlock * 2], out_[kCloudsBlock * 2];
    int64_t acc_[kCloudsBlock * 2];                /* the grains' sum, Q48 */
    /* the diffuser's and the reverb's write pointers, the reverb's LFOs (float32's values, Q48) and low-passes */
    int32_t diffuser_wp_, reverb_wp_;
    bool diffuser_rest_, reverb_rest_;             /* resting: nothing to do (see Run) */
    int64_t lfo_y0_[2], lfo_y1_[2];
    int32_t lfo_value_[2];                         /* Q30 */
    int32_t lp_decay_[2];                          /* Q24 */
};

/* Clouds on the FM-1's 44.1 kHz: resampled to 32 kHz and back (polyphase, 16 taps), the dry signal mixed at 44.1 kHz
 * (it never goes through the resamplers). The wet signal is about 2 ms late. */
class CloudsFx {
 public:
    void Init(int16_t* buffer, int32_t* diffuser, uint16_t* reverb);
    /* n stereo frames, Q15 (L R) in io[], replaced by Clouds' output at patch */
    void Process(const CloudsPatch& patch, int32_t* io, size_t n);
    void set_max_grains(int32_t n) { clouds_.set_max_grains(n); }
    void set_lofi(bool lofi);                      /* (see Clouds::set_lofi) */

 private:
    Clouds clouds_;
    int16_t down_hist_[64];                        /* the last 16 inputs at 44.1 kHz, L R, twice (a window never wraps) */
    int32_t down_pos_, down_phase_;                /* where the history ends; the next output's phase / 320 */
    int32_t down_count_;                           /* inputs until the next 32 kHz output */
    int32_t up_hist_[64];                          /* the last 16 wet outputs at 32 kHz, Q15, L R, twice */
    int32_t up_pos_, up_phase_;
    int32_t block_in_[kCloudsBlock * 2], block_wet_[kCloudsBlock * 2];
    int32_t block_fill_;
    int32_t fifo_[128 * 2];                        /* the wet signal at 44.1 kHz, Q15 */
    uint32_t fifo_read_, fifo_write_;
    int32_t fade_out_;                             /* Q30, the dry side of the crossfade, smoothed */
    bool lofi_;
};

}  // namespace fom

#endif
