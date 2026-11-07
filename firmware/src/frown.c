/* SPDX-License-Identifier: GPL-3.0-only */
/* Frown of Misery: Mutable Instruments' Braids (and, to come, Rings and Clouds) on the M-VAVE FM-1.
 * Built on Dinghy (zvenson), on Felucca's (Leo Kuroshita, Hügelton Instruments) and SLOOP's (isod89) platform.
 * This file is the app; the sound is dsp/engine/fom_engine.cc; everything below platform.c is Dinghy's, unchanged.
 *
 *   keys       play Braids, 4 voices paraphonic (one shape, one TIMBRE / COLOR, a pitch each); out over USB-MIDI too
 *   ALGORITHM  the shape, 48 of them
 *   KNOB 1, 2  TIMBRE, COLOR (what they do depends on the shape: the screen says)
 *   KNOB 3, 4  attack, release
 *   OCT- OCT+  the octave
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack, with their velocity; a MIDI clock shows its tempo
 *   SMK-37     the keys with velocity, the outer keys, the pads (white keys F3..G5), the pitch wheel (+-2 semitones)
 *
 * All of it is kept in flash, 2 s after the last change, when nothing sounds.
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "FROWN"                        /* the rescue and crash screens */
#define STORE_MAGIC 0x574F5246u                 /* "FROW": our settings, never another Dinghy fork's */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif
#include "fom_engine.h"
#include "fm1_shapes.h"

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
static void app_audio(int32_t *out, uint32_t n)
{
    fom_render_add(out, n);
}

/* ---- notes (main loop): which of the engine's voices plays what */
static struct { uint8_t note, on; uint32_t age; } voice[FOM_NV];
static uint32_t ages, last_note = 255;

static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    if (note > 127u || !vel) return;
    for (v = 0; v < FOM_NV; v++) {              /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == note && voice[v].on ? 0u                  /* the same note, held */
                       : voice[v].on ? 0x80000000u + voice[v].age                   /* held: the oldest goes */
                       : fom_voice_sounding(v) ? 2u : 1u;                           /* free: silent before releasing */
        if (score < best || (score == best && voice[v].age < voice[pick].age)) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].on = 1;
    voice[pick].age = ++ages;
    fom_voice_play(pick, note, (int32_t)(vel * 90u), 1);   /* 127 -> 11430: four voices stay under full scale */
    last_note = note;
}
static void stop(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < FOM_NV; v++)
        if (voice[v].on && voice[v].note == note) {
            voice[v].on = 0;
            fom_voice_release(v);
        }
}
static int quiet(void)                          /* nothing sounds, not even a release */
{
    uint32_t v;
    for (v = 0; v < FOM_NV; v++)
        if (fom_voice_sounding(v)) return 0;
    return 1;
}

/* ---- what survives a power-off */
static struct {
    uint8_t oct, ch, shape, atk, rel, pad[3];
    uint16_t timbre, color;
} set;
static void set_defaults(void)
{
    set.oct = 0; set.ch = 0;
    set.shape = 0;                              /* CSAW */
    set.timbre = 8192; set.color = 0;
    set.atk = 7; set.rel = 13;                  /* ~3 ms, ~190 ms */
}
static int set_valid(void)
{
    return (int8_t)set.oct >= -2 && (int8_t)set.oct <= 2 && set.ch <= 16u && set.shape < FM1_NSHAPES
        && set.atk <= FOM_ENV_MAX && set.rel <= FOM_ENV_MAX && set.timbre <= 32767u && set.color <= 32767u;
}
static void set_apply(void)
{
    fom_set_shape(set.shape);
    fom_set_timbre(set.timbre);
    fom_set_color(set.color);
    fom_set_env(set.atk, set.rel);
}

/* ---- the screen */
#define C_ACC RGB(255, 52, 52)                  /* a sore red */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define C_TRACK RGB(40, 40, 40)
#define KEY0 53u                                /* key 0 = F3 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, uint32_t v)
{
    char t[10];
    uint32_t i = 0;
    do { t[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i) *p++ = t[--i];
    return p;
}
static char *put_note(char *p, uint32_t note)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note > 127u) return put(p, "-");
    p = put(p, NAME[note % 12u]);
    if (note < 12u) return put(p, "-1");
    return put_u(p, note / 12u - 1u);
}

static void draw_shape(void)
{
    char s[32], *p = s;
    p = put_u(p, set.shape + 1u);
    p = put(p, "/48  ");
    p = put(p, FM1_SHAPE_NAME[set.shape]);
    *p = 0;
    draw_text_box(0, 48, 240, &FONT_S, s, C_WHITE, 1);
}

static void draw_param(int32_t y, const char *label, uint32_t v)   /* a label and a bar, v 0..32767 */
{
    cv_begin(240, 16, C_BLACK);
    cv_text(10, 0, &FONT_S, label, C_OFF);
    cv_rect(80, 3, 150, 10, C_TRACK);
    if (v >= 219u) cv_rect(80, 3, (int32_t)((v * 150u) >> 15), 10, C_ACC);
    cv_blit(0, y);
}

static void draw_env(void)
{
    char s[40], *p = s;
    p = put(p, "ATK ");
    p = put_u(p, fom_env_ms(set.atk));
    p = put(p, "ms  REL ");
    p = put_u(p, fom_env_ms(set.rel));
    p = put(p, "ms  ");
    p = put_note(p, last_note);
    *p = 0;
    draw_text_box(0, 106, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 56, C_BLACK);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 56, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 34, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, 148);
}

