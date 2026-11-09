/* SPDX-License-Identifier: GPL-3.0-only */
/* Frown of Misery: Mutable Instruments' Braids (and, to come, Rings and Clouds) on the M-VAVE FM-1.
 * Built on Dinghy (zvenson), on Felucca's (Leo Kuroshita, Hügelton Instruments) and SLOOP's (isod89) platform.
 * This file is the app; the sound is dsp/engine/fom_engine.cc; everything below platform.c is Dinghy's, unchanged.
 *
 *   keys       play; 4 voices paraphonic (one shape, a pitch each) or MONO (legato, last note wins); out over USB-MIDI
 *   SELECT     the engine: BRAIDS, RINGS, or BRAIDS > RINGS (Braids into Rings' audio input: a note plays Braids and
 *              picks Rings' voice and note; Braids' LEVEL drives Rings, Rings' PART LEVEL is its output)
 *   FX         Clouds after the engine, or not (the strip under the info line shows the route)
 *   REC        Clouds' FREEZE: the buffer stops recording, the grains keep playing what it holds
 *   ALGORITHM  Braids' shape, 48 of them; Rings' model: MODAL RESONATOR, SYMPATHETIC STRINGS, STRING
 *   EDIT       the next page of parameters; KNOB 1..4 turn the four on the screen
 *     BRAIDS     OSC  TIMBRE, COLOR (what they do depends on the shape: the screen says), FINE tune, LEVEL
 *                AMP  ATTACK, RELEASE, GLIDE, VOICES (POLY 4 / MONO)
 *     RINGS      RES  STRUCTURE, BRIGHTNESS, DAMPING, POSITION (Rings' four knobs; STRUCTURE tunes the
 *                     sympathetic strings and sets the string's dispersion)
 *                PART POLY (1, 2, 4 voices taking the strikes in turn), FINE tune, LEVEL
 *              Rings has no note-off: a key strikes, DAMPING decides how long it rings.
 *     CLOUDS     GRAIN POSITION, SIZE, DENSITY, TEXTURE (Clouds' granular mode, its knobs as on the module)
 *                MIX   BLEND (dry/wet), SPREAD, FEEDBACK, REVERB
 *                PITCH PITCH, -24..+24 semitones; QUALITY HI (16 bits, 32 kHz) or LO (Clouds' low fidelity: 8-bit
 *                      buffers, 16 kHz, half the CPU; always LO after the sympathetic strings, where HI does not fit)
 *   OCT- OCT+  the octave
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack, with their velocity, pitch bend +-2; a MIDI clock shows its tempo;
 *              program change 0..5 (programs 1..6 in most software): BRAIDS, RINGS, BRAIDS > CLOUDS, RINGS > CLOUDS,
 *              BRAIDS > RINGS, BRAIDS > RINGS > CLOUDS
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
static volatile uint32_t clouds_peak;           /* Clouds' part of it */

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), dt, load;
    fom_render_add(out, n);
    dt = fm1_ticks() - t0;
    /* the budget of n frames at 44.1 kHz: n * 24 MHz / 44100 = n * 544.2 ticks */
    load = n ? dt * 1000u / (n * 544u) : 0u;
    cpu_now = load;
    if (load > cpu_peak) cpu_peak = load;
    fom_set_load(load);                         /* (Clouds' guard: fewer grains if the time runs short) */
    load = n ? fom_clouds_ticks() * 1000u / (n * 544u) : 0u;
    if (load > clouds_peak) clouds_peak = load;
}

/* ---- what survives a power-off: the panel, and every parameter of the engine */
#define SET_VERSION 5u
enum { V_POLY, V_MONO };
static struct {
    uint8_t version, oct, ch, engine, page, voices, clouds, pad;
    int16_t p[FOM_NPARAM];
} set;

