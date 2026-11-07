/* SPDX-License-Identifier: MIT
 *
 * Adapter between Mutable Instruments' Braids macro-oscillator and the FM-1.
 *
 * This file is ours. The DSP it drives is Emilie Gillet's, vendored unmodified
 * under MIT in dsp/mi/braids (see dsp/mi/PROVENANCE.md). Keeping the adapter
 * separate means the vendored tree stays byte-identical to upstream and can be
 * re-pulled without losing our changes.
 *
 * Two mismatches to bridge:
 *
 *   1. SAMPLE RATE. Braids runs at 96 kHz; the FM-1 at 44.1 kHz. See the note
 *      on kPitchCorrection below -- for the analog-oscillator shapes this is
 *      exactly a pitch offset and needs no regenerated tables.
 *
 *   2. BLOCK SIZE. MacroOscillator::Render writes through a private 24-sample
 *      temp_buffer_, so it must never be asked for more than 24 samples. The
 *      FM-1's audio ISR hands us 256 frames. Render() below buffers across
 *      calls so callers can ask for any length.
 */
#ifndef FM1_BRAIDS_H
#define FM1_BRAIDS_H

#include <stdint.h>
#include <stddef.h>

#include "braids/macro_oscillator.h"
#include "braids/settings.h"

namespace fm1 {

/* How many samples to ask MacroOscillator::Render for at a time.
 *
 * Braids uses 24, the size of its private temp_buffer_; more than that
 * overruns it. We use 11, and the reason is a rate bug that regenerated
 * tables cannot fix.
 *
 * Some of Braids' processing runs ONCE PER RENDER CALL rather than per
 * sample. The clearest case is the struck-bell decay, which sits outside the
 * sample loop in RenderStruckBell:
 *
 *     if (parameter_[0] < 32000)
 *         for (i) partial_amplitude[i] = partial_amplitude[i] * decay >> 16;
 *     while (size--) { ... }
 *
 * So the decay rate is per BLOCK. At 96 kHz, 24-sample blocks give 4000
 * decay steps a second. At 44.1 kHz the same 24 samples last 2.18x longer, so
 * the bell rings 2.18x too long -- measured as a 51 dB mean envelope error
 * against the reference, with the tail stretching from 0.75 s to 1.6 s.
 *
 *     24 * 44100 / 96000 = 11.025
 *
 * Rendering 11 samples at a time restores 4009 blocks a second, within 0.2%
 * of Braids' own rate, so every block-rate process lands at the right
 * wall-clock speed -- not just the bell. The per-sample work is unchanged;
 * only the call overhead rises.
 *
 * THE CHUNK MUST BE EVEN. RenderStruckBell runs at half rate and interpolates:
 *
 *     while (size--) {
 *         *buffer++ = (out + previous_sample) >> 1;
 *         *buffer++ = out; size--;
 *     }
 *
 * Two samples written per iteration, size decremented twice. An odd size
 * makes the inner size-- underflow size_t, and the loop runs away writing
 * past the buffer -- caught here as a stack-buffer-overflow under
 * AddressSanitizer after trying 11.
 *
 * 11.025 is not even, so the two chunk sizes alternate, 12 then 10, averaging
 * exactly 11: 44100/11 = 4009 calls a second, within 0.2% of Braids' 4000.
 *
 * Verified against a 96 kHz reference build: scripts/compare-braids-ref.sh.
 * That reference must run Braids as upstream does -- 24 samples at 96 kHz --
 * so it overrides this to a fixed 24. Without the override both sides would
 * shift together and the comparison would be blind to the very bug it exists
 * to catch. */
#ifndef FM1_BRAIDS_CHUNK          /* fixed size; the reference build sets 24 */
#define FM1_BRAIDS_CHUNK 0        /* 0 = alternate 12/10 */
#endif

const size_t kBraidsChunkA   = FM1_BRAIDS_CHUNK ? FM1_BRAIDS_CHUNK : 12;
const size_t kBraidsChunkB   = FM1_BRAIDS_CHUNK ? FM1_BRAIDS_CHUNK : 10;
const size_t kBraidsChunkMax = kBraidsChunkA > kBraidsChunkB
                             ? kBraidsChunkA : kBraidsChunkB;

/* Pitch is in 1/128 of a semitone throughout Braids, so MIDI note 60 is 7680. */
const int16_t kPitchPerSemitone = 128;

/* --- the sample-rate correction: none needed ------------------------------
 *
 * Braids ships tables generated for 96 kHz. We generate our own at 44.1 kHz
 * instead -- scripts/gen-braids-resources.sh re-runs MI's own generators with
 * `sample_rate = 44100`, so every rate-dependent table is correct at source:
 *
 *   oscillator_increments, oscillator_delays   pitch
 *   svf_cutoff / damp / scale                  filter cutoffs
 *   resonator_coefficient / resonator_scale    resonator tuning
 *   bowing_* / blowing_* envelopes             attack and decay times
 *   granular_envelope_rate                     grain rate
 *
 * That pipeline is validated: regenerated at 96000 it reproduces upstream's
 * resources.cc with all 45 data tables identical (docs/BRAIDS_PORT.md).
 *
 * HISTORY -- this used to be +1724 (13.4676 semitones), exploiting the fact
 * that replaying 96 kHz increments at 44.1 kHz is exactly a transposition.
 * That fixed PITCH for every shape but left everything calibrated in seconds
 * or absolute Hz running 96000/44100 = 2.18x fast, so the physical models,
 * filtered and granular shapes were in tune but wrong in character. With the
 * tables regenerated there is nothing left to correct. */
const int16_t kPitchCorrection = 0;

/* Shapes whose character depends on a table calibrated in seconds or absolute
 * Hz -- the physical models, filtered, vowel and granular ones.
 *
 * With dsp/mi/braids/resources.cc regenerated at 44.1 kHz these are all
 * correct, and this predicate should report nothing. It is kept because it
 * names exactly which shapes would silently regress if the vendored tables
 * were ever replaced with upstream's 96 kHz ones: everything would stay in
 * tune, and only these would go wrong, which is a hard failure to spot by ear.
 *
 * dsp/tests/test_braids.c asserts the tables are the 44.1 kHz ones. */
inline bool shape_is_rate_calibrated(braids::MacroOscillatorShape s)
{
    switch (s) {
    case braids::MACRO_OSC_SHAPE_BOWED:
    case braids::MACRO_OSC_SHAPE_BLOWN:
    case braids::MACRO_OSC_SHAPE_FLUTED:
    case braids::MACRO_OSC_SHAPE_PLUCKED:
    case braids::MACRO_OSC_SHAPE_STRUCK_BELL:
    case braids::MACRO_OSC_SHAPE_STRUCK_DRUM:
    case braids::MACRO_OSC_SHAPE_KICK:
    case braids::MACRO_OSC_SHAPE_SNARE:
    case braids::MACRO_OSC_SHAPE_CYMBAL:
    case braids::MACRO_OSC_SHAPE_VOWEL:
    case braids::MACRO_OSC_SHAPE_VOWEL_FOF:
    case braids::MACRO_OSC_SHAPE_VOSIM:
    case braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_LP:
    case braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_PK:
    case braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_BP:
    case braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_HP:
    case braids::MACRO_OSC_SHAPE_FILTERED_NOISE:
    case braids::MACRO_OSC_SHAPE_TWIN_PEAKS_NOISE:
    case braids::MACRO_OSC_SHAPE_CLOCKED_NOISE:
    case braids::MACRO_OSC_SHAPE_GRANULAR_CLOUD:
    case braids::MACRO_OSC_SHAPE_PARTICLE_NOISE:
        return true;
    default:
        return false;
    }
}

/* True when TIMBRE selects from a QUANTISED table rather than varying
 * something continuously. On these shapes TIMBRE is structural, not timbral:
 *
 *   TRIPLE_*      intervals[parameter >> 9] -- a table of SEMITONE intervals
 *                 for the second and third oscillators. TIMBRE is a harmony
 *                 control; the shape is a chord generator.
 *   FM family     lut_fm_frequency_quantizer -- quantised frequency ratios
 *   WAVE*         wave_line[parameter >> 9] -- discrete wavetable selection
 *
 * Modulating TIMBRE on these steps between entries instead of gliding, so a
 * slow LFO reads as a voice lurching through semitones rather than breathing.
 * Found by ear on TRIPLE_SAW, where one of the three oscillators walked whole
 * semitones over about 15 seconds. */
inline bool shape_timbre_is_stepped(braids::MacroOscillatorShape s)
{
    switch (s) {
    case braids::MACRO_OSC_SHAPE_TRIPLE_SAW:
    case braids::MACRO_OSC_SHAPE_TRIPLE_SQUARE:
    case braids::MACRO_OSC_SHAPE_TRIPLE_TRIANGLE:
    case braids::MACRO_OSC_SHAPE_TRIPLE_SINE:
    case braids::MACRO_OSC_SHAPE_TRIPLE_RING_MOD:
    case braids::MACRO_OSC_SHAPE_FM:
    case braids::MACRO_OSC_SHAPE_FEEDBACK_FM:
    case braids::MACRO_OSC_SHAPE_CHAOTIC_FEEDBACK_FM:
    case braids::MACRO_OSC_SHAPE_WAVETABLES:
    case braids::MACRO_OSC_SHAPE_WAVE_MAP:
    case braids::MACRO_OSC_SHAPE_WAVE_LINE:
    case braids::MACRO_OSC_SHAPE_WAVE_PARAPHONIC:
        return true;
    default:
        return false;
    }
}

class BraidsVoice {
 public:
    void Init();

