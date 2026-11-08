// SPDX-License-Identifier: MIT
// The fixed-point Clouds (dsp/engine/fom_clouds.cc, the code that runs on the FM-1) driven exactly like ref_clouds
// drives the float original, at 32 kHz: compare.py measures the two against each other.
//
//   port_clouds IN.wav OUT.wav [key=value ...]   (ref_clouds' keys and defaults; mode=0 and quality=0 only, ported)
//   port_clouds IN.wav OUT.wav fx=1 ...          the firmware's path instead: CloudsFx at 44.1 kHz, out at 44.1 kHz
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fom_clouds.h"
#include "wav.h"

namespace {
int16_t buffer[2 * fom::kCloudsBufferSamples];
int32_t diffuser[fom::kCloudsDiffuserSize];
uint16_t reverb[fom::kCloudsReverbSize];
fom::Clouds clouds;
fom::CloudsFx fx;
}

static uint32_t q16(float v) { return v <= 0.0f ? 0u : v >= 1.0f ? 65535u : (uint32_t)(v * 65536.0f + 0.5f); }

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s IN.wav OUT.wav [position= size= density= texture= pitch= blend= spread= "
                             "feedback= reverb= freeze= seconds= fx=1]\n", argv[0]);
        return 2;
    }
    float position = 0.5f, size = 0.5f, density = 0.6f, texture = 0.5f, pitch = 0.0f;
    float blend = 1.0f, spread = 0.5f, feedback = 0.0f, rev = 0.3f, freeze_at = -1.0f, seconds = -1.0f;
    int use_fx = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "not key=value: %s\n", argv[i]); return 2; }
        std::string k = a.substr(0, eq);
        float v = std::strtof(a.c_str() + eq + 1, nullptr);
        if (k == "mode" || k == "quality") {
            if (v != 0.0f) { std::fprintf(stderr, "ported: mode=0 (granular), quality=0 (stereo, 16 bits)\n"); return 2; }
        }
        else if (k == "position") position = v;
        else if (k == "size") size = v;
        else if (k == "density") density = v;
        else if (k == "texture") texture = v;
        else if (k == "pitch") pitch = v;
        else if (k == "blend") blend = v;
        else if (k == "spread") spread = v;
        else if (k == "feedback") feedback = v;
        else if (k == "reverb") rev = v;
        else if (k == "freeze") freeze_at = v;
        else if (k == "seconds") seconds = v;
        else if (k == "fx") use_fx = (int)v;
        else { std::fprintf(stderr, "unknown key: %s\n", k.c_str()); return 2; }
    }

    std::vector<int16_t> src;
    int ch = 1, src_rate = 32000;
    if (!fomref::read_wav(argv[1], &src, &ch, &src_rate)) { std::fprintf(stderr, "%s: not a 16-bit WAV\n", argv[1]); return 1; }
    const int rate = use_fx ? 44100 : 32000;
    const long src_frames = (long)(src.size() / ch);
    long total = seconds > 0 ? (long)(seconds * rate) : (long)((double)src_frames * rate / src_rate);

    fom::CloudsPatch p;
    p.position = q16(position); p.size = q16(size); p.density = q16(density); p.texture = q16(texture);
    p.pitch = (int32_t)(pitch * 65536.0f + (pitch < 0 ? -0.5f : 0.5f));
    p.blend = q16(blend); p.spread = q16(spread); p.feedback = q16(feedback); p.reverb = q16(rev);
    p.freeze = false;
    clouds.Init(buffer, diffuser, reverb);
    fx.Init(buffer, diffuser, reverb);

    const int block = 32;
    std::vector<int16_t> out;
    out.reserve(total * 2);
    int16_t in[block * 2], o[block * 2];
    int32_t io[block * 2];
    for (long t = 0; t < total; t += block) {
        p.freeze = freeze_at >= 0 && t >= (long)(freeze_at * rate);
        for (int i = 0; i < block; ++i) {           // the input, linearly resampled (as ref_clouds does)
            double pos = (double)(t + i) * src_rate / rate;
            long j = (long)pos;
            double f = pos - j;
            for (int c = 0; c < 2; ++c) {
                int cc = c < ch ? c : 0;
                double a = j < src_frames ? src[j * ch + cc] : 0, b = j + 1 < src_frames ? src[(j + 1) * ch + cc] : a;
                in[2 * i + c] = (int16_t)(a + (b - a) * f);
            }
        }
        if (use_fx) {
            for (int i = 0; i < 2 * block; ++i) io[i] = in[i];
            fx.Process(p, io, block);
            for (int i = 0; i < 2 * block; ++i) o[i] = (int16_t)(io[i] < -32768 ? -32768 : io[i] > 32767 ? 32767 : io[i]);
        } else if (t == 0) {
            for (int i = 0; i < 2 * block; ++i) o[i] = 0;   // Clouds' first block: silent, unrecorded (its buffers are
        } else {                                            // set up by the Prepare() after it)
            clouds.ProcessShort(p, in, o, block);
        }
        for (int i = 0; i < 2 * block; ++i) out.push_back(o[i]);
    }
    if (!fomref::write_wav(argv[2], out, 2, rate)) { std::perror(argv[2]); return 1; }
    std::printf("%s: port of Clouds (granular), %.2f s at %d Hz\n", argv[2], (double)total / rate, rate);
    return 0;
}
