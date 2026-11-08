// SPDX-License-Identifier: MIT
// Reference render of Mutable Instruments Clouds, the original float code, unmodified (dsp/ref/eurorack), on the
// host: what the fixed-point port in dsp/ must sound like. Clouds runs at its own 32 kHz; the input is resampled
// to it (linear: this is a test signal, not a product).
//
//   ref_clouds IN.wav OUT.wav [key=value ...]
//     mode=0..3       0 granular, 1 stretch, 2 looping delay, 3 spectral
//     quality=0..3    Clouds' 4 qualities: bit 0 mono, bit 1 low fidelity (8-bit mu-law, half rate)
//     position= size= density= texture=   0..1 (default 0.5 0.5 0.6 0.5)
//     pitch=0         semitones
//     blend= spread= feedback= reverb=    0..1 (default 1 0.5 0 0.3): dry/wet, stereo spread, feedback, reverb
//     freeze=SECONDS  freeze the buffer from then on (default: never)     seconds=  the length (default: the input's)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "clouds/dsp/granular_processor.h"
#include "wav.h"

namespace {
uint8_t block_mem[118784];          // the sizes Clouds itself gives the processor (clouds/clouds.cc)
uint8_t block_ccm[65536 - 128];
clouds::GranularProcessor processor;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s IN.wav OUT.wav [mode= quality= position= size= density= texture= pitch= "
                             "blend= spread= feedback= reverb= freeze= seconds=]\n", argv[0]);
        return 2;
    }
    int mode = 0, quality = 0;
    float position = 0.5f, size = 0.5f, density = 0.6f, texture = 0.5f, pitch = 0.0f;
    float blend = 1.0f, spread = 0.5f, feedback = 0.0f, reverb = 0.3f, freeze_at = -1.0f, seconds = -1.0f;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "not key=value: %s\n", argv[i]); return 2; }
        std::string k = a.substr(0, eq);
        float v = std::strtof(a.c_str() + eq + 1, nullptr);
        if (k == "mode") mode = (int)v;
        else if (k == "quality") quality = (int)v;
        else if (k == "position") position = v;
        else if (k == "size") size = v;
        else if (k == "density") density = v;
        else if (k == "texture") texture = v;
        else if (k == "pitch") pitch = v;
        else if (k == "blend") blend = v;
        else if (k == "spread") spread = v;
        else if (k == "feedback") feedback = v;
        else if (k == "reverb") reverb = v;
        else if (k == "freeze") freeze_at = v;
        else if (k == "seconds") seconds = v;
        else { std::fprintf(stderr, "unknown key: %s\n", k.c_str()); return 2; }
    }
    // the knobs in the FM-1's steps (1/65536, as dsp/engine's CloudsPatch has them), so a comparison with the port
    // measures the arithmetic, not the knobs' resolution: DENSITY 0.2 is not 0.2f there
    for (float* v : {&position, &size, &density, &texture, &blend, &spread, &feedback, &reverb}) {
        float q = *v <= 0.0f ? 0.0f : *v >= 1.0f ? 65535.0f : (float)(long)(*v * 65536.0f + 0.5f);
        if (q > 65535.0f) q = 65535.0f;
        *v = q / 65536.0f;
    }
    pitch = (float)(long)(pitch * 65536.0f + (pitch < 0 ? -0.5f : 0.5f)) / 65536.0f;

    std::vector<int16_t> src;
    int ch = 1, src_rate = 32000;
    if (!fomref::read_wav(argv[1], &src, &ch, &src_rate)) { std::fprintf(stderr, "%s: not a 16-bit WAV\n", argv[1]); return 1; }
    const int rate = 32000;
    const long src_frames = (long)(src.size() / ch);
    long total = seconds > 0 ? (long)(seconds * rate) : (long)((double)src_frames * rate / src_rate);

    processor.Init(block_mem, sizeof(block_mem), block_ccm, sizeof(block_ccm));
    processor.set_playback_mode(static_cast<clouds::PlaybackMode>(mode));
    processor.set_quality(quality);
    clouds::Parameters* p = processor.mutable_parameters();
    p->position = position; p->size = size; p->density = density; p->texture = texture; p->pitch = pitch;
    p->dry_wet = blend; p->stereo_spread = spread; p->feedback = feedback; p->reverb = reverb;
    p->freeze = false; p->trigger = false; p->gate = false;

    const int block = 32;               // Clouds' audio callback size (32 frames at 32 kHz)
    std::vector<int16_t> out;
    out.reserve(total * 2);
    clouds::ShortFrame in[block], o[block];
    for (long t = 0; t < total; t += block) {
        p->freeze = freeze_at >= 0 && t >= (long)(freeze_at * rate);
        for (int i = 0; i < block; ++i) {           // the input, linearly resampled to 32 kHz
            double pos = (double)(t + i) * src_rate / rate;
            long j = (long)pos;
            double f = pos - j;
            for (int c = 0; c < 2; ++c) {
                int cc = c < ch ? c : 0;
                double a = j < src_frames ? src[j * ch + cc] : 0, b = j + 1 < src_frames ? src[(j + 1) * ch + cc] : a;
                (c ? in[i].r : in[i].l) = (int16_t)(a + (b - a) * f);
            }
        }
        processor.Process(in, o, block);
        processor.Prepare();            // (the main loop's job on the module: once a block is plenty here)
        for (int i = 0; i < block; ++i) { out.push_back(o[i].l); out.push_back(o[i].r); }
    }
    if (!fomref::write_wav(argv[2], out, 2, rate)) { std::perror(argv[2]); return 1; }
    std::printf("%s: Clouds mode %d, quality %d, %.2f s at 32 kHz\n", argv[2], mode, quality, (double)total / rate);
    return 0;
}
