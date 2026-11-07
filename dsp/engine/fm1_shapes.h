/* SPDX-License-Identifier: MIT
 *
 * Names and categories for Braids' 48 oscillator shapes.
 *
 * Braids itself puts these on a four-character LED display and gets away with
 * it, because its users learn the codes over months. We have a 240x240 screen
 * and no such excuse, so the names are spelled out and the shapes are grouped
 * -- 48 on a bare encoder is thirty detents between a saw stack and a bowed
 * string, which is unusable. SHIFT + ALGORITHM jumps whole categories and
 * turns that into three.
 *
 * The order is braids::MacroOscillatorShape in dsp/mi/braids/settings.h and
 * must stay in step with it: the index IS the parameter value. That header is
 * C++ and namespaced, so the firmware's C UI cannot read it -- hence this.
 * A test checks the count; nothing can check the spelling, so these were
 * transcribed against the enum rather than from memory.
 */
#ifndef FM1_SHAPES_H
#define FM1_SHAPES_H

#include <stdint.h>

#define FM1_NSHAPES 48

static const char *const FM1_SHAPE_NAME[FM1_NSHAPES] = {
    /*  0 */ "CSAW",            "MORPH",           "SAW SQUARE",
    /*  3 */ "SINE TRIANGLE",   "BUZZ",
    /*  5 */ "SQUARE SUB",      "SAW SUB",         "SQUARE SYNC",
    /*  8 */ "SAW SYNC",
    /*  9 */ "TRIPLE SAW",      "TRIPLE SQUARE",   "TRIPLE TRIANGLE",
    /* 12 */ "TRIPLE SINE",     "TRIPLE RING MOD",
    /* 14 */ "SAW SWARM",       "SAW COMB",        "TOY",
    /* 17 */ "DIGITAL FILT LP", "DIGITAL FILT PK", "DIGITAL FILT BP",
    /* 20 */ "DIGITAL FILT HP",
    /* 21 */ "VOSIM",           "VOWEL",           "VOWEL FOF",
    /* 24 */ "HARMONICS",
    /* 25 */ "FM",              "FEEDBACK FM",     "CHAOTIC FM",
    /* 28 */ "PLUCKED",         "BOWED",           "BLOWN",
    /* 31 */ "FLUTED",
    /* 32 */ "STRUCK BELL",     "STRUCK DRUM",     "KICK",
    /* 35 */ "CYMBAL",          "SNARE",
    /* 37 */ "WAVETABLES",      "WAVE MAP",        "WAVE LINE",
    /* 40 */ "WAVE PARAPHONIC",
    /* 41 */ "FILTERED NOISE",  "TWIN PEAKS",      "CLOCKED NOISE",
    /* 44 */ "GRANULAR CLOUD",  "PARTICLE NOISE",
    /* 46 */ "DIGITAL MOD",     "QUESTION MARK",
};

/* --- what TIMBRE and COLOR actually do -----------------------------------
 *
 * Braids has two knobs for forty-eight oscillators, and they mean something
 * different on every one of them. On the module you learn that from the
 * manual; here the screen can simply say it, the same way a slot's P1 reads
 * DRIVE or TUNE depending on its type.
 *
 * These were derived from the VENDORED SOURCE rather than from the Braids
 * manual -- dsp/mi/braids/macro_oscillator.cc, analog_oscillator.cc and
 * digital_oscillator.cc, which is what actually runs here. A manual can
 * describe a version we did not port; the renderer cannot. The derivation,
 * line by line and shape by shape, is docs/BRAIDS_SHAPES.md.
 *
 * Six characters, enforced by the same test that guards every other knob
 * label (dsp/tests/test_pages.c, FM1_LABEL_MAX).
 */
