/* SPDX-License-Identifier: MIT */
#include "fom_engine.h"

#include "fm1_braids.h"
#include "fm1_shapes.h"

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
    g_shape_now = 0xFFFFFFFFu;
}

void fom_set_engine(uint32_t e) { g_engine = e < FOM_NENGINES ? e : 0u; }

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

void fom_render_add(int32_t *out, uint32_t n)
{
    uint32_t v, i, s0, shape = (uint32_t)g_p[FOM_P_SHAPE];
    int16_t timbre = (int16_t)g_p[FOM_P_TIMBRE], color = (int16_t)g_p[FOM_P_COLOR];
    int32_t bend = g_bend + g_p[FOM_P_FINE], level = g_p[FOM_P_LEVEL];
    int32_t up = 0x7FFF00 >> g_p[FOM_P_ATTACK], down = 0x7FFF00 >> g_p[FOM_P_RELEASE];   /* Q23 per frame */
    int32_t gk = kGlideK[g_p[FOM_P_GLIDE]];
    if (n > FOM_BLOCK_MAX) n = FOM_BLOCK_MAX;
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