static void set_defaults(void)
{
    uint32_t p;
    set.version = SET_VERSION;
    set.oct = 0; set.ch = 0; set.engine = FOM_E_BRAIDS; set.page = 0; set.voices = V_POLY; set.clouds = 0;
    for (p = 0; p < FOM_NPARAM; p++) set.p[p] = fom_param_info(p)->def;
}
static int set_valid(void)
{
    uint32_t p;
    if (set.version != SET_VERSION || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u ||
        set.engine >= FOM_NENGINES || set.voices > V_MONO || set.clouds > 1u)
        return 0;
    for (p = 0; p < FOM_NPARAM; p++)
        if (set.p[p] < fom_param_info(p)->min || set.p[p] > fom_param_info(p)->max) return 0;
    return 1;
}
static void set_apply(void)
{
    uint32_t p;
    fom_set_engine(set.engine);
    fom_set_clouds(set.clouds);
    for (p = 0; p < FOM_NPARAM; p++) fom_set_param(p, set.p[p]);
}

/* ---- the pages: what KNOB 1..4 turn, per module; a route's pages are its modules' in order. Parameters past
 * FOM_NPARAM are the app's own. */
enum { A_VOICES = FOM_NPARAM, A_NONE = 255 };
enum { M_BRAIDS, M_RINGS, M_CLOUDS, M_N };
typedef struct {
    const char *page[3];
    uint8_t npages;
    uint8_t knob[3][4];
} module_ui_t;
static const module_ui_t MODULES[M_N] = {
    {{"OSC", "AMP"}, 2,
     {{FOM_P_TIMBRE, FOM_P_COLOR, FOM_P_FINE, FOM_P_LEVEL},
      {FOM_P_ATTACK, FOM_P_RELEASE, FOM_P_GLIDE, A_VOICES}}},
    {{"RES", "PART"}, 2,
     {{FOM_P_STRUCTURE, FOM_P_BRIGHTNESS, FOM_P_DAMPING, FOM_P_POSITION},
      {FOM_P_POLY, FOM_P_FINE, FOM_P_R_LEVEL, A_NONE}}},
    {{"GRAIN", "MIX", "PITCH"}, 3,
     {{FOM_P_C_POSITION, FOM_P_C_SIZE, FOM_P_C_DENSITY, FOM_P_C_TEXTURE},
      {FOM_P_C_BLEND, FOM_P_C_SPREAD, FOM_P_C_FEEDBACK, FOM_P_C_REVERB},
      {FOM_P_C_PITCH, FOM_P_C_QUALITY, A_NONE, A_NONE}}},
};
/* the route's modules in order: the engine's one or two, then Clouds if it follows */
static uint32_t route_modules(uint8_t *m)
{
    uint32_t n = 0;
    if (set.engine != FOM_E_RINGS) m[n++] = M_BRAIDS;
    if (set.engine != FOM_E_BRAIDS) m[n++] = M_RINGS;
    if (set.clouds) m[n++] = M_CLOUDS;
    return n;
}
static uint32_t npages(void)
{
    uint8_t m[3];
    uint32_t i, n = route_modules(m), p = 0;
    for (i = 0; i < n; i++) p += MODULES[m[i]].npages;
    return p;
}
/* page i of the route: its module and its index there */
static uint32_t page_module(uint32_t i, uint32_t *local)
{
    uint8_t m[3];
    uint32_t k, n = route_modules(m);
    for (k = 0; k < n; k++) {
        if (i < MODULES[m[k]].npages) { if (local) *local = i; return m[k]; }
        i -= MODULES[m[k]].npages;
    }
    if (local) *local = 0;
    return m[0];
}
static const char *page_name(uint32_t i)
{
    uint32_t l, m = page_module(i, &l);
    return MODULES[m].page[l];
}
static uint32_t page_knob(uint32_t i, uint32_t k)
{
    uint32_t l, m = page_module(i, &l);
    return MODULES[m].knob[l][k];
}

