// SPDX-License-Identifier: MIT
// Reference render of Mutable Instruments Rings, the original float code, unmodified (dsp/ref/eurorack), on the
// host: what the fixed-point port in dsp/ must sound like. Rings runs at its own 48 kHz here; compare.py resamples.
//
//   ref_rings OUT.wav [key=value ...]
//     model=0..5      0 modal, 1 sympathetic strings, 2 string, 3 FM voice, 4 sympathetic quantized,
//                     5 string and reverb
//     poly=1|2|4      polyphony
//     structure= brightness= damping= position=   0..1, the four knobs (default 0.25 0.5 0.5 0.3)
//     notes=60,64,67  MIDI notes, one struck every `every` seconds (default 60)
//     every=1.0       seconds between strikes        seconds=4.0  the length
//     in=FILE.wav     an external exciter (mono or the left channel) instead of the internal one
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rings/dsp/part.h"
#include "rings/dsp/patch.h"
#include "rings/dsp/performance_state.h"
#include "wav.h"

namespace {
uint16_t reverb_buffer[32768];
rings::Part part;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s OUT.wav [model= poly= structure= brightness= damping= position= notes= every= seconds= in=]\n", argv[0]);
        return 2;
    }
    int model = 0, poly = 1;
    float structure = 0.25f, brightness = 0.5f, damping = 0.5f, position = 0.3f, every = 1.0f, seconds = 4.0f;
    std::vector<int> notes = {60};
    const char* in_path = nullptr;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "not key=value: %s\n", argv[i]); return 2; }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "model") model = std::atoi(v.c_str());
        else if (k == "poly") poly = std::atoi(v.c_str());
        else if (k == "structure") structure = std::strtof(v.c_str(), nullptr);
        else if (k == "brightness") brightness = std::strtof(v.c_str(), nullptr);
        else if (k == "damping") damping = std::strtof(v.c_str(), nullptr);
        else if (k == "position") position = std::strtof(v.c_str(), nullptr);
        else if (k == "every") every = std::strtof(v.c_str(), nullptr);
        else if (k == "seconds") seconds = std::strtof(v.c_str(), nullptr);
        else if (k == "in") in_path = argv[i] + eq + 1;
        else if (k == "notes") {
            notes.clear();
            for (const char* p = v.c_str(); *p; ) {
                notes.push_back(std::atoi(p));
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
        } else { std::fprintf(stderr, "unknown key: %s\n", k.c_str()); return 2; }
    }

    std::vector<int16_t> ext;
    int ext_ch = 1, ext_rate = 48000;
    if (in_path && !fomref::read_wav(in_path, &ext, &ext_ch, &ext_rate)) {
        std::fprintf(stderr, "%s: not a 16-bit WAV\n", in_path);
        return 1;
    }

    part.Init(reverb_buffer);
    part.set_polyphony(poly);
    part.set_model(static_cast<rings::ResonatorModel>(model));

    const int rate = 48000, block = (int)rings::kMaxBlockSize;
    const long total = (long)(seconds * rate), period = (long)(every * rate);
    std::vector<int16_t> out;
    out.reserve(total * 2);
    float in[rings::kMaxBlockSize], o[rings::kMaxBlockSize], aux[rings::kMaxBlockSize];
    size_t next_note = 0;
    rings::Patch patch = {structure, brightness, damping, position};
    rings::PerformanceState ps;
    ps.internal_exciter = in_path == nullptr;
    ps.internal_strum = false;
    ps.internal_note = false;
    ps.tonic = 0.0f;
    ps.fm = 0.0f;
    ps.chord = 0;
    ps.note = (float)notes[0];
    for (long t = 0; t < total; t += block) {
        ps.strum = false;
        if (period > 0 && t % period < block) {      // the first block of each period: strike the next note
            ps.note = (float)notes[next_note++ % notes.size()];
            ps.strum = true;
        }
        for (int i = 0; i < block; ++i) {
            long n = (t + i) * ext_ch;
            in[i] = in_path && n < (long)ext.size() ? ext[n] / 32768.0f : 0.0f;
        }
        part.Process(ps, patch, in, o, aux, block);
        for (int i = 0; i < block; ++i) {
            float l = o[i], r = aux[i];
            l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
            r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
            out.push_back((int16_t)(l * 32767.0f));
            out.push_back((int16_t)(r * 32767.0f));
        }
    }
    if (!fomref::write_wav(argv[1], out, 2, rate)) { std::perror(argv[1]); return 1; }
    std::printf("%s: Rings model %d, poly %d, %zu notes, %.2f s at 48 kHz\n", argv[1], model, poly, notes.size(), seconds);
    return 0;
}
