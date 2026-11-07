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

struct Voice {
    volatile uint32_t note;     /* MIDI note */
    volatile int32_t gate;      /* target level, Q15; 0 = released */
    volatile uint32_t strike;   /* set by the main loop, taken by the ISR */
    int32_t amp;                /* the level now, Q23 (gate << 8): glides to gate in steps fine enough for 24 s */
};

FOM_POOL fm1::BraidsVoice g_osc[FOM_NV];
FOM_POOL int16_t g_buf[FOM_BLOCK_MAX];
Voice g_v[FOM_NV];

volatile uint32_t g_shape, g_timbre, g_color;
volatile uint32_t g_attack = 7u, g_release = 11u;     /* envelope indices */
volatile int32_t g_bend;
uint32_t g_shape_now = 0xFFFFFFFFu;

}  // namespace

extern "C" {

void fom_init(void)
{
    uint32_t v;
    for (v = 0; v < FOM_NV; v++) {
        g_osc[v].Init();
        g_v[v].note = 60u;
        g_v[v].gate = 0;
        g_v[v].strike = 0;
        g_v[v].amp = 0;
    }
    g_shape = braids::MACRO_OSC_SHAPE_CSAW;
    g_timbre = 0;
    g_color = 0;
    g_bend = 0;
    g_shape_now = 0xFFFFFFFFu;
}

void fom_set_shape(uint32_t shape) { g_shape = shape < FM1_NSHAPES ? shape : FM1_NSHAPES - 1u; }
void fom_set_timbre(uint32_t t) { g_timbre = t > 32767u ? 32767u : t; }
void fom_set_color(uint32_t c) { g_color = c > 32767u ? 32767u : c; }
void fom_set_env(uint32_t a, uint32_t r)
{
    g_attack = a > FOM_ENV_MAX ? FOM_ENV_MAX : a;
    g_release = r > FOM_ENV_MAX ? FOM_ENV_MAX : r;
}
uint32_t fom_env_ms(uint32_t i) { return i > FOM_ENV_MAX ? 0u : ((1u << i) * 10u + 220u) / 441u; }
void fom_set_bend(int32_t b) { g_bend = b; }

void fom_voice_play(uint32_t v, uint32_t note, int32_t gate, int strike)
{
    if (v >= FOM_NV || note > 127u) return;
    g_v[v].note = note;                         /* the pitch first, then the gate: safe to see half-done */
    if (strike) g_v[v].strike = 1u;
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
    uint32_t v, i, shape = g_shape;
    int16_t timbre = (int16_t)g_timbre, color = (int16_t)g_color;
    int32_t bend = g_bend, up = 0x7FFF00 >> g_attack, down = 0x7FFF00 >> g_release;   /* Q23 per frame */
    if (n > FOM_BLOCK_MAX) n = FOM_BLOCK_MAX;
    for (v = 0; v < FOM_NV; v++) {
        Voice &vc = g_v[v];
        fm1::BraidsVoice &osc = g_osc[v];
        int32_t amp = vc.amp, g = vc.gate << 8, pitch;
        if (shape != g_shape_now)
            osc.set_shape((braids::MacroOscillatorShape)shape);
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        pitch = (int32_t)vc.note * fm1::kPitchPerSemitone + bend;
        osc.set_pitch((int16_t)(pitch < 0 ? 0 : pitch > 16383 ? 16383 : pitch));
        osc.set_parameters(timbre, color);
        if (vc.strike) { vc.strike = 0; osc.Strike(); }
        osc.Render(g_buf, n);
        for (i = 0; i < n; i++) {
            int32_t s;
            if (amp < g) { amp += up; if (amp > g) amp = g; }
            else if (amp > g) { amp -= down; if (amp < g) amp = g; }
            s = ((int32_t)g_buf[i] * (amp >> 8)) >> 15;
            out[2u * i] += s;
            out[2u * i + 1u] += s;
        }
        vc.amp = amp;
    }
    g_shape_now = shape;
}

}  // extern "C"