static const char *param_name(uint32_t p)
{
    if (p == A_VOICES) return "VOICES";
    if (p == FOM_P_TIMBRE) return FM1_SHAPE_TIMBRE[set.p[FOM_P_SHAPE]];
    if (p == FOM_P_COLOR) return FM1_SHAPE_COLOR[set.p[FOM_P_SHAPE]];
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
    if (set.engine != FOM_E_BRAIDS) fom_strike(note, vel);  /* (Rings alone: voice[] below only keeps keys lit) */
    if (set.engine != FOM_E_RINGS && set.voices == V_MONO) { mono_on(note, vel); return; }
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

/* ---- the screen: the faceplate of the module whose page is shown, after Mutable Instruments' panels (their light
 * aluminium, dark DIN lettering, teal and pink label pills, the patterned band under the subtitle; drawn anew here:
 * nothing of the panels' artwork is copied), knobs that turn, and the route as small plates and patch cables */
#define C_PANEL RGB(226, 226, 222)                /* the aluminium */
#define C_INK RGB(40, 40, 42)                     /* its lettering */
#define C_INK2 RGB(118, 118, 120)
#define C_BAND RGB(176, 176, 172)
#define C_TEAL RGB(0, 146, 158)
#define C_PINK RGB(222, 56, 92)
#define C_KNOB RGB(56, 56, 58)
#define C_SKIRT RGB(198, 198, 194)
#define C_DARK RGB(104, 104, 106)                 /* the output box */
#define C_LED RGB(255, 60, 30)
#define C_LEDOFF RGB(70, 22, 16)
#define C_ACC C_PINK
#define C_TRACK RGB(40, 40, 40)                  /* (the CPU bar: dim, 2 px, along the bottom) */
#define KEY0 53u                                /* key 0 = F3 */
#define Y_HEAD 0                                /* 64 (platform.c's canvas: 240 x 64 at most): name, band, display */
#define Y_TABS 64                               /* 18 */
#define Y_KNOBS 82                              /* 78: four knobs */
#define Y_INFO 160                              /* 18 */
#define Y_ROUTE 178                             /* 38 */
#define Y_STATUS 216                            /* 22, then the CPU bar on 238..239 */

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

/* text on any ground (gfx.c's cv_text blends onto black): the font's 16 levels between bg and fg */
static int32_t cv_text_on(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t ramp[16];
    int32_t fr = fg >> 11, fgg = (fg >> 5) & 63, fb = fg & 31, br = bg >> 11, bgg = (bg >> 5) & 63, bb = bg & 31, a;
    for (a = 0; a < 16; a++)
        ramp[a] = (uint16_t)(((br + (fr - br) * a / 15) << 11) | ((bgg + (fgg - bgg) * a / 15) << 5) |
                             (bb + (fb - bb) * a / 15));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}
static void cv_text_mid(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    cv_text_on(cx - text_w(f, s) / 2, y, f, s, fg, bg);
}

static void cv_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy, w = r;
    for (dy = 0; dy <= r; dy++) {               /* (the half width shrinks row by row: an integer square root) */
        while (w > 0 && w * w + dy * dy > r * r + r / 2) w--;
        cv_rect(cx - w, cy - dy, 2 * w + 1, 1, c);
        if (dy) cv_rect(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}
static void cv_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c, uint16_t inside)
{
    cv_disc(cx, cy, r, c);
    cv_disc(cx, cy, r - 1, inside);
}
static void cv_pill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c, uint16_t bg)   /* rounded corners */
{
    cv_rect(x, y, w, h, c);
    cv_pset(x, y, bg); cv_pset(x + w - 1, y, bg); cv_pset(x, y + h - 1, bg); cv_pset(x + w - 1, y + h - 1, bg);
}

/* the module a page belongs to: the engine's own pages, then Clouds' */
static uint32_t shown_module(void) { return page_module(set.page, 0); }

/* Braids' four-character display, its names from braids/settings.cc (MIT; its own wave glyphs drawn as ASCII) */
static const char *const BRAIDS_CODE[FM1_NSHAPES] = {
    "CSAW", "/\\/_", "/|_|", "FOLD", "BUZZ", "SUB_", "SUB/", "SYN_", "SYN/", "//x3", "_-x3", "/\\x3", "SIx3",
    "RING", "SWRM", "COMB", "TOY*", "ZLPF", "ZPKF", "ZBPF", "ZHPF", "VOSM", "VOWL", "VFOF", "HARM", "FM  ", "FBFM",
    "WTFM", "PLUK", "BOWD", "BLOW", "FLUT", "BELL", "DRUM", "KICK", "CYMB", "SNAR", "WTBL", "WMAP", "WLIN", "WTx4",
    "NOIS", "TWNQ", "CLKN", "CLOU", "PRTC", "QPSK", "????",
};

static uint32_t frozen;                         /* Clouds' FREEZE (REC): not kept over a power-off */
static uint32_t clouds_shown;                   /* Clouds' share of the CPU, per mille (peak of 0.5 s) */

