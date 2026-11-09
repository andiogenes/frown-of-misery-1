/* SPDX-License-Identifier: MIT */
#include "fom_engine.h"

#include "fm1_braids.h"
#include "fm1_shapes.h"
#include "fom_rings.h"
#include "fom_clouds.h"
#include "fom_marbles.h"

static_assert(FM1_NSHAPES == braids::MACRO_OSC_SHAPE_LAST, "fm1_shapes.h is out of step with Braids' shapes");

/* Four MacroOscillators are ~69 KB: too much for .bss (96 KB, shared with the platform), so on the device they
 * live in .pool, which the boot zeroes. Init() sets everything anyway. On the host this is a plain static. */
#if defined(__pi32v2__) || defined(FM1_TARGET)
#define FOM_POOL __attribute__((section(".pool")))
#else
#define FOM_POOL
#endif

namespace {

const fom_param_info_t kParams[FOM_NPARAM] = {
    /* FOM_P_SHAPE   */ {"SHAPE", 0, FM1_NSHAPES - 1, 0, FOM_K_INDEX},
    /* FOM_P_TIMBRE  */ {"TIMBRE", 0, 32767, 8192, FOM_K_BAR},
    /* FOM_P_COLOR   */ {"COLOR", 0, 32767, 0, FOM_K_BAR},
    /* FOM_P_FINE    */ {"FINE", -64, 63, 0, FOM_K_CENTS},
    /* FOM_P_LEVEL   */ {"LEVEL", 0, 32767, 32767, FOM_K_BAR},
    /* FOM_P_ATTACK  */ {"ATTACK", 0, FOM_ENV_MAX, 7, FOM_K_TIME},
    /* FOM_P_RELEASE */ {"REL", 0, FOM_ENV_MAX, 13, FOM_K_TIME},
    /* FOM_P_GLIDE   */ {"GLIDE", 0, FOM_ENV_MAX, 0, FOM_K_TIME},
    /* Rings' knobs and their defaults (Resonator::Init, Part) */
    /* FOM_P_STRUCTURE  */ {"STRUCT", 0, 32767, 8192, FOM_K_BAR},
    /* FOM_P_BRIGHTNESS */ {"BRIGHT", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_DAMPING    */ {"DAMP", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_POSITION   */ {"POSN", 0, 32767, 9830, FOM_K_BAR},
    /* FOM_P_POLY       */ {"POLY", 0, 2, 0, FOM_K_POLY},
    /* FOM_P_MODEL      */ {"MODEL", 0, 2, 0, FOM_K_INDEX},
    /* Clouds' knobs and its defaults (dsp/ref/ref_clouds.cc's, the module's at noon) */
    /* FOM_P_C_POSITION */ {"POSN", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_C_SIZE     */ {"SIZE", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_C_DENSITY  */ {"DENSTY", 0, 32767, 22938, FOM_K_BAR},
    /* FOM_P_C_TEXTURE  */ {"TEXTUR", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_C_BLEND    */ {"BLEND", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_C_SPREAD   */ {"SPREAD", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_C_FEEDBACK */ {"FDBK", 0, 32767, 0, FOM_K_BAR},
    /* FOM_P_C_REVERB   */ {"REVERB", 0, 32767, 9830, FOM_K_BAR},
    /* FOM_P_C_PITCH    */ {"PITCH", -24, 24, 0, FOM_K_SEMI},
    /* FOM_P_C_QUALITY  */ {"QUALTY", 0, 1, 0, FOM_K_QUALITY},
    /* FOM_P_R_LEVEL    */ {"LEVEL", 0, 32767, 32767, FOM_K_BAR},
    /* Marbles' knobs, at its defaults (noon; DEJA VU off, LENGTH 8) */
    /* FOM_P_M_RATE     */ {"RATE", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_M_TBIAS    */ {"BIAS", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_M_JITTER   */ {"JITTER", 0, 32767, 0, FOM_K_BAR},
    /* FOM_P_M_MODEL    */ {"MODEL", 0, 6, 0, FOM_K_LIST},
    /* FOM_P_M_SPREAD   */ {"SPREAD", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_M_XBIAS    */ {"BIAS", 0, 32767, 16384, FOM_K_BAR},
    /* FOM_P_M_STEPS    */ {"STEPS", 0, 32767, 26000, FOM_K_BAR},
    /* FOM_P_M_SCALE    */ {"SCALE", 0, 5, 0, FOM_K_LIST},
    /* FOM_P_M_DEJAVU   */ {"DEJAVU", 0, 32767, 0, FOM_K_BAR},
    /* FOM_P_M_LENGTH   */ {"LENGTH", 0, 32767, 22000, FOM_K_BAR},
    /* FOM_P_M_XRANGE   */ {"RANGE", 0, 2, 0, FOM_K_LIST},
    /* FOM_P_M_TRANGE   */ {"T RNG", 0, 2, 1, FOM_K_LIST},
    /* FOM_P_M_YDEST    */ {"Y TO", 0, FOM_Y_N - 1, 0, FOM_K_LIST},
    /* FOM_P_M_YAMOUNT  */ {"Y AMT", 0, 32767, 8192, FOM_K_BAR},
    /* FOM_P_M_YRATE    */ {"Y RATE", 0, 32767, 16384, FOM_K_BAR},
};

/* Glide: a one-pole slew of the pitch, updated every kSub frames. kGlideK[i] = 65536 (1 - e^(-kSub / 2^i)),
 * computed on a computer (no floating point here). */
const uint32_t kSub = 32;
const int32_t kGlideK[FOM_ENV_MAX + 1] = {
    65536, 65536, 65514, 64336, 56667, 41427, 25786, 14497, 7701, 3971, 2016,
    1016, 510, 256, 128, 64, 32, 16, 8, 4, 2};

struct Voice {
    volatile uint32_t note;     /* MIDI note */
    volatile int32_t gate;      /* target level, Q15; 0 = released */
    volatile uint32_t strike;   /* set by the main loop, taken by the ISR */
    volatile uint32_t jump;     /* set with a struck note when gliding is off: the pitch goes there at once */
    int32_t amp;                /* the level now, Q23 (gate << 8): glides to gate in steps fine enough for 24 s */
    int32_t pitch;              /* the pitch now, 1/128 semitone << 8 (glides to the note) */
    volatile int32_t detune;    /* 1/128 semitone above the note (Marbles' X between semitones) */
};

FOM_POOL fm1::BraidsVoice g_osc[FOM_NV];
FOM_POOL fom::RingsModal g_rings;
FOM_POOL fom::RingsStrings g_strings;
/* the string models' delay lines: 8 strings of 2048 samples, and the dispersion's allpass for 4 (80 KB) */
FOM_POOL int32_t g_string_lines[fom::kNumStrings * fom::kStringDelay];
FOM_POOL int32_t g_string_stretch[fom::kMaxPolyphony * fom::kStringDelay / 2];

/* Clouds: its recording buffers (Clouds' own size, 128 KB) in .pool, the reverb's and the diffuser's memory and the
 * processor in .bss; the engine renders into g_dry first when Clouds follows */
FOM_POOL int16_t g_clouds_buf[2 * fom::kCloudsBufferSamples];
int32_t g_clouds_diffuser[fom::kCloudsDiffuserSize];
uint16_t g_clouds_reverb[fom::kCloudsReverbSize];
fom::CloudsFx g_clouds;
int32_t g_dry[2 * FOM_BLOCK_MAX];
volatile uint32_t g_clouds_on, g_freeze;
uint32_t g_clouds_quiet = 0xFFFFFFFFu;          /* frames since Clouds' output last moved (ISR) */
/* Clouds' grain limit: 32 after Braids, 16 after Rings, 8 after its sympathetic strings (the heaviest model); and
 * lower still for a while if the audio interrupt runs short of time (fom_set_load) */
volatile uint32_t g_load;                        /* the last block's load, per mille of the interrupt's budget */
int32_t g_guard;                                 /* grains taken off by the guard */
uint32_t g_guard_calm;                           /* blocks since the load was last high */
uint32_t (*g_ticks)(void);                       /* the app's timer, for Clouds' share */
/* Marbles */
volatile uint32_t g_marbles_on, g_marbles_root = 48u;
volatile int32_t g_marbles_clock = -1;
volatile uint32_t g_marbles_gates;
volatile int32_t g_marbles_y;
volatile uint32_t g_entropy, g_entropy_new;
uint32_t g_marbles_prev;                          /* the gates at the last step */
/* Y's target and its offset now (in the target's own units), read by the renders through pv() */
volatile int32_t g_mod_param = -1, g_mod_value;
volatile uint32_t g_clouds_ticks;

/* Rings' strikes, from the main loop to the ISR: a ring, one writer each side (w by the main loop, r by the ISR) */
volatile uint32_t g_strike_w, g_strike_r;
volatile uint8_t g_strike_note[8], g_strike_vel[8];
FOM_POOL int16_t g_buf[kSub];
Voice g_v[FOM_NV];

volatile int32_t g_p[FOM_NPARAM];
volatile uint32_t g_engine;
volatile int32_t g_bend;
uint32_t g_shape_now = 0xFFFFFFFFu;

}  // namespace

extern "C" {

const fom_param_info_t *fom_param_info(uint32_t p) { return p < FOM_NPARAM ? &kParams[p] : 0; }

void fom_init(void)
{
    uint32_t v, p;
    for (v = 0; v < FOM_NV; v++) {
        g_osc[v].Init();
        g_v[v].note = 60u;
        g_v[v].detune = 0;
        g_v[v].gate = 0;
        g_v[v].strike = 0;
        g_v[v].jump = 1;
        g_v[v].amp = 0;
        g_v[v].pitch = 60 * fm1::kPitchPerSemitone * 256;
    }
    for (p = 0; p < FOM_NPARAM; p++) g_p[p] = kParams[p].def;
    g_engine = FOM_E_BRAIDS;
    g_bend = 0;
    g_rings.Init();
    g_strings.Init(g_string_lines, g_string_stretch);
    g_strike_w = g_strike_r = 0;
    g_shape_now = 0xFFFFFFFFu;
    g_clouds.Init(g_clouds_buf, g_clouds_diffuser, g_clouds_reverb);
    g_clouds_on = g_freeze = 0;
    g_load = 0; g_guard = 0; g_guard_calm = 0;
    g_marbles_on = 0; g_marbles_prev = 0; g_marbles_gates = 0; g_mod_param = -1;
    fom::marbles_init(0x5EED1234u);
    g_clouds_ticks = 0;
    g_clouds_quiet = 0xFFFFFFFFu;
}

void fom_set_engine(uint32_t e) { g_engine = e < FOM_NENGINES ? e : 0u; }
void fom_set_clouds(int on) { g_clouds_on = on ? 1u : 0u; }
void fom_set_freeze(int on) { g_freeze = on ? 1u : 0u; }
void fom_set_load(uint32_t permille) { g_load = permille; }

void fom_set_marbles(int on)
{
    if (!on && g_marbles_on) {                  /* its notes stop with it */
        g_marbles_on = 0;
        for (uint32_t v = 0; v < 3u; v++) g_v[v].gate = 0;
        g_mod_param = -1;
        g_marbles_gates = 0;
        return;
    }
    g_marbles_on = on ? 1u : 0u;
}
void fom_marbles_root(uint32_t note) { g_marbles_root = note > 127u ? 127u : note; }
void fom_marbles_entropy(uint32_t word) { g_entropy = word; g_entropy_new = 1u; }
void fom_marbles_clock(int level) { g_marbles_clock = level; }
uint32_t fom_marbles_gates(void) { return g_marbles_gates; }
int32_t fom_marbles_y(void) { return g_marbles_y; }
void fom_set_ticks(uint32_t (*ticks)(void)) { g_ticks = ticks; }
uint32_t fom_clouds_ticks(void) { return g_clouds_ticks; }
int fom_clouds_lofi(void)
{
    /* LO by itself where HI does not fit: after the sympathetic strings, and after Braids > Rings */
    return g_p[FOM_P_C_QUALITY] || g_engine == FOM_E_BRAIDS_RINGS || (g_engine == FOM_E_RINGS && g_p[FOM_P_MODEL] == 1);
}

void fom_strike(uint32_t note, uint32_t velocity)
{
    uint32_t w = g_strike_w;
    if (w - g_strike_r >= 8u) return;           /* full: dropped */
    g_strike_note[w & 7u] = (uint8_t)(note > 127u ? 127u : note);
    g_strike_vel[w & 7u] = (uint8_t)(velocity > 127u ? 127u : velocity ? velocity : 1u);
    g_strike_w = w + 1u;                        /* (published last: the ISR reads the entry only after this) */
}

int fom_sounding(void)
{
    uint32_t v;
    if (g_clouds_on && g_clouds_quiet < 44100u) return 1;      /* Clouds' grains and reverb still moving */
    if (g_engine != FOM_E_BRAIDS &&
        ((g_p[FOM_P_MODEL] ? g_strings.sounding() : g_rings.sounding()) || g_strike_w != g_strike_r))
        return 1;
    if (g_engine == FOM_E_RINGS) return 0;
    for (v = 0; v < FOM_NV; v++)
        if (g_v[v].gate || g_v[v].amp) return 1;
    return 0;
}

/* a parameter as the renders take it: its value plus Marbles' Y if Y is on it, within its range */
static int32_t pv(uint32_t p)
{
    int32_t v = g_p[p];
    if ((int32_t)p == g_mod_param) {
        v += g_mod_value;
        if (v < kParams[p].min) v = kParams[p].min;
        if (v > kParams[p].max) v = kParams[p].max;
    }
    return v;
}

static uint32_t q16(int32_t v) { return ((uint32_t)v << 1) | ((uint32_t)v >> 14); }   /* 0..32767 -> 0..65535 */

static void render_rings(int32_t* out, uint32_t n, const int32_t* in)   /* in: Rings' audio input, Q24, or null */
{
    static const int32_t kPoly[3] = {1, 2, 4};
    fom::RingsPatch patch = {q16(pv(FOM_P_STRUCTURE)), q16(pv(FOM_P_BRIGHTNESS)), q16(pv(FOM_P_DAMPING)),
                             q16(pv(FOM_P_POSITION))};
    int32_t fine = g_p[FOM_P_FINE] + g_bend;
    if (g_p[FOM_P_MODEL]) {                     /* the string models: their own 24-sample grid, strikes queued */
        g_strings.set_model(g_p[FOM_P_MODEL]);
        g_strings.set_polyphony(kPoly[g_p[FOM_P_POLY]]);
        g_strings.set_fine(fine);
        for (uint32_t s0 = 0; s0 < n; s0 += 32u) {
            uint32_t r = g_strike_r;
            if (r != g_strike_w) {
                g_strings.Strike(g_strike_note[r & 7u], fine, g_strike_vel[r & 7u]);
                g_strike_r = r + 1u;
            }
            g_strings.Render(patch, g_p[FOM_P_R_LEVEL], out + 2u * s0, n - s0 < 32u ? n - s0 : 32u, in ? in + s0 : 0);
        }
        return;
    }
    g_rings.set_polyphony(kPoly[g_p[FOM_P_POLY]]);
    g_rings.set_fine(fine);
    /* At most one strike per 32 frames, as Rings takes one strum per 24-sample block: the struck voice is the
     * active one, which alone gets the exciter. Four strikes in one block would leave three voices silent. A chord
     * spreads over ~2 ms. */
    for (uint32_t s0 = 0; s0 < n; s0 += 32u) {
        uint32_t r = g_strike_r;
        if (r != g_strike_w) {
            g_rings.Strike(g_strike_note[r & 7u], fine, g_strike_vel[r & 7u]);
            g_strike_r = r + 1u;
        }
        g_rings.Render(patch, g_p[FOM_P_R_LEVEL], out + 2u * s0, n - s0 < 32u ? n - s0 : 32u, in ? in + s0 : 0);
    }
}

void fom_set_param(uint32_t p, int32_t v)
{
    if (p >= FOM_NPARAM) return;
    g_p[p] = v < kParams[p].min ? kParams[p].min : v > kParams[p].max ? kParams[p].max : v;
}

uint32_t fom_env_ms(uint32_t i) { return i > FOM_ENV_MAX ? 0u : ((1u << i) * 10u + 220u) / 441u; }
void fom_set_bend(int32_t b) { g_bend = b; }

void fom_voice_play(uint32_t v, uint32_t note, int32_t gate, int strike)
{
    if (v >= FOM_NV || note > 127u) return;
    g_v[v].note = note;                         /* the pitch first, then the gate: safe to see half-done */
    g_v[v].detune = 0;
    if (strike) {
        if (!g_p[FOM_P_GLIDE] || (!g_v[v].gate && !g_v[v].amp)) g_v[v].jump = 1u;   /* from silence: no glide in */
        g_v[v].strike = 1u;
    }
    g_v[v].gate = gate < 0 ? 0 : gate > 32767 ? 32767 : gate;
}

void fom_voice_release(uint32_t v)
{
    if (v < FOM_NV) g_v[v].gate = 0;
}

int fom_voice_sounding(uint32_t v)
{
    return v < FOM_NV && (g_v[v].gate || g_v[v].amp);
}

static void render_engine(int32_t *out, uint32_t n);

static void render_block(int32_t *out, uint32_t n);
static void marbles_tick(void);

void fom_render_add(int32_t *out, uint32_t n)
{
    if (n > FOM_BLOCK_MAX) n = FOM_BLOCK_MAX;
    g_clouds_ticks = 0;
    if (!g_marbles_on) { render_block(out, n); return; }
    for (uint32_t s0 = 0; s0 < n; s0 += fom::kMarblesStep) {   /* Marbles every 32 samples, the notes in between */
        marbles_tick();
        render_block(out + 2u * s0, n - s0 < fom::kMarblesStep ? n - s0 : fom::kMarblesStep);
    }
}

static void render_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (!g_clouds_on) { render_engine(out, n); return; }
    for (i = 0; i < 2u * n; i++) g_dry[i] = 0;
    render_engine(g_dry, n);
    uint32_t t0 = g_ticks ? g_ticks() : 0;
    fom::CloudsPatch cp;
    cp.position = q16(pv(FOM_P_C_POSITION)); cp.size = q16(pv(FOM_P_C_SIZE));
    cp.density = q16(pv(FOM_P_C_DENSITY)); cp.texture = q16(pv(FOM_P_C_TEXTURE));
    cp.blend = q16(g_p[FOM_P_C_BLEND]); cp.spread = q16(g_p[FOM_P_C_SPREAD]);
    cp.feedback = q16(g_p[FOM_P_C_FEEDBACK]); cp.reverb = q16(g_p[FOM_P_C_REVERB]);
    cp.pitch = g_p[FOM_P_C_PITCH] << 16;
    cp.freeze = g_freeze != 0;
    int lofi = fom_clouds_lofi();
    g_clouds.set_lofi(lofi != 0);
    /* the most grains: Clouds' own after Braids (32, LO 46); after Rings 16 (LO 24); after the sympathetic strings and
     * after Braids > Rings 8 */
    int32_t cap = g_engine == FOM_E_BRAIDS ? (lofi ? 46 : 32)
                : g_engine == FOM_E_BRAIDS_RINGS || g_p[FOM_P_MODEL] == 1 ? 8 : (lofi ? 24 : 16);
    if (g_load > 850u) { g_guard += 4; g_guard_calm = 0; }  /* over 85 %: four grains fewer at once */
    else if (g_guard && g_load < 700u && ++g_guard_calm >= 64u) { g_guard--; g_guard_calm = 0; }   /* back slowly */
    if (g_guard > cap - 4) g_guard = cap - 4;
    g_clouds.set_max_grains(cap - g_guard);
    g_clouds.Process(cp, g_dry, n);
    int32_t peak = 0;
    for (i = 0; i < 2u * n; i++) {
        int32_t s = g_dry[i];
        out[i] += s;
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    g_clouds_quiet = peak > 8 ? 0u : g_clouds_quiet < 0x80000000u ? g_clouds_quiet + n : g_clouds_quiet;
    g_clouds_ticks += g_ticks ? g_ticks() - t0 : 0;
}

static void render_braids(int32_t *out, uint32_t n);

/* one Marbles step (the ISR, every 32 samples): its gates play voices 1..3, X their pitches, Y its parameter */
static void marbles_tick(void)
{
    static const uint8_t kYParam[FOM_Y_N] = {0, FOM_P_TIMBRE, FOM_P_COLOR, FOM_P_STRUCTURE, FOM_P_BRIGHTNESS,
                                             FOM_P_DAMPING, FOM_P_POSITION, FOM_P_C_POSITION, FOM_P_C_SIZE,
                                             FOM_P_C_DENSITY, FOM_P_C_TEXTURE};
    fom::MarblesPatch mp;
    fom::MarblesOut mo;
    if (g_entropy_new) { g_entropy_new = 0; fom::marbles_entropy(g_entropy); }
    mp.t_rate = q16(g_p[FOM_P_M_RATE]); mp.t_bias = q16(g_p[FOM_P_M_TBIAS]); mp.t_jitter = q16(g_p[FOM_P_M_JITTER]);
    mp.deja_vu = q16(g_p[FOM_P_M_DEJAVU]); mp.length = q16(g_p[FOM_P_M_LENGTH]);
    mp.x_spread = q16(g_p[FOM_P_M_SPREAD]); mp.x_bias = q16(g_p[FOM_P_M_XBIAS]); mp.x_steps = q16(g_p[FOM_P_M_STEPS]);
    mp.y_rate = q16(g_p[FOM_P_M_YRATE]);
    mp.t_model = (uint8_t)g_p[FOM_P_M_MODEL]; mp.t_range = (uint8_t)g_p[FOM_P_M_TRANGE];
    mp.x_range = (uint8_t)g_p[FOM_P_M_XRANGE]; mp.x_scale = (uint8_t)g_p[FOM_P_M_SCALE]; mp.x_mode = 0;
    mp.t_deja_vu = mp.x_deja_vu = true;         /* (DEJA VU at 0 is the buttons off) */
    fom::marbles_step(mp, g_marbles_clock, &mo);
    uint32_t rise = mo.gates & ~g_marbles_prev, fall = g_marbles_prev & ~mo.gates;
    g_marbles_prev = mo.gates;
    g_marbles_gates = mo.gates;
    g_marbles_y = mo.y;
    for (uint32_t v = 0; v < 3u; v++) {
        if (rise >> v & 1u) {
            int32_t pitch = (int32_t)g_marbles_root * 256 + mo.x[v];   /* semitones Q8 */
            if (pitch < 0) pitch = 0;
            if (pitch > 127 * 256) pitch = 127 * 256;
            uint32_t note = (uint32_t)(pitch >> 8);
            if (g_engine != FOM_E_RINGS) {
                Voice &vc = g_v[v];
                vc.note = note;
                vc.detune = (pitch & 255) >> 1;  /* (Q8 -> 1/128 semitone) */
                if (!g_p[FOM_P_GLIDE] || (!vc.gate && !vc.amp)) vc.jump = 1u;
                vc.strike = 1u;
                vc.gate = 11430;                 /* (a key's velocity 100 in the app) */
            }
            if (g_engine != FOM_E_BRAIDS) {      /* Rings: a strike (the ISR is the queue's only writer now) */
                if (g_p[FOM_P_MODEL]) g_strings.Strike(note, g_p[FOM_P_FINE] + g_bend, 100);
                else g_rings.Strike(note, g_p[FOM_P_FINE] + g_bend, 100);
            }
        }
        if ((fall >> v & 1u) && g_engine != FOM_E_RINGS) g_v[v].gate = 0;
    }
    /* Y: -5..+5 V over the parameter's whole range, times the amount */
    int32_t dest = g_p[FOM_P_M_YDEST];
    if (dest > 0 && dest < FOM_Y_N) {
        int32_t p = kYParam[dest], range = kParams[p].max - kParams[p].min;
        g_mod_value = (int32_t)(((int64_t)mo.y * range * g_p[FOM_P_M_YAMOUNT]) >> 31);
        g_mod_param = p;
    } else {
        g_mod_param = -1;
    }
}
static int32_t g_rings_in[FOM_BLOCK_MAX];               /* Braids into Rings: its audio input, Q24 */
static int32_t g_braids_out[2 * FOM_BLOCK_MAX];

static void render_engine(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (g_engine == FOM_E_RINGS) { render_rings(out, n, 0); return; }
    if (g_engine == FOM_E_BRAIDS) { render_braids(out, n); return; }
    for (i = 0; i < 2u * n; i++) g_braids_out[i] = 0;          /* Braids > Rings: Braids, mono, into Rings' input */
    render_braids(g_braids_out, n);
    for (i = 0; i < n; i++) {
        int32_t x = g_braids_out[2u * i];
        x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;       /* (Rings' codec takes no more) */
        g_rings_in[i] = x << 9;
    }
    render_rings(out, n, g_rings_in);
}

static void render_braids(int32_t *out, uint32_t n)
{
    uint32_t v, i, s0, shape = (uint32_t)g_p[FOM_P_SHAPE];
    int16_t timbre = (int16_t)pv(FOM_P_TIMBRE), color = (int16_t)pv(FOM_P_COLOR);
    int32_t bend = g_bend + g_p[FOM_P_FINE], level = g_p[FOM_P_LEVEL];
    int32_t up = 0x7FFF00 >> g_p[FOM_P_ATTACK], down = 0x7FFF00 >> g_p[FOM_P_RELEASE];   /* Q23 per frame */
    int32_t gk = kGlideK[g_p[FOM_P_GLIDE]];
    for (v = 0; v < FOM_NV; v++) {
        Voice &vc = g_v[v];
        fm1::BraidsVoice &osc = g_osc[v];
        int32_t amp = vc.amp, g = vc.gate << 8, target;
        if (shape != g_shape_now)
            osc.set_shape((braids::MacroOscillatorShape)shape);
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        target = ((int32_t)vc.note * fm1::kPitchPerSemitone + vc.detune) << 8;
        if (vc.jump) { vc.jump = 0; vc.pitch = target; }
        osc.set_parameters(timbre, color);
        if (vc.strike) { vc.strike = 0; osc.Strike(); }
        for (s0 = 0; s0 < n; s0 += kSub) {      /* sub-blocks: the glide moves the pitch every kSub frames */
            uint32_t m = n - s0 < kSub ? n - s0 : kSub;
            int32_t pitch;
            vc.pitch += (int32_t)(((int64_t)(target - vc.pitch) * gk) >> 16);
            pitch = (vc.pitch >> 8) + bend;
            osc.set_pitch((int16_t)(pitch < 0 ? 0 : pitch > 16383 ? 16383 : pitch));
            osc.Render(g_buf, m);
            for (i = 0; i < m; i++) {
                int32_t s;
                if (amp < g) { amp += up; if (amp > g) amp = g; }
                else if (amp > g) { amp -= down; if (amp < g) amp = g; }
                s = ((((int32_t)g_buf[i] * (amp >> 8)) >> 15) * level) >> 15;
                out[2u * (s0 + i)] += s;
                out[2u * (s0 + i) + 1u] += s;
            }
        }
        vc.amp = amp;
    }
    g_shape_now = shape;
}

}  // extern "C"
