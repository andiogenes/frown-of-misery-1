/* SPDX-License-Identifier: MIT
 *
 * Frown of Misery's sound engine, as the C firmware sees it.
 *
 * Today: four paraphonic Braids voices sharing one shape, TIMBRE and COLOR, each with its own pitch and a
 * click-free amplitude glide (attack / release). Rings and Clouds will join behind this same C interface.
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

void fom_init(void);

/* The shared sound: shape 0..FM1_NSHAPES-1 (dsp/engine/fm1_shapes.h), timbre and color 0..32767. */
void fom_set_shape(uint32_t shape);
void fom_set_timbre(uint32_t timbre);
void fom_set_color(uint32_t color);
/* The envelope: attack and release times as an index 0..FOM_ENV_MAX, a full-scale sweep taking 2^index frames
 * (0: at once, 10: 23 ms, 15: 0.74 s, 20: 24 s). fom_env_ms() says how long an index is. */
#define FOM_ENV_MAX 20u
void fom_set_env(uint32_t attack, uint32_t release);
uint32_t fom_env_ms(uint32_t index);
/* The pitch wheel, in 1/128 semitone (Braids' pitch unit): +-256 is +-2 semitones. */
void fom_set_bend(int32_t bend);

/* Voice v plays MIDI note `note` at level `gate` (Q15, 0 releases it); strike: excite it anew (a note-on). */
void fom_voice_play(uint32_t v, uint32_t note, int32_t gate, int strike);
void fom_voice_release(uint32_t v);
int  fom_voice_sounding(uint32_t v);       /* its gate is up, or its release still rings */

/* Adds n stereo frames (L, R interleaved, n <= FOM_BLOCK_MAX) to out. The audio ISR calls this. */
void fom_render_add(int32_t *out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* FOM_ENGINE_H */