static void draw_band(int32_t y)                /* the strip under the subtitle: a chain of small diamonds */
{
    int32_t x;
    cv_rect(0, y, 240, 1, C_BAND);
    cv_rect(0, y + 6, 240, 1, C_BAND);
    for (x = 2; x < 240; x += 8) {
        cv_pset(x + 3, y + 1, C_BAND); cv_pset(x + 2, y + 2, C_BAND); cv_pset(x + 4, y + 2, C_BAND);
        cv_pset(x + 1, y + 3, C_BAND); cv_pset(x + 5, y + 3, C_BAND); cv_pset(x + 2, y + 4, C_BAND);
        cv_pset(x + 4, y + 4, C_BAND); cv_pset(x + 3, y + 5, C_BAND);
    }
}

/* Rings' two LEDs: the polyphony (one, two, four dots: teal, orange, red) and the model (modal: rings; sympathetic:
 * strings in a circle; string: one string) */
static void draw_rings_leds(int32_t y)
{
    static const uint16_t COL[3] = {C_TEAL, RGB(240, 150, 20), C_PINK};
    int32_t i, poly = set.p[FOM_P_POLY], model = set.p[FOM_P_MODEL];
    for (i = 0; i < 3; i++) {
        int32_t cx = 22 + 20 * i, on = i == poly;
        uint16_t c = on ? COL[i] : C_SKIRT;
        cv_disc(cx, y, 7, c);
        if (i == 0) cv_disc(cx, y, 2, C_PANEL);
        if (i == 1) { cv_disc(cx - 3, y, 1, C_PANEL); cv_disc(cx + 3, y, 1, C_PANEL); }
        if (i == 2) { cv_disc(cx - 2, y - 2, 1, C_PANEL); cv_disc(cx + 2, y - 2, 1, C_PANEL);
                      cv_disc(cx - 2, y + 2, 1, C_PANEL); cv_disc(cx + 2, y + 2, 1, C_PANEL); }
    }
    for (i = 0; i < 3; i++) {
        int32_t cx = 178 + 20 * i, on = i == model;
        uint16_t c = on ? COL[i] : C_SKIRT;
        cv_disc(cx, y, 7, c);
        if (i == 0) { cv_ring(cx, y, 5, C_PANEL, c); cv_ring(cx, y, 3, C_PANEL, c); }
        if (i == 1) { cv_rect(cx - 3, y - 4, 1, 9, C_PANEL); cv_rect(cx, y - 4, 1, 9, C_PANEL); cv_rect(cx + 3, y - 4, 1, 9, C_PANEL); }
        if (i == 2) cv_line(cx - 4, y + 2, cx + 4, y - 2, C_PANEL);
    }
}

static void draw_head(void)                     /* the module's top: name, subtitle, band, its display or LEDs */
{
    static const char *const NAME[3] = {"Braids", "Rings", "Clouds"};
    static const char *const SUB[3] = {"macro oscillator", "resonator", "texture synthesizer"};
    uint32_t m = shown_module();
    char s[32], *q = s;
    cv_begin(240, 64, C_PANEL);
    cv_text_on(8, 3, &FONT_S, NAME[m], C_INK, C_PANEL);
    cv_text_on(232 - text_w(&FONT_S, SUB[m]), 3, &FONT_S, SUB[m], C_INK, C_PANEL);
    draw_band(21);
    if (m == M_BRAIDS) {                        /* the display, red on black, and what the shape is called */
        uint32_t sh = (uint32_t)set.p[FOM_P_SHAPE];
        cv_pill(8, 30, 84, 32, RGB(24, 12, 10), C_PANEL);
        cv_text_mid(50, 30, &FONT_L, BRAIDS_CODE[sh], C_LED, RGB(24, 12, 10));
        q = put(put_u(q, sh + 1u), "/48");
        *q = 0;
        cv_text_on(102, 30, &FONT_S, s, C_INK2, C_PANEL);
        cv_text_on(102, 46, &FONT_S, FM1_SHAPE_NAME[sh], C_INK, C_PANEL);
    } else if (m == M_RINGS) {
        static const char *const MODEL[3] = {"MODAL", "SYMPATHETIC", "STRING"};
        draw_rings_leds(38);
        cv_text_mid(120, 31, &FONT_S, MODEL[set.p[FOM_P_MODEL]], C_INK, C_PANEL);
        cv_text_mid(120, 47, &FONT_S, set.p[FOM_P_POLY] == 2 ? "4 voices" : set.p[FOM_P_POLY] ? "2 voices" : "1 voice",
                    C_INK2, C_PANEL);
    } else {                                    /* Clouds: FREEZE (REC), its mode and quality */
        cv_disc(22, 46, 9, frozen ? C_PINK : C_SKIRT);
        cv_disc(22, 46, 5, frozen ? RGB(255, 140, 160) : C_PANEL);
        cv_text_on(36, 38, &FONT_S, "FREEZE", C_INK, C_PANEL);
        cv_text_on(120, 30, &FONT_S, "granular", C_INK2, C_PANEL);
        cv_text_on(120, 46, &FONT_S, fom_clouds_lofi() ? "lo-fi, 16 kHz" : "16 bit, 32 kHz", C_INK, C_PANEL);
    }
    cv_blit(0, Y_HEAD);
}

