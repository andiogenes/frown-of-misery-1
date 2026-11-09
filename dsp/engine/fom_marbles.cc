/* SPDX-License-Identifier: MIT */
#include "fom_marbles.h"

#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"
#include "stmlib/dsp/hysteresis_quantizer.h"
#include "stmlib/utils/gate_flags.h"

using namespace marbles;
using namespace stmlib;

namespace fom {

namespace {

const float kRate = 44100.0f / static_cast<float>(kMarblesStep);

/* From marbles/settings.cc and marbles/marbles.cc (MIT): the six scales, the loop lengths DEJA VU's LENGTH steps
 * through, the Y clock dividers. */
const Scale kPresetScales[6] = {
  // C major
  {
    1.0f,
    12,
    {
      { 0.0000f, 255 },  // C
      { 0.0833f, 16 },   // C#
      { 0.1667f, 96 },   // D
      { 0.2500f, 24 },   // D#
      { 0.3333f, 128 },  // E
      { 0.4167f, 64 },   // F
      { 0.5000f, 8 },    // F#
      { 0.5833f, 192 },  // G
      { 0.6667f, 16 },   // G#
      { 0.7500f, 96 },   // A
      { 0.8333f, 24 },   // A#
      { 0.9167f, 128 },  // B
    }
  },
  
  // C minor
  {
    1.0f,
    12,
    {
      { 0.0000f, 255 },  // C
      { 0.0833f, 16 },   // C#
      { 0.1667f, 96 },   // D
      { 0.2500f, 128 },  // Eb
      { 0.3333f, 8 },    // E
      { 0.4167f, 64 },   // F
      { 0.5000f, 4 },    // F#
      { 0.5833f, 192 },  // G
      { 0.6667f, 96 },   // G#
      { 0.7500f, 16 },   // A
      { 0.8333f, 128 },  // Bb
      { 0.9167f, 16 },   // B
    }
  },
  
  // Pentatonic
  {
    1.0f,
    12,
    {
      { 0.0000f, 255 },  // C
      { 0.0833f, 4 },    // C#
      { 0.1667f, 96 },   // D
      { 0.2500f, 4 },    // Eb
      { 0.3333f, 4 },    // E
      { 0.4167f, 140 },  // F
      { 0.5000f, 4 },    // F#
      { 0.5833f, 192 },  // G
      { 0.6667f, 4 },    // G#
      { 0.7500f, 96 },   // A
      { 0.8333f, 4 },    // Bb
      { 0.9167f, 4 },    // B
    }
  },
  
  // Pelog
  {
    1.0f,
    7,
    {
      { 0.0000f, 255 },  // C
      { 0.1275f, 128 },  // Db+
      { 0.2625f, 32 },  // Eb-
      { 0.4600f, 8 },    // F#-
      { 0.5883f, 192 },  // G
      { 0.7067f, 64 },  // Ab
      { 0.8817f, 16 },    // Bb+
    }
  },
  
  // Raag Bhairav That
  {
    1.0f,
    12,
    {
      { 0.0000f, 255 }, // ** Sa
      { 0.0752f, 128 }, // ** Komal Re
      { 0.1699f, 4 },   //    Re
      { 0.2630f, 4 },   //    Komal Ga
      { 0.3219f, 128 }, // ** Ga
      { 0.4150f, 64 },  // ** Ma
      { 0.4918f, 4 },   //    Tivre Ma
      { 0.5850f, 192 }, // ** Pa
      { 0.6601f, 64 },  // ** Komal Dha
      { 0.7549f, 4 },   //    Dha
      { 0.8479f, 4 },   //    Komal Ni
      { 0.9069f, 64 },  // ** Ni
    }
  },
  
  // Raag Shri
  {
    1.0f,
    12,
    {
      { 0.0000f, 255 }, // ** Sa
      { 0.0752f, 4 },   //    Komal Re
      { 0.1699f, 128 }, // ** Re
      { 0.2630f, 64 },  // ** Komal Ga
      { 0.3219f, 4 },   //    Ga
      { 0.4150f, 128 }, // ** Ma
      { 0.4918f, 4 },   //    Tivre Ma
      { 0.5850f, 192 }, // ** Pa
      { 0.6601f, 4 },   //    Komal Dha
      { 0.7549f, 64 },  // ** Dha
      { 0.8479f, 128 }, // ** Komal Ni
      { 0.9069f, 4 },   //    Ni
    }
  },
};

const int kLoopLength[] = {
  1,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  5, 5, 5, 5,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  7, 7,
  8, 8, 8, 8, 8, 8, 8, 8, 8,
  10, 10, 10,
  12, 12, 12, 12, 12, 12, 12,
  14, 14,
  16
};

const Ratio kYDividerRatios[] = {
  { 1, 64 },
  { 1, 48 },
  { 1, 32 },
  { 1, 24 },
  { 1, 16 },
  { 1, 12 },
  { 1, 8 },
  { 1, 6 },
  { 1, 4 },
  { 1, 3 },
  { 1, 2 },
  { 1, 1 },
};

/* ~8 KB: in .pool on the device (zeroed at boot; .bss is tight), Init sets them */
#ifdef FM1_TARGET
#define FOM_POOL __attribute__((section(".pool")))
#else
#define FOM_POOL
#endif
RandomGenerator g_generator;
FOM_POOL RandomStream g_stream;
FOM_POOL TGenerator g_t;
FOM_POOL XYGenerator g_xy;
HysteresisQuantizer2 g_length_quantizer;
GateFlags g_clock_flags;
float g_ramp[4];
bool g_gates[2];
float g_voltages[4];

}  // namespace

void marbles_init(uint32_t seed)
{
    g_generator.Init(seed ? seed : 1);
    g_stream.Init(&g_generator);
    g_t.Init(&g_stream, kRate);
    g_xy.Init(&g_stream, kRate);
    for (int i = 0; i < 6; ++i) g_xy.LoadScale(i, kPresetScales[i]);
    g_length_quantizer.Init(sizeof(kLoopLength) / sizeof(int), 0.25f, false);
    g_clock_flags = GATE_FLAG_LOW;
}

void marbles_entropy(uint32_t word)
{
    g_stream.Write(word);
}

static inline float knob(uint32_t q16) { return static_cast<float>(q16) / 65536.0f; }

/* marbles.cc's Process, one sample: the knobs as its CV reader leaves them, DEJA VU's deadband at noon */
void marbles_step(const MarblesPatch& p, int clock, MarblesOut* out)
{
    float deja_vu = knob(p.deja_vu);
    if (deja_vu < 0.47f) deja_vu *= 1.06382978723f;
    else if (deja_vu > 0.53f) deja_vu = 0.5f + (deja_vu - 0.53f) * 1.06382978723f;
    else deja_vu = 0.5f;
    int length = g_length_quantizer.Lookup(kLoopLength, knob(p.length));

    Ramps ramps;
    ramps.master = &g_ramp[0];
    ramps.external = &g_ramp[1];
    ramps.slave[0] = &g_ramp[2];
    ramps.slave[1] = &g_ramp[3];

    bool external = clock >= 0;
    g_clock_flags = ExtractGateFlags(g_clock_flags, clock > 0);
    GateFlags flags = g_clock_flags;

    g_t.set_model(TGeneratorModel(p.t_model > 6 ? 6 : p.t_model));
    g_t.set_range(TGeneratorRange(p.t_range > 2 ? 2 : p.t_range));
    g_t.set_rate(120.0f * knob(p.t_rate) - 60.0f);       /* (cv_reader: semitones around 2 Hz, +-5 octaves) */
    g_t.set_bias(knob(p.t_bias));
    g_t.set_jitter(knob(p.t_jitter));
    g_t.set_deja_vu(p.t_deja_vu ? deja_vu : 0.0f);
    g_t.set_length(length);
    g_t.set_pulse_width_mean(0.5f);
    g_t.set_pulse_width_std(0.0f);
    g_t.Process(external, &flags, ramps, g_gates, 1);

    GroupSettings x, y;
    x.control_mode = ControlMode(p.x_mode > 2 ? 2 : p.x_mode);
    x.voltage_range = VoltageRange(p.x_range > 2 ? 2 : p.x_range);
    x.register_mode = false;
    x.register_value = 0.0f;
    x.spread = knob(p.x_spread);
    x.bias = knob(p.x_bias);
    x.steps = knob(p.x_steps);
    x.deja_vu = p.x_deja_vu ? deja_vu : 0.0f;
    x.length = length;
    x.ratio.p = 1;
    x.ratio.q = 1;
    x.scale_index = p.x_scale > 5 ? 5 : p.x_scale;
    y.control_mode = CONTROL_MODE_IDENTICAL;
    y.voltage_range = VOLTAGE_RANGE_FULL;
    y.register_mode = false;
    y.register_value = 0.0f;
    y.spread = 0.5f;
    y.bias = 0.5f;
    y.steps = 0.0f;
    y.deja_vu = 0.0f;
    y.length = 1;
    y.ratio = kYDividerRatios[(p.y_rate >> 8) * 12 >> 8];
    y.scale_index = x.scale_index;
    g_xy.Process(CLOCK_SOURCE_INTERNAL_T1_T2_T3, x, y, &flags, ramps, g_voltages, 1);

    out->gates = (g_gates[0] ? 1u : 0u) | (g_ramp[0] < 0.5f ? 2u : 0u) | (g_gates[1] ? 4u : 0u);
    for (int i = 0; i < 3; ++i) out->x[i] = static_cast<int32_t>(g_voltages[i] * 3072.0f);
    float yv = g_voltages[3] * 6553.6f;
    out->y = yv > 32767.0f ? 32767 : yv < -32768.0f ? -32768 : static_cast<int32_t>(yv);
}

}  // namespace fom

/* Marbles' classes have (empty) destructors, which C++ registers to run at exit; the firmware never exits, and its
 * startup runs no static constructors (Marbles' Init sets everything): these stubs only satisfy the linker. */
#ifdef FM1_TARGET
extern "C" {
void* __dso_handle = 0;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
}
#endif
