/* SPDX-License-Identifier: GPL-3.0-only */
/* Frown of Misery: Mutable Instruments' Braids (and, to come, Rings and Clouds) on the M-VAVE FM-1.
 * Built on Dinghy (zvenson), on Felucca's (Leo Kuroshita, Hügelton Instruments) and SLOOP's (isod89) platform.
 * This file is the app; the sound is dsp/engine/fom_engine.cc; everything below platform.c is Dinghy's, unchanged.
 *
 *   keys       play; 4 voices paraphonic (one shape, a pitch each) or MONO (legato, last note wins); out over USB-MIDI
 *   SELECT     the engine: BRAIDS or RINGS (Clouds to come)
 *   ALGORITHM  Braids' shape, 48 of them; Rings' model: MODAL RESONATOR, SYMPATHETIC STRINGS, STRING
 *   EDIT       the next page of parameters; KNOB 1..4 turn the four on the screen
 *     BRAIDS     OSC  TIMBRE, COLOR (what they do depends on the shape: the screen says), FINE tune, LEVEL
 *                AMP  ATTACK, RELEASE, GLIDE, VOICES (POLY 4 / MONO)
 *     RINGS      RES  STRUCTURE, BRIGHTNESS, DAMPING, POSITION (Rings' four knobs; STRUCTURE tunes the
 *                     sympathetic strings and sets the string's dispersion)
 *                PART POLY (1, 2, 4 voices taking the strikes in turn), FINE tune, LEVEL
 *              Rings has no note-off: a key strikes, DAMPING decides how long it rings.
 *   OCT- OCT+  the octave
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack, with their velocity, pitch bend +-2; a MIDI clock shows its tempo;
 *              program change 0, 1 (programs 1, 2 in most software) picks the engine, as SELECT does
 *   SMK-37     the keys with velocity, the outer keys, the pads (white keys F3..G5), the pitch wheel (+-2 semitones)
 *   the screen the engine, the shape, the page, the four parameters, the last note, CPU load of the audio interrupt
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

#ifdef DINGHY_HOST
static uint32_t fm1_ticks(void) { return 0; }   /* (no timer on the host: the CPU meter reads 0 there) */
#define FM1_TICKS_PER_US 24u
#endif

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
static volatile uint32_t cpu_now, cpu_peak;     /* the audio ISR's load, per mille of its time budget */

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), dt, load;
    fom_render_add(out, n);
    dt = fm1_ticks() - t0;
    /* the budget of n frames at 44.1 kHz: n * 24 MHz / 44100 = n * 544.2 ticks */
    load = n ? dt * 1000u / (n * 544u) : 0u;
    cpu_now = load;
    if (load > cpu_peak) cpu_peak = load;
}

/* ---- what survives a power-off: the panel, and every parameter of the engine */
#define SET_VERSION 2u
enum { V_POLY, V_MONO };
static struct {
    uint8_t version, oct, ch, engine, page, voices, pad[2];
    int16_t p[FOM_NPARAM];
} set;

static void set_defaults(void)
{
    uint32_t p;
    set.version = SET_VERSION;
    set.oct = 0; set.ch = 0; set.engine = FOM_E_BRAIDS; set.page = 0; set.voices = V_POLY;
    for (p = 0; p < FOM_NPARAM; p++) set.p[p] = fom_param_info(p)->def;
}
static int set_valid(void)
{
    uint32_t p;
    if (set.version != SET_VERSION || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u ||
        set.engine >= FOM_NENGINES || set.voices > V_MONO)
        return 0;
    for (p = 0; p < FOM_NPARAM; p++)
        if (set.p[p] < fom_param_info(p)->min || set.p[p] > fom_param_info(p)->max) return 0;
    return 1;
}
static void set_apply(void)
{
    uint32_t p;
    fom_set_engine(set.engine);
    for (p = 0; p < FOM_NPARAM; p++) fom_set_param(p, set.p[p]);
}