static void draw_status(int32_t oct, uint32_t ch)
{
    char s[24] = "OCT +0  MIDI OMNI";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (ch) {
        s[13] = 'C'; s[14] = 'H'; s[15] = (char)(ch < 10u ? ' ' : '1'); s[16] = (char)('0' + ch % 10u);
    }
    draw_text_box(0, 214, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_midi(uint32_t src, uint32_t bpm, uint32_t running)   /* where notes came from last, the clock */
{
    char s[32], *p = s;
    p = put(p, src == 2u ? "TRS IN" : src == 1u ? "USB IN" : "");
    if (bpm) {
        if (p != s) p = put(p, "  ");
        p = put(p, running ? "CLOCK " : "clock ");
        p = put_u(p, bpm);
        p = put(p, " BPM");
    }
    *p = 0;
    draw_text_box(0, 126, 240, &FONT_S, s, C_OFF, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
static uint32_t idle_ok, midi_src, clk_n, clk_t0, clk_bpm, clk_run, clk_beat_ms;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {                      /* clock: 24 a quarter note */
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    clk_bpm = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
                clk_beat_ms = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_run = 1; clk_n = 0; continue; }   /* START: the next clock is beat 1 */
        if (st == 0xFBu) { clk_run = 1; continue; }              /* CONTINUE */
        if (st == 0xFCu) { clk_run = 0; continue; }              /* STOP */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;      /* not our channel */
        if (ty == 0x90u && d2) { play(d1, d2); midi_src = (uint32_t)src; }
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
        else if (ty == 0xE0u) fom_set_bend(((int32_t)((uint32_t)d2 << 7 | d1) - 8192) >> 5);   /* +-2 semitones */
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

static uint16_t nudge(uint16_t v, int32_t e)    /* a knob's detents on a 0..32767 value: 64 detents end to end */
{
    int32_t c = (int32_t)v + e * 512;
    return (uint16_t)(c < 0 ? 0 : c > 32767 ? 32767 : c);
}
static uint8_t nudge_u8(uint8_t v, int32_t e, int32_t max)
{
    int32_t c = (int32_t)v + e;
    return (uint8_t)(c < 0 ? 0 : c > max ? max : c);
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, status_drawn = ~0u, midi_drawn = ~0u;
    uint32_t shape_drawn = ~0u, timbre_drawn = ~0u, color_drawn = ~0u, env_drawn = ~0u;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    fom_init();
    if (settings_load(&set, sizeof set) || !set_valid())
        set_defaults();
    set_apply();

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_L, "FROWN", C_ACC, 1);

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, out_ch = set.ch ? set.ch - 1u : 0u, st;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); play(held[k], vel); midi_send(0x90u | out_ch, held[k], vel); }
            if (up >> k & 1u) { stop(held[k]); midi_send(0x80u | out_ch, held[k], 0); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); play(ext_note[key], vel); midi_send(0x90u | out_ch, ext_note[key], vel); }
            else if (ext_note[key]) { stop(ext_note[key]); midi_send(0x80u | out_ch, ext_note[key], 0); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); play(pad_note[pad], vel); midi_send(0x90u | out_ch, pad_note[pad], vel); }
            else if (pad_note[pad]) { stop(pad_note[pad]); midi_send(0x80u | out_ch, pad_note[pad], 0); pad_note[pad] = 0; }
        }
        fom_set_bend(smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms) >> 5);   /* 1/4096 -> 1/128 semitone */
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) { set.ch = nudge_u8(set.ch, e, 16); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            set.shape = nudge_u8(set.shape, e, FM1_NSHAPES - 1);
            fom_set_shape(set.shape); dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) { set.timbre = nudge(set.timbre, e); fom_set_timbre(set.timbre); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB2)) != 0) { set.color = nudge(set.color, e); fom_set_color(set.color); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB3)) != 0) { set.atk = nudge_u8(set.atk, e, FOM_ENV_MAX); fom_set_env(set.atk, set.rel); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB4)) != 0) { set.rel = nudge_u8(set.rel, e, FOM_ENV_MAX); fom_set_env(set.atk, set.rel); dirty = 1; t_change = fm1_ms; }

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < FOM_NV; k++)            /* what is held, on the keys of this octave */
            if (voice[k].on && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));   /* lit: running; a blink a beat */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows red while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 128 : 8, on ? 20 : 1, on ? 20 : 1);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (set.shape != shape_drawn) {
            shape_drawn = set.shape;
            draw_shape();
            timbre_drawn = color_drawn = ~0u;   /* their labels follow the shape */
        }
        if (set.timbre != timbre_drawn) { timbre_drawn = set.timbre; draw_param(68, FM1_SHAPE_TIMBRE[set.shape], set.timbre); }
        if (set.color != color_drawn) { color_drawn = set.color; draw_param(86, FM1_SHAPE_COLOR[set.shape], set.color); }
        if ((st = set.atk | (uint32_t)set.rel << 8 | last_note << 16) != env_drawn) { env_drawn = st; draw_env(); }
        if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }   /* redraw only what changed */
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4) != status_drawn) { status_drawn = st; draw_status(oct, set.ch); }
        if ((st = midi_src | clk_bpm << 2 | clk_run << 12) != midi_drawn) { midi_drawn = st; draw_midi(midi_src, clk_bpm, clk_run); }
        idle_ok = 0;

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