static const char *const FM1_SHAPE_TIMBRE[FM1_NSHAPES] = {
    /*  0 CSAW            */ "PW",
    /*  1 MORPH           */ "WAVE",
    /*  2 SAW SQUARE      */ "PW",
    /*  3 SINE TRIANGLE   */ "FOLD",
    /*  4 BUZZ            */ "HARM",
    /*  5 SQUARE SUB      */ "PW",
    /*  6 SAW SUB         */ "PW",
    /*  7 SQUARE SYNC     */ "SYNC",
    /*  8 SAW SYNC        */ "SYNC",
    /*  9 TRIPLE SAW      */ "NOTE 2",
    /* 10 TRIPLE SQUARE   */ "NOTE 2",
    /* 11 TRIPLE TRIANGLE */ "NOTE 2",
    /* 12 TRIPLE SINE     */ "NOTE 2",
    /* 13 TRIPLE RING MOD */ "MOD 1",
    /* 14 SAW SWARM       */ "DETUNE",
    /* 15 SAW COMB        */ "TUNE",
    /* 16 TOY             */ "RATE",
    /* 17 DIGITAL FILT LP */ "FREQ",
    /* 18 DIGITAL FILT PK */ "FREQ",
    /* 19 DIGITAL FILT BP */ "FREQ",
    /* 20 DIGITAL FILT HP */ "FREQ",
    /* 21 VOSIM           */ "FORM 1",
    /* 22 VOWEL           */ "VOWEL",
    /* 23 VOWEL FOF       */ "VOWEL",
    /* 24 HARMONICS       */ "PEAK",
    /* 25 FM              */ "INDEX",
    /* 26 FEEDBACK FM     */ "INDEX",
    /* 27 CHAOTIC FM      */ "INDEX",
    /* 28 PLUCKED         */ "DECAY",
    /* 29 BOWED           */ "BOW",
    /* 30 BLOWN           */ "BREATH",
    /* 31 FLUTED          */ "BREATH",
    /* 32 STRUCK BELL     */ "DECAY",
    /* 33 STRUCK DRUM     */ "DECAY",
    /* 34 KICK            */ "DECAY",
    /* 35 CYMBAL          */ "FREQ",
    /* 36 SNARE           */ "TONE",
    /* 37 WAVETABLES      */ "WAVE",
    /* 38 WAVE MAP        */ "MAP X",
    /* 39 WAVE LINE       */ "SCAN",
    /* 40 WAVE PARAPHONIC */ "WAVE",
    /* 41 FILTERED NOISE  */ "RESO",
    /* 42 TWIN PEAKS      */ "RESO",
    /* 43 CLOCKED NOISE   */ "LOOP",
    /* 44 GRANULAR CLOUD  */ "GRAIN",
    /* 45 PARTICLE NOISE  */ "DENSTY",
    /* 46 DIGITAL MOD     */ "RATE",
    /* 47 QUESTION MARK   */ "SPEED",
};

static const char *const FM1_SHAPE_COLOR[FM1_NSHAPES] = {
    /*  0 CSAW            */ "DEPTH",
    /*  1 MORPH           */ "FUZZ",
    /*  2 SAW SQUARE      */ "MIX",
    /*  3 SINE TRIANGLE   */ "MIX",
    /*  4 BUZZ            */ "DETUNE",
    /*  5 SQUARE SUB      */ "SUB",
    /*  6 SAW SUB         */ "SUB",
    /*  7 SQUARE SYNC     */ "MIX",
    /*  8 SAW SYNC        */ "MIX",
    /*  9 TRIPLE SAW      */ "NOTE 3",
    /* 10 TRIPLE SQUARE   */ "NOTE 3",
    /* 11 TRIPLE TRIANGLE */ "NOTE 3",
    /* 12 TRIPLE SINE     */ "NOTE 3",
    /* 13 TRIPLE RING MOD */ "MOD 2",
    /* 14 SAW SWARM       */ "HPF",
    /* 15 SAW COMB        */ "RESO",
    /* 16 TOY             */ "BITS",
    /* 17 DIGITAL FILT LP */ "SHAPE",
    /* 18 DIGITAL FILT PK */ "SHAPE",
    /* 19 DIGITAL FILT BP */ "SHAPE",
    /* 20 DIGITAL FILT HP */ "SHAPE",
    /* 21 VOSIM           */ "FORM 2",
    /* 22 VOWEL           */ "SHIFT",
    /* 23 VOWEL FOF       */ "SINGER",
    /* 24 HARMONICS       */ "WIDTH",
    /* 25 FM              */ "RATIO",
    /* 26 FEEDBACK FM     */ "RATIO",
    /* 27 CHAOTIC FM      */ "RATIO",
    /* 28 PLUCKED         */ "POSN",
    /* 29 BOWED           */ "POSN",
    /* 30 BLOWN           */ "BODY",
    /* 31 FLUTED          */ "JET",
    /* 32 STRUCK BELL     */ "DETUNE",
    /* 33 STRUCK DRUM     */ "TONE",
    /* 34 KICK            */ "TONE",
    /* 35 CYMBAL          */ "MIX",
    /* 36 SNARE           */ "SNAP",
    /* 37 WAVETABLES      */ "BANK",
    /* 38 WAVE MAP        */ "MAP Y",
    /* 39 WAVE LINE       */ "ROUGH",
    /* 40 WAVE PARAPHONIC */ "CHORD",
    /* 41 FILTERED NOISE  */ "MODE",
    /* 42 TWIN PEAKS      */ "PEAK 2",
    /* 43 CLOCKED NOISE   */ "STEPS",
    /* 44 GRANULAR CLOUD  */ "SPREAD",
    /* 45 PARTICLE NOISE  */ "SPREAD",
    /* 46 DIGITAL MOD     */ "DATA",
    /* 47 QUESTION MARK   */ "NOISE",
};