static void draw_tabs(void)                     /* this module's pages (the one shown underlined); < > more modules */
{
    uint32_t i, first = set.page, n = npages(), w = 0, m = shown_module();
    int32_t x;
    while (first > 0 && page_module(first - 1u, 0) == m) first--;
    cv_begin(240, 18, C_PANEL);
    for (i = first; i < n && page_module(i, 0) == m; i++) w += text_w(&FONT_S, page_name(i)) + (i > first ? 24u : 0u);
    x = (240 - (int32_t)w) / 2;
    for (i = first; i < n && page_module(i, 0) == m; i++) {
        uint32_t on = i == set.page;
        int32_t x1 = cv_text_on(x, 0, &FONT_S, page_name(i), on ? C_INK : C_INK2, C_PANEL);
        if (on) cv_rect(x, 16, x1 - x, 2, C_INK);
        x = x1 + 24;
    }
    if (first > 0) cv_text_on(6, 0, &FONT_S, "<", C_INK2, C_PANEL);
    if (i < n) cv_text_on(226, 0, &FONT_S, ">", C_INK2, C_PANEL);
    cv_blit(0, Y_TABS);
}

static int32_t param_get(uint32_t p) { return p == A_VOICES ? set.voices : p < FOM_NPARAM ? set.p[p] : 0; }

/* the label's pill, as on the panels: teal or pink for the knobs they mark so, plain lettering for the rest */
static uint16_t pill_colour(uint32_t p)
{
    switch (p) {
    case FOM_P_TIMBRE: case FOM_P_C_SIZE: case FOM_P_C_TEXTURE: return C_TEAL;
    case FOM_P_COLOR: case FOM_P_C_POSITION: case FOM_P_C_DENSITY: return C_PINK;
    default: return C_PANEL;
    }
}

/* the pointer at 33 steps from 7 to 5 o'clock: (sin, -cos) * 256 */
static const int16_t KNOB_DIR[33][2] = {
    {-181, 181}, {-206, 152}, {-226, 121}, {-241, 86}, {-251, 50}, {-256, 13}, {-255, -25}, {-248, -62},
    {-237, -98}, {-220, -132}, {-198, -162}, {-172, -190}, {-142, -213}, {-109, -231}, {-74, -245}, {-38, -253},
    {0, -256}, {38, -253}, {74, -245}, {109, -231}, {142, -213}, {172, -190}, {198, -162}, {220, -132},
    {237, -98}, {248, -62}, {255, -25}, {256, 13}, {251, 50}, {241, 86}, {226, 121}, {206, 152}, {181, 181},
};

