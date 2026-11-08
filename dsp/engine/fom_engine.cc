/* SPDX-License-Identifier: MIT */
#include "fom_engine.h"

#include "fm1_braids.h"
#include "fm1_shapes.h"
#include "fom_rings.h"
#include "fom_clouds.h"

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
    g_clouds_quiet = 0xFFFFFFFFu;
}

void fom_set_engine(uint32_t e) { g_engine = e < FOM_NENGINES ? e : 0u; }
void fom_set_clouds(int on) { g_clouds_on = on ? 1u : 0u; }
void fom_set_freeze(int on) { g_freeze = on ? 1u : 0u; }
void fom_set_load(uint32_t permille) { g_load = permille; }

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
    if (g_engine == FOM_E_RINGS)
        return (g_p[FOM_P_MODEL] ? g_strings.sounding() : g_rings.sounding()) || g_strike_w != g_strike_r;
    for (v = 0; v < FOM_NV; v++)
        if (g_v[v].gate || g_v[v].amp) return 1;
    return 0;
}

static uint32_t q16(int32_t v) { return ((uint32_t)v << 1) | ((uint32_t)v >> 14); }   /* 0..32767 -> 0..65535 */

static void render_rings(int32_t* out, uint32_t n)
{
    static const int32_t kPoly[3] = {1, 2, 4};
    fom::RingsPatch patch = {q16(g_p[FOM_P_STRUCTURE]), q16(g_p[FOM_P_BRIGHTNESS]), q16(g_p[FOM_P_DAMPING]),
                             q16(g_p[FOM_P_POSITION])};
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
            g_strings.Render(patch, g_p[FOM_P_LEVEL], out + 2u * s0, n - s0 < 32u ? n - s0 : 32u);
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
        g_rings.Render(patch, g_p[FOM_P_LEVEL], out + 2u * s0, n - s0 < 32u ? n - s0 : 32u);
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

void fom_render_add(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (n > FOM_BLOCK_MAX) n = FOM_BLOCK_MAX;
    if (!g_clouds_on) { render_engine(out, n); return; }
    for (i = 0; i < 2u * n; i++) g_dry[i] = 0;
    render_engine(g_dry, n);
    fom::CloudsPatch cp;
    cp.position = q16(g_p[FOM_P_C_POSITION]); cp.size = q16(g_p[FOM_P_C_SIZE]);
    cp.density = q16(g_p[FOM_P_C_DENSITY]); cp.texture = q16(g_p[FOM_P_C_TEXTURE]);
    cp.blend = q16(g_p[FOM_P_C_BLEND]); cp.spread = q16(g_p[FOM_P_C_SPREAD]);
    cp.feedback = q16(g_p[FOM_P_C_FEEDBACK]); cp.reverb = q16(g_p[FOM_P_C_REVERB]);
    cp.pitch = g_p[FOM_P_C_PITCH] << 16;
    cp.freeze = g_freeze != 0;
    int32_t cap = g_engine != FOM_E_RINGS ? 32 : g_p[FOM_P_MODEL] == 1 ? 8 : 16;
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
}

static void render_engine(int32_t *out, uint32_t n)
{
    uint32_t v, i, s0, shape = (uint32_t)g_p[FOM_P_SHAPE];
    int16_t timbre = (int16_t)g_p[FOM_P_TIMBRE], color = (int16_t)g_p[FOM_P_COLOR];
    int32_t bend = g_bend + g_p[FOM_P_FINE], level = g_p[FOM_P_LEVEL];
    int32_t up = 0x7FFF00 >> g_p[FOM_P_ATTACK], down = 0x7FFF00 >> g_p[FOM_P_RELEASE];   /* Q23 per frame */
    int32_t gk = kGlideK[g_p[FOM_P_GLIDE]];
    if (g_engine == FOM_E_RINGS) { render_rings(out, n); return; }
    for (v = 0; v < FOM_NV; v++) {
        Voice &vc = g_v[v];
        fm1::BraidsVoice &osc = g_osc[v];
        int32_t amp = vc.amp, g = vc.gate << 8, target;
        if (shape != g_shape_now)
            osc.set_shape((braids::MacroOscillatorShape)shape);
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        target = ((int32_t)vc.note * fm1::kPitchPerSemitone) << 8;
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