/* --- categories ----------------------------------------------------------
 *
 * Twelve groups, each a contiguous run; docs/UX_DESIGN.md section 5 lists
 * them. `first` is the index SHIFT + ALGORITHM lands on.
 *
 * The drum shapes are near-useless for drones but are NOT hidden. They live
 * in one category you skip in a single jump, which costs nothing and avoids a
 * "where did it go" problem later.
 */
#define FM1_NSHAPE_CATS 12

typedef struct {
    const char *name;
    uint8_t     first;          /* first shape index in this category */
} fm1_shape_cat_t;

static const fm1_shape_cat_t FM1_SHAPE_CAT[FM1_NSHAPE_CATS] = {
    { "ANALOG",    0  },
    { "SUB SYNC",  5  },
    { "TRIPLE",    9  },
    { "SWARM",     14 },
    { "FILTER",    17 },
    { "VOWEL",     21 },
    { "HARMONIC",  24 },
    { "PHYSICAL",  28 },
    { "STRUCK",    32 },
    { "WAVETABLE", 37 },
    { "NOISE",     41 },
    { "MISC",      46 },
};

/* Which category a shape belongs to. Linear over twelve entries, called once
 * per redraw -- a lookup table would be 48 bytes to save nothing. */
static inline uint8_t fm1_shape_cat(uint8_t shape)
{
    uint8_t c = FM1_NSHAPE_CATS - 1;
    while (c > 0 && shape < FM1_SHAPE_CAT[c].first) c--;
    return c;
}

/* --- quantised knobs -----------------------------------------------------
 *
 * On some shapes a knob indexes a TABLE rather than sweeping a range, so it
 * steps instead of gliding. The screen needs to know, because a knob that
 * jumps reads as a knob that is broken -- it has already caused exactly that
 * confusion once.
 *
 * This used to be per CATEGORY, which was wrong in both directions and was
 * caught by reading the renderers for docs/BRAIDS_SHAPES.md:
 *
 *   - HARMONICS (24) was marked stepped. Neither of its knobs steps: PEAK
 *     slides the loudest partial and WIDTH slides the skirt, both continuous
 *     (digital_oscillator.cc RenderHarmonics). It shared a category with the
 *     three FM shapes and inherited their badge.
 *   - TRIPLE RING MOD (13) was marked stepped AND chord. It is neither: both
 *     knobs are plain continuous pitch offsets for two ring modulators
 *     (RenderTripleRingMod), nothing is quantised and nothing is a chord.
 *   - WAVE MAP (38) and WAVE LINE (39) were marked stepped. Both crossfade
 *     between adjacent waves across the whole range.
 *   - WAVE PARAPHONIC (40) is the one actual chord generator outside the
 *     TRIPLE group -- its COLOR picks from a 17-entry chord table -- and was
 *     not marked as one.
 *
 * Which is the whole argument for deriving this from the renderers: a badge
 * nobody checked against the code was wrong on four shapes out of twelve.
 */
static inline int fm1_shape_timbre_stepped(uint8_t shape)
{
    return (shape >= 9 && shape <= 12);         /* TRIPLE intervals */
}

static inline int fm1_shape_color_stepped(uint8_t shape)
{
    return (shape >= 9  && shape <= 12) ||      /* TRIPLE intervals  */
           (shape >= 25 && shape <= 27) ||      /* FM ratios         */
            shape == 37 ||                      /* wavetable bank    */
            shape == 40 ||                      /* paraphonic chord  */
            shape == 43;                        /* clocked noise     */
}

static inline int fm1_shape_quantised(uint8_t shape)
{
    return fm1_shape_timbre_stepped(shape) || fm1_shape_color_stepped(shape);
}

/* On the TRIPLE shapes the voice is a CHORD GENERATOR: TIMBRE and COLOR set
 * the intervals of the second and third oscillators. WAVE PARAPHONIC is one
 * too, with four oscillators and a single knob choosing the chord. Invisible
 * from the panel, so the screen says so. */
static inline int fm1_shape_is_chord(uint8_t shape)
{
    return (shape >= 9 && shape <= 12) || shape == 40;
}

#endif /* FM1_SHAPES_H */