/* ---- the pages: what KNOB 1..4 turn, per engine. Parameters past FOM_NPARAM are the app's own. */
enum { A_VOICES = FOM_NPARAM, A_NONE = 255 };
#define NPAGE_MAX 4u
typedef struct {
    const char *name;
    uint8_t npages;
    const char *page[NPAGE_MAX];
    uint8_t knob[NPAGE_MAX][4];
} engine_ui_t;
static const engine_ui_t ENGINES[FOM_NENGINES] = {
    {"BRAIDS", 2, {"OSC", "AMP"},
     {{FOM_P_TIMBRE, FOM_P_COLOR, FOM_P_FINE, FOM_P_LEVEL},
      {FOM_P_ATTACK, FOM_P_RELEASE, FOM_P_GLIDE, A_VOICES}}},
    {"RINGS", 2, {"RES", "PART"},
     {{FOM_P_STRUCTURE, FOM_P_BRIGHTNESS, FOM_P_DAMPING, FOM_P_POSITION},
      {FOM_P_POLY, FOM_P_FINE, FOM_P_LEVEL, A_NONE}}},
};
static const engine_ui_t *ui(void) { return &ENGINES[set.engine]; }

static const char *param_name(uint32_t p)
{
    if (p == A_VOICES) return "VOICES";
    if (set.engine == FOM_E_BRAIDS && p == FOM_P_TIMBRE) return FM1_SHAPE_TIMBRE[set.p[FOM_P_SHAPE]];
    if (set.engine == FOM_E_BRAIDS && p == FOM_P_COLOR) return FM1_SHAPE_COLOR[set.p[FOM_P_SHAPE]];
    return fom_param_info(p)->name;
}

/* ---- notes (main loop): which of the engine's voices plays what */
static struct { uint8_t note, on; uint32_t age; } voice[FOM_NV];
static uint32_t ages, last_note = 255;
static uint8_t mono_stack[16], mono_n;          /* MONO: the notes held, the newest last */

static void all_off(void)
{
    uint32_t v;
    for (v = 0; v < FOM_NV; v++) { voice[v].on = 0; fom_voice_release(v); }
    mono_n = 0;
}

static void mono_sound(uint32_t note, uint32_t vel, int strike)
{
    voice[0].note = (uint8_t)note;
    voice[0].on = 1;
    fom_voice_play(0, note, (int32_t)(vel * 200u), strike);   /* one voice: louder than one of four */
    last_note = note;
}
static void mono_on(uint32_t note, uint32_t vel)
{
    uint32_t i, j, legato = mono_n > 0;
    for (i = j = 0; i < mono_n; i++)            /* (the same note again: to the top) */
        if (mono_stack[i] != note) mono_stack[j++] = mono_stack[i];
    mono_n = (uint8_t)j;
    if (mono_n == sizeof mono_stack) {          /* full: forget the oldest */
        for (i = 1; i < mono_n; i++) mono_stack[i - 1] = mono_stack[i];
        mono_n--;
    }
    mono_stack[mono_n++] = (uint8_t)note;
    mono_sound(note, vel, !legato);             /* legato: glide there, nothing restarts */
}
static void mono_off(uint32_t note)
{
    uint32_t i, j, top = mono_n ? mono_stack[mono_n - 1] : 255u;
    for (i = j = 0; i < mono_n; i++)
        if (mono_stack[i] != note) mono_stack[j++] = mono_stack[i];
    mono_n = (uint8_t)j;
    if (note != top) return;                    /* an older note let go: the sound stays */
    if (mono_n) mono_sound(mono_stack[mono_n - 1], 100, 0);   /* back to the note still held */
    else { voice[0].on = 0; fom_voice_release(0); }
}