static void draw_knob(uint32_t k)               /* knob k (KNOB 1..4) on this page: the knob, its label, its value */
{
    uint32_t p = page_knob(set.page, k);
    char s[24], *q = s;
    const int32_t cx = 30, cy = 21;
    cv_begin(60, 78, C_PANEL);
    if (p != A_NONE) {
        int32_t v = param_get(p), lo = 0, hi = 1, i, dx, dy;
        const char *name = param_name(p);
        uint16_t pc = pill_colour(p);
        if (p != A_VOICES) { lo = fom_param_info(p)->min; hi = fom_param_info(p)->max; }
        i = hi > lo ? (v - lo) * 32 / (hi - lo) : 0;
        cv_disc(cx, cy, 19, C_SKIRT);           /* the skirt, its scale, the cap and the pointer */
        for (dx = 0; dx <= 32; dx += 8)
            cv_pset(cx + KNOB_DIR[dx][0] * 18 / 256, cy + KNOB_DIR[dx][1] * 18 / 256, C_INK2);
        cv_disc(cx, cy, 14, C_KNOB);
        dx = KNOB_DIR[i][0], dy = KNOB_DIR[i][1];
        cv_line(cx + dx * 4 / 256, cy + dy * 4 / 256, cx + dx * 13 / 256, cy + dy * 13 / 256, C_WHITE);
        cv_line(cx + dx * 4 / 256 + 1, cy + dy * 4 / 256, cx + dx * 13 / 256 + 1, cy + dy * 13 / 256, C_WHITE);
        if (pc != C_PANEL) {
            int32_t w = text_w(&FONT_S, name) + 8;
            cv_pill(cx - w / 2, 43, w, 17, pc, C_PANEL);
            cv_text_mid(cx, 43, &FONT_S, name, C_WHITE, pc);
        } else {
            cv_text_mid(cx, 43, &FONT_S, name, C_INK, C_PANEL);
        }
        if (p == A_VOICES) q = put(q, v == V_MONO ? "MONO" : "POLY 4");
        else switch (fom_param_info(p)->kind) {
        case FOM_K_TIME:
            q = (p == FOM_P_GLIDE && !v) ? put(q, "OFF") : put_ms(q, fom_env_ms((uint32_t)v));
            break;
        case FOM_K_POLY:
            q = put(put_u(q, 1u << (uint32_t)v), v ? " VOICES" : " VOICE");
            break;
        case FOM_K_QUALITY:
            q = put(q, fom_clouds_lofi() ? (v ? "LO" : "LO AUTO") : "HI");   /* (AUTO: the route needs it) */
            break;
        case FOM_K_SEMI:
            *q++ = v < 0 ? '-' : v > 0 ? '+' : ' ';
            q = put(put_u(q, (uint32_t)(v < 0 ? -v : v)), " st");
            break;
        case FOM_K_CENTS: {
            int32_t c = v * 100 / 128;
            *q++ = c < 0 ? '-' : '+';
            q = put(put_u(q, (uint32_t)(c < 0 ? -c : c)), " ct");
            break;
        }
        case FOM_K_INDEX:
            q = put_u(q, (uint32_t)v);
            break;
        default:                                /* a bar: the knob says it */
            break;
        }
        *q = 0;
        if (q != s) cv_text_mid(cx, 61, &FONT_S, s, C_INK2, C_PANEL);
    }
    cv_blit(60u * k, Y_KNOBS);
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
    cv_begin(240, 18, C_PANEL);
    if (set.clouds) {                           /* (and Clouds' own share of the CPU, right) */
        char c[16], *q = put(c, "CLOUDS ");
        q = put(put_u(q, (clouds_shown + 5u) / 10u), "%");
        *q = 0;
        cv_text_on(8, 1, &FONT_S, s, C_INK2, C_PANEL);
        cv_text_on(232 - text_w(&FONT_S, c), 1, &FONT_S, c, C_INK2, C_PANEL);
    } else cv_text_mid(120, 1, &FONT_S, s, C_INK2, C_PANEL);
    cv_blit(0, Y_INFO);
}

/* the route: a small plate per module, a jack on each, patch cables between them and on to the OUT box; Clouds'
 * share of the CPU on its plate (LO: lo-fi; a red LED while frozen) */