    void set_shape(braids::MacroOscillatorShape shape) { osc_.set_shape(shape); }

    /* MIDI note in 1/128 semitone (note 60 == 60 * 128), corrected for the
     * FM-1's sample rate on the way in. */
    void set_pitch(int16_t pitch) { osc_.set_pitch((int16_t)(pitch + kPitchCorrection)); }

    /* TIMBRE and COLOR, Braids' two macro parameters, 0..32767. */
    void set_parameters(int16_t timbre, int16_t color) { osc_.set_parameters(timbre, color); }

    /* Excite the shape. 14 of Braids' 34 render paths consume a strike flag
     * and are silent or static without one -- the plucked and struck models,
     * the bowed/blown/fluted excitations, the vowel and noise shapes. Wire
     * MIDI note-on to this. Harmless for the shapes that ignore it. */
    void Strike() { osc_.Strike(); }

    /* Any number of samples; chunked to kBraidsChunk internally. */
    void Render(int16_t *out, size_t n);

 private:
    braids::MacroOscillator osc_;
    int16_t buf_[kBraidsChunkMax];
    uint8_t sync_[kBraidsChunkMax];  /* always zero: no hard sync input */
    size_t  avail_;                  /* samples left in buf_ */
    size_t  pos_;
    size_t  next_;                   /* chunk for the next refill, A or B */
};

/* MIDI note number -> Braids pitch. */
inline int16_t pitch_from_midi(int note) { return (int16_t)(note * kPitchPerSemitone); }

}  // namespace fm1

#endif  /* FM1_BRAIDS_H */