static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    if (note > 127u || !vel) return;
    if (set.engine == FOM_E_RINGS) fom_strike(note, vel);   /* (voice[] below only keeps the keys lit) */
    else if (set.voices == V_MONO) { mono_on(note, vel); return; }
    for (v = 0; v < FOM_NV; v++) {              /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == note && voice[v].on ? 0u                  /* the same note, held */
                       : voice[v].on ? 0x80000000u + voice[v].age                   /* held: the oldest goes */
                       : fom_voice_sounding(v) ? 2u : 1u;                           /* free: silent before releasing */
        if (score < best || (score == best && voice[v].age < voice[pick].age)) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].on = 1;
    voice[pick].age = ++ages;
    if (set.engine != FOM_E_RINGS)
        fom_voice_play(pick, note, (int32_t)(vel * 90u), 1);   /* 127 -> 11430: four voices stay under full scale */
    last_note = note;
}
static void stop(uint32_t note)
{
    uint32_t v;
    if (note > 127u) return;
    if (set.engine != FOM_E_RINGS && set.voices == V_MONO) { mono_off(note); return; }
    for (v = 0; v < FOM_NV; v++)
        if (voice[v].on && voice[v].note == note) {
            voice[v].on = 0;
            if (set.engine != FOM_E_RINGS) fom_voice_release(v);   /* (Rings: no note-off) */
        }
}
static int quiet(void)                          /* nothing sounds, not even a release or a ringing resonator */
{
    return !fom_sounding();
}

/* ---- the screen */
#define C_ACC RGB(255, 52, 52)                  /* a sore red */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define C_TRACK RGB(40, 40, 40)                /* (also the CPU bar: dim, 2 px, under the status line) */
#define KEY0 53u                                /* key 0 = F3 */
#define Y_SHAPE 40
#define Y_TABS 60
#define Y_ROW0 82                               /* four rows of 18 px */
#define Y_INFO 156
#define Y_STATUS 222

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
static char *put_ms(char *p, uint32_t ms)       /* 0 ms .. 999 ms, 1.0 s .. 23.8 s */
{
    if (ms < 1000u) return put(put_u(p, ms), " ms");
    p = put_u(p, ms / 1000u);
    *p++ = '.';
    return put(put_u(p, ms / 100u % 10u), " s");
}

static void draw_title(void)
{
    draw_text_box(0, 4, 240, &FONT_L, ui()->name, C_ACC, 1);
}

static void draw_shape(void)                    /* the line under the engine's name: Braids' shape, Rings' model */
{
    char s[32], *p = s;
    if (set.engine == FOM_E_RINGS) {
        static const char *const MODEL[3] = {"MODAL RESONATOR", "SYMPATHETIC STRINGS", "STRING"};
        p = put(p, MODEL[set.p[FOM_P_MODEL]]);
    } else {
        p = put_u(p, (uint32_t)set.p[FOM_P_SHAPE] + 1u);
        p = put(p, "/48  ");
        p = put(p, FM1_SHAPE_NAME[set.p[FOM_P_SHAPE]]);
    }
    *p = 0;
    draw_text_box(0, Y_SHAPE, 240, &FONT_S, s, C_WHITE, 1);
}

static void draw_tabs(void)                     /* the pages, the one shown in red */
{
    uint32_t i, w = 0;
    int32_t x;
    for (i = 0; i < ui()->npages; i++) w += text_w(&FONT_S, ui()->page[i]) + (i ? 24u : 0u);
    cv_begin(240, 16, C_BLACK);
    x = (240 - (int32_t)w) / 2;
    for (i = 0; i < ui()->npages; i++) {
        uint32_t on = i == set.page;
        int32_t x1 = cv_text(x, 0, &FONT_S, ui()->page[i], on ? C_ACC : C_OFF);
        if (on) cv_rect(x, 15, x1 - x, 1, C_ACC);
        x = x1 + 24;
    }
    cv_blit(0, Y_TABS);
}

static int32_t param_get(uint32_t p) { return p == A_VOICES ? set.voices : p < FOM_NPARAM ? set.p[p] : 0; }