static void draw_plate(int32_t x, int32_t y, int32_t w, const char *name, uint16_t bg)
{
    cv_rect(x, y, w, 30, C_INK2);
    cv_rect(x + 1, y + 1, w - 2, 28, bg);
    cv_rect(x + 1, y + 1, w - 2, 2, C_BAND);    /* (a hint of the band) */
    cv_text_mid(x + w / 2, y + 4, &FONT_S, name, bg == C_DARK ? C_WHITE : C_INK, bg);
}
static void draw_jack(int32_t cx, int32_t cy)
{
    cv_disc(cx, cy, 4, C_INK2);
    cv_disc(cx, cy, 2, C_INK);
}
static void draw_cable(int32_t x0, int32_t x1, int32_t y, uint16_t c)   /* a sagging cable, jack to jack */
{
    int32_t i, px = x0, py = y;
    for (i = 1; i <= 16; i++) {                 /* a parabola, 7 px down at the middle */
        int32_t x = x0 + (x1 - x0) * i / 16, t = i * (16 - i);
        int32_t yy = y + t * 7 / 64;
        cv_line(px, py, x, yy, c); cv_line(px, py + 1, x, yy + 1, c); cv_line(px, py + 2, x, yy + 2, c);
        px = x; py = yy;
    }
    cv_disc(x0, y + 1, 3, c);
    cv_disc(x1, y + 1, 3, c);
}
static void draw_route(void)
{
    static const char *const NAME[3] = {"Braids", "Rings", "Clouds"};
    static const uint16_t CABLE[3] = {C_PINK, RGB(240, 150, 20), C_TEAL};
    uint8_t m[3];
    const char *label[3];
    int32_t w[3], x, gap, total = 0, jack_out = 0, y = 3;
    uint32_t i, n = route_modules(m);
    for (i = 0; i < n; i++) {
        label[i] = m[i] == M_CLOUDS && fom_clouds_lofi() ? "Clouds lo" : NAME[m[i]];
        w[i] = (int32_t)text_w(&FONT_S, label[i]) + 12;
        total += w[i];
    }
    /* the plates, then the OUT box (with three plates, the last cable runs off the edge to it), cables between */
    gap = (240 - 12 - (n < 3u ? 36 : 4) - total) / (int32_t)n;
    if (gap > 26) gap = 26;
    cv_begin(240, 38, C_PANEL);
    cv_rect(0, 0, 240, 1, C_BAND);
    x = 6;
    for (i = 0; i < n; i++) {
        draw_plate(x, y, w[i], label[i], C_PANEL);
        if (i) {
            draw_jack(x + 7, y + 24);
            draw_cable(jack_out, x + 7, y + 23, CABLE[i - 1]);
        }
        if (m[i] == M_CLOUDS && frozen) cv_disc(x + w[i] - 6, y + 7, 3, C_PINK);
        draw_jack(x + w[i] - 8, y + 24);
        jack_out = x + w[i] - 8;
        x += w[i] + gap;
    }
    if (n < 3u) {
        draw_plate(198, y, 36, "OUT", C_DARK);
        draw_jack(216, y + 24);
        draw_cable(jack_out, 216, y + 23, CABLE[2]);
    } else {
        draw_cable(jack_out, 238, y + 23, CABLE[2]);
    }
    cv_blit(0, Y_ROUTE);
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
    cv_begin(240, 22, C_DARK);
    cv_text_mid(120, 3, &FONT_S, s, cpu >= 800u ? RGB(255, 170, 170) : C_WHITE, C_DARK);
    cv_blit(0, Y_STATUS);
    {   /* and as a bar on the bottom two rows, 2.4 px a percent: a glance, and tests/emu_smoke.mjs reads it */
        uint32_t w = cpu * 240u / 1000u;
        if (w > 240u) w = 240u;
        lcd_fill(0, 238, 240, 2, C_BLACK);
        if (w) lcd_fill(0, 238, w, 2, cpu >= 800u ? C_PINK : C_INK2);
    }
}

