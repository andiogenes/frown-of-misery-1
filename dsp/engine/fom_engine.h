/* SPDX-License-Identifier: MIT
 *
 * Frown of Misery's sound engine, as the C firmware sees it.
 *
 * One engine sounds at a time (fom_set_engine), Braids or Rings, and Clouds after it if asked (fom_set_clouds).
 * Every engine is driven the same way: notes on its voices, and parameters by number (FOM_P_*), each an integer in its own range
 * (fom_param_info). The firmware's UI is built from that table, so a new engine needs no new UI code.
 *
 * Braids: four paraphonic voices sharing one shape, TIMBRE and COLOR, each with its own pitch, a click-free
 * amplitude glide (attack / release) and a pitch glide.
 * Rings: the modal resonator, the sympathetic strings or the string (dsp/engine/fom_rings.cc), 1, 2 or 4 voices
 * taking the strikes in turn; a note-on strikes (fom_strike), there is no note-off: DAMPING decides how long it
 * rings. Braids > Rings: Braids' voices into Rings' audio input instead of its own exciter; a note-on plays Braids
 * and strikes Rings, which then only picks the voice and its note (Rings' V/OCT and STRUM with the input patched).
 * Clouds (dsp/engine/fom_clouds.cc): its granular mode, as an effect after either engine (fom_set_clouds).
 *
 * Threading: fom_render_add() runs in the audio ISR. Everything else is called from the main loop and only
 * stores 32-bit values the ISR reads, written in an order that is safe to see half-done (the pitch before the
 * gate), the same rule Dinghy's sine uses.
 */
#ifndef FOM_ENGINE_H
#define FOM_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOM_NV 4u              /* voices */
#define FOM_BLOCK_MAX 256u     /* the most frames fom_render_add() is ever asked for at once */

enum {                         /* engines */
    FOM_E_BRAIDS,
    FOM_E_RINGS,
    FOM_E_BRAIDS_RINGS,        /* Braids into Rings' audio input (its exciter): a note plays Braids and picks Rings' */
    FOM_NENGINES
};

enum {                         /* parameters (all engines; each engine reads the ones it uses) */
    FOM_P_SHAPE,               /* Braids: 0..47, dsp/engine/fm1_shapes.h */
    FOM_P_TIMBRE,              /* 0..32767 */
    FOM_P_COLOR,               /* 0..32767 */
    FOM_P_FINE,                /* fine tune, -64..63 in 1/128 semitone */
    FOM_P_LEVEL,               /* 0..32767, the engine's output level */
    FOM_P_ATTACK,              /* envelope times, 0..FOM_ENV_MAX: a full sweep takes 2^index frames */
    FOM_P_RELEASE,
    FOM_P_GLIDE,               /* pitch glide, 0 (off)..FOM_ENV_MAX: a time constant of 2^index frames */
    FOM_P_STRUCTURE,           /* Rings' four knobs, 0..32767 */
    FOM_P_BRIGHTNESS,
    FOM_P_DAMPING,
    FOM_P_POSITION,
    FOM_P_POLY,                /* Rings' polyphony: 0, 1, 2 = 1, 2, 4 voices */
    FOM_P_MODEL,               /* Rings' resonator: 0 modal, 1 sympathetic strings, 2 string */
    FOM_P_C_POSITION,          /* Clouds' knobs, 0..32767 */
    FOM_P_C_SIZE,
    FOM_P_C_DENSITY,
    FOM_P_C_TEXTURE,
    FOM_P_C_BLEND,             /* BLEND's four: dry/wet, stereo spread, feedback, reverb */
    FOM_P_C_SPREAD,
    FOM_P_C_FEEDBACK,
    FOM_P_C_REVERB,
    FOM_P_C_PITCH,             /* Clouds' PITCH, -24..24 semitones */
    FOM_P_C_QUALITY,
    FOM_P_R_LEVEL,             /* Rings' output level, 0..32767 (Braids' LEVEL drives Rings in BRAIDS > RINGS) */           /* Clouds' quality: 0 HI (16 bits, 32 kHz), 1 LO (Clouds' low fidelity: 8 bits, 16 kHz) */
    FOM_NPARAM
};
#define FOM_ENV_MAX 20u

/* How a parameter is shown and stored. */
enum { FOM_K_BAR, FOM_K_TIME, FOM_K_CENTS, FOM_K_INDEX, FOM_K_POLY, FOM_K_SEMI, FOM_K_QUALITY };
typedef struct {
    const char *name;          /* <= 6 characters */
    int16_t min, max, def;
    uint8_t kind;              /* FOM_K_*: a bar, a time (fom_env_ms), cents (value * 100 / 128), a plain index,
                                  polyphony (1 << value), semitones, HI / LO */
} fom_param_info_t;
const fom_param_info_t *fom_param_info(uint32_t p);

void fom_init(void);
void fom_set_engine(uint32_t e);
/* Clouds (the granular mode) after the engine, or not; and its FREEZE (the buffer stops recording) */
void fom_set_clouds(int on);
void fom_set_freeze(int on);
/* The audio interrupt's last load, per mille of its time (the app measures it): over 85 %, Clouds plays fewer grains
 * at once until it calms down. Called from the interrupt, after fom_render_add. */
void fom_set_load(uint32_t permille);
/* Clouds runs LO whatever FOM_P_C_QUALITY says after Rings' sympathetic strings (HI does not fit there) */
int fom_clouds_lofi(void);                 /* LO now, asked for or not */
/* The timer the engine reads to tell Clouds' share of the interrupt's time (fom_clouds_ticks: in the last call) */
void fom_set_ticks(uint32_t (*ticks)(void));
uint32_t fom_clouds_ticks(void);
void fom_set_param(uint32_t p, int32_t v);  /* clamped to the parameter's range */
uint32_t fom_env_ms(uint32_t index);        /* how long an envelope or glide index is, in ms */
/* The pitch wheel, in 1/128 semitone (Braids' pitch unit): +-256 is +-2 semitones. */
void fom_set_bend(int32_t bend);

/* Voice v plays MIDI note `note` at level `gate` (Q15, 0 releases it). strike: excite it anew (a note-on);
 * 0 for legato (the pitch glides there, nothing restarts). */
void fom_voice_play(uint32_t v, uint32_t note, int32_t gate, int strike);
void fom_voice_release(uint32_t v);
int  fom_voice_sounding(uint32_t v);       /* its gate is up, or its release still rings */

/* Rings: strike MIDI note `note` at velocity 1..127 on the next voice. Up to 8 strikes are queued between two
 * audio blocks (a chord); more are dropped. */
void fom_strike(uint32_t note, uint32_t velocity);
int fom_sounding(void);                    /* something still rings, Clouds' tail too (settings wait for silence) */

/* Adds n stereo frames (L, R interleaved, n <= FOM_BLOCK_MAX) to out. The audio ISR calls this. */
void fom_render_add(int32_t *out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* FOM_ENGINE_H */