static void draw_row(uint32_t k)                /* knob k's parameter on this page: its name, its value */
{
    uint32_t p = ui()->knob[set.page][k];
    char s[24], *q = s, lab[8];
    int32_t v = param_get(p);
    cv_begin(240, 16, C_BLACK);
    lab[0] = (char)('1' + k); lab[1] = ' '; lab[2] = 0;
    cv_text(4, 0, &FONT_S, lab, C_TRACK);
    if (p != A_NONE) {
        cv_text(20, 0, &FONT_S, param_name(p), C_OFF);
        if (p == A_VOICES) q = put(q, v == V_MONO ? "MONO" : "POLY 4");
        else switch (fom_param_info(p)->kind) {
        case FOM_K_BAR:
            cv_rect(80, 3, 150, 10, C_TRACK);
            if (v >= 219) cv_rect(80, 3, (int32_t)(((uint32_t)v * 150u) >> 15), 10, C_ACC);
            break;
        case FOM_K_TIME:
            q = (p == FOM_P_GLIDE && !v) ? put(q, "OFF") : put_ms(q, fom_env_ms((uint32_t)v));
            break;
        case FOM_K_POLY:
            q = put(put_u(q, 1u << (uint32_t)v), v ? " VOICES" : " VOICE");
            break;
        case FOM_K_CENTS: {
            int32_t c = v * 100 / 128;
            *q++ = c < 0 ? '-' : '+';
            q = put(put_u(q, (uint32_t)(c < 0 ? -c : c)), " ct");
            break;
        }
        default:
            q = put_u(q, (uint32_t)v);
        }
        *q = 0;
        if (q != s) cv_text(80, 0, &FONT_S, s, C_WHITE);
    }
    cv_blit(0, Y_ROW0 + 18 * (int32_t)k);
}