/* ---- the route: the engine (SELECT), Clouds after it or not (FX); a MIDI program change sets both */
static uint32_t engine_changed;                 /* set when it changes: the main loop saves it */
static void set_route(uint32_t e, uint32_t clouds)
{
    if (e >= FOM_NENGINES || clouds > 1u || (e == set.engine && clouds == set.clouds)) return;
    if (e != set.engine) { all_off(); set.page = 0; }
    set.engine = (uint8_t)e;
    set.clouds = (uint8_t)clouds;
    if (set.page >= npages()) set.page = 0;
    fom_set_engine(set.engine);
    fom_set_clouds(set.clouds);
    if (!clouds && frozen) { frozen = 0; fom_set_freeze(0); }
    engine_changed = 1;
}
static void set_engine(uint32_t e) { set_route(e, set.clouds); }

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
        else if (ty == 0xC0u && d1 < 6u) {                   /* program change: the route */
            static const uint8_t ROUTE[6][2] = {{FOM_E_BRAIDS, 0}, {FOM_E_RINGS, 0}, {FOM_E_BRAIDS, 1},
                                                {FOM_E_RINGS, 1}, {FOM_E_BRAIDS_RINGS, 0}, {FOM_E_BRAIDS_RINGS, 1}};
            set_route(ROUTE[d1][0], ROUTE[d1][1]);
        }
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
    uint32_t head_drawn = ~0u, tabs_drawn = ~0u, route_drawn = ~0u, row_drawn[4], cpu_shown = 0;
    uint32_t t_cpu = 0;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;
    fom_init();
    fom_set_ticks(fm1_ticks);                   /* (Clouds' share of the CPU, for the screen) */
    if (settings_load(&set, sizeof set) || !set_valid())
        set_defaults();
    if (set.page >= npages()) set.page = 0;
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
        if (b & (1u << BTN_EDIT)) { set.page = (uint8_t)((set.page + 1u) % npages()); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1; t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {   /* the engine */
            int32_t c = (int32_t)set.engine + e;
            set_engine((uint32_t)(c < 0 ? 0 : c >= (int32_t)FOM_NENGINES ? (int32_t)FOM_NENGINES - 1 : c));
        }
        if (b & (1u << BTN_FX)) set_route(set.engine, !set.clouds);   /* Clouds after the engine, or not */
        if (b & (1u << BTN_REC) && set.clouds) { frozen = !frozen; fom_set_freeze((int)frozen); }
        if (engine_changed) { engine_changed = 0; dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {   /* Braids' shape, Rings' model */
            /* Rings' model on Rings' pages (and wherever Braids is not), else Braids' shape */
            knob_turn(shown_module() == M_RINGS || set.engine == FOM_E_RINGS ? FOM_P_MODEL : FOM_P_SHAPE, e);
            dirty = 1; t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++)
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0 && page_knob(set.page, k) != A_NONE) {
                knob_turn(page_knob(set.page, k), e); dirty = 1; t_change = fm1_ms;
            }

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */

        if (fm1_ms - t_cpu >= 500u) {           /* the CPU meter: the peak of the last half second */
            t_cpu = fm1_ms;
            cpu_shown = cpu_peak;
            cpu_peak = cpu_now;
            clouds_shown = clouds_peak;
            clouds_peak = 0;
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
        fm1_led_key(BTN_FX, set.clouds);
        fm1_led_key(BTN_REC, frozen);
        fm1_led_key(BTN_PLAY, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));   /* lit: running; a blink a beat */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows red while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 128 : 8, on ? 20 : 1, on ? 20 : 1);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if ((st = shown_module() | (uint32_t)set.p[FOM_P_SHAPE] << 2 | (uint32_t)set.p[FOM_P_MODEL] << 8 |
                  (uint32_t)set.p[FOM_P_POLY] << 10 | frozen << 12 | (uint32_t)fom_clouds_lofi() << 13) != head_drawn) {
            head_drawn = st;
            draw_head();
            for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;   /* TIMBRE's and COLOR's names follow the shape */
        }
        if ((st = set.engine | set.clouds << 4 | frozen << 5 | (uint32_t)fom_clouds_lofi() << 6) != route_drawn) {
            route_drawn = st;
            draw_route();
        }
        if ((st = set.page | set.engine << 4 | set.clouds << 8) != tabs_drawn) {
            tabs_drawn = st;
            draw_tabs();
            for (k = 0; k < 4u; k++) row_drawn[k] = ~0u;
        }
        for (k = 0; k < 4u; k++)
            if ((st = (uint32_t)(param_get(page_knob(set.page, k)) & 0xFFFF)) != row_drawn[k]) {
                row_drawn[k] = st;
                draw_knob(k);
            }
        if ((st = last_note | midi_src << 8 | clk_bpm << 10 | clk_run << 20 | set.clouds << 21 |
                  ((clouds_shown + 5u) / 10u) << 22) != info_drawn) {
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