static void draw_info(uint32_t src, uint32_t bpm, uint32_t running)   /* the last note, where it came from, the clock */
{
    char s[40], *p = s;
    p = put(p, "NOTE ");
    p = put_note(p, last_note);
    if (src) p = put(p, src == 2u ? "  TRS" : "  USB");
    if (bpm) {
        p = put(p, running ? "  CLOCK " : "  clock ");
        p = put_u(p, bpm);
    }
    *p = 0;
    draw_text_box(0, Y_INFO, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_status(int32_t oct, uint32_t ch, uint32_t cpu)   /* cpu: per mille */
{
    char s[40], *p = s;
    p = put(p, "OCT ");
    *p++ = oct < 0 ? '-' : '+';
    *p++ = (char)('0' + (oct < 0 ? -oct : oct));
    p = put(p, "  MIDI ");
    p = ch ? put_u(put(p, "CH"), ch) : put(p, "OMNI");
    p = put(p, "  CPU ");
    p = put(put_u(p, (cpu + 5u) / 10u), "%");
    *p = 0;
    draw_text_box(0, Y_STATUS, 240, &FONT_S, s, cpu >= 800u ? C_ACC : C_OFF, 1);
    {   /* and as a bar on the bottom two rows, 2.4 px a percent: a glance, and tests/emu_smoke.mjs reads it */
        uint32_t w = cpu * 240u / 1000u;
        if (w > 240u) w = 240u;
        lcd_fill(0, 238, 240, 2, C_BLACK);
        if (w) lcd_fill(0, 238, w, 2, cpu >= 800u ? C_ACC : C_TRACK);
    }
}

/* ---- the engine: SELECT or a MIDI program change */
static uint32_t engine_changed;                 /* set when it changes: the main loop saves it */
static void set_engine(uint32_t e)
{
    if (e >= FOM_NENGINES || e == set.engine) return;
    all_off();
    set.engine = (uint8_t)e;
    set.page = 0;
    fom_set_engine(set.engine);
    engine_changed = 1;
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
        else if (ty == 0xC0u) set_engine(d1);                 /* program change: the engine */
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

/* a knob's detents on parameter p: a bar moves 1/64 of its range a detent, a time or an index one step */
static void knob_turn(uint32_t p, int32_t e)
{
    if (p == A_VOICES) {
        uint8_t v = (uint8_t)(e > 0 ? V_MONO : V_POLY);
        if (v != set.voices) { all_off(); set.voices = v; }
        return;
    }
    if (p >= FOM_NPARAM) return;
    {
        const fom_param_info_t *in = fom_param_info(p);
        int32_t step = in->kind == FOM_K_BAR ? 512 : in->kind == FOM_K_CENTS ? 2 : 1;
        int32_t c = set.p[p] + e * step;
        set.p[p] = (int16_t)(c < in->min ? in->min : c > in->max ? in->max : c);
        fom_set_param(p, set.p[p]);
    }
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, status_drawn = ~0u, info_drawn = ~0u;
    uint32_t title_drawn = ~0u, shape_drawn = ~0u, tabs_drawn = ~0u, row_drawn[4], cpu_shown = 0, t_cpu = 0;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;
    fom_init();
    if (settings_load(&set, sizeof set) || !set_valid())
        set_defaults();
    if (set.page >= ui()->npages) set.page = 0;
    set_apply();

    lcd_fill(0, 0, 240, 240, C_BLACK);

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
        if (b & (1u << BTN_EDIT)) { set.page = (uint8_t)((set.page + 1u) % ui()->npages); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {   /* the engine */
            int32_t c = (int32_t)set.engine + e;
            set_engine((uint32_t)(c < 0 ? 0 : c >= (int32_t)FOM_NENGINES ? (int32_t)FOM_NENGINES - 1 : c));
        }
        if (engine_changed) { engine_changed = 0; dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {   /* Braids' shape, Rings' model */
            knob_turn(set.engine == FOM_E_RINGS ? FOM_P_MODEL : FOM_P_SHAPE, e); dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++)
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0 && ui()->knob[set.page][k] != A_NONE) {
                knob_turn(ui()->knob[set.page][k], e); dirty = 1; t_change = fm1_ms;
            }

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */

        if (fm1_ms - t_cpu >= 500u) {           /* the CPU meter: the peak of the last half second */
            t_cpu = fm1_ms;
            cpu_shown = cpu_peak;
            cpu_peak = cpu_now;
        }

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < FOM_NV; k++)            /* what is held, on the keys of this octave */
            if (voice[k].on && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_EDIT, set.page != 0);
        fm1_led_key(BTN_PLAY, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));   /* lit: running; a blink a beat */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows red while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 128 : 8, on ? 20 : 1, on ? 20 : 1);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (set.engine != title_drawn) { title_drawn = set.engine; draw_title(); tabs_drawn = ~0u; }
        if ((st = (uint32_t)set.p[FOM_P_SHAPE] | set.engine << 8 | (uint32_t)set.p[FOM_P_MODEL] << 12) != shape_drawn) {
            shape_drawn = st;
            draw_shape();
            for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;   /* TIMBRE's and COLOR's names follow the shape */
        }
        if ((st = set.page | set.engine << 4) != tabs_drawn) {
            tabs_drawn = st;
            draw_tabs();
            for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;
        }
        for (k = 0; k < 4u; k++)
            if ((st = (uint32_t)(param_get(ui()->knob[set.page][k]) & 0xFFFF)) != row_drawn[k]) {
                row_drawn[k] = st;
                draw_row(k);
            }
        if ((st = last_note | midi_src << 8 | clk_bpm << 10 | clk_run << 20) != info_drawn) {
            info_drawn = st;
            draw_info(midi_src, clk_bpm, clk_run);
        }
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4 | (cpu_shown + 5u) / 10u << 9) != status_drawn) {
            status_drawn = st;
            draw_status(oct, set.ch, cpu_shown);
        }
        idle_ok = 0;

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
