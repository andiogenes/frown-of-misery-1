// SPDX-License-Identifier: MIT
// The fixed-point Rings (dsp/engine/fom_rings.cc, the code that runs on the FM-1) driven exactly like ref_rings
// drives the float original, at 44.1 kHz: compare.py measures the two against each other.
//
//   port_rings OUT.wav [model=0] [poly=1|2|4] [structure= brightness= damping= position=] [notes=60,64] [every=1]
//              [seconds=4]                                  (the same keys and defaults as ref_rings)
//              [refrate=48000]   the reference's rate: a strike lands where ref_rings' does, at the first of Rings'
//                                24-sample blocks past each period at that rate, converted to seconds
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fom_rings.h"
#include "wav.h"

namespace {
fom::RingsModal modal;
fom::RingsStrings strings;
int32_t lines[fom::kNumStrings * fom::kStringDelay], stretch[fom::kMaxPolyphony * fom::kStringDelay / 2];
}

static uint32_t q16(float v) { return v <= 0.0f ? 0u : v >= 1.0f ? 65535u : (uint32_t)(v * 65536.0f + 0.5f); }

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s OUT.wav [poly= structure= brightness= damping= position= notes= every= seconds=]\n", argv[0]); return 2; }
    int poly = 1, refrate = 48000, model = 0;
    float structure = 0.25f, brightness = 0.5f, damping = 0.5f, position = 0.3f, every = 1.0f, seconds = 4.0f;
    std::vector<int> notes = {60};
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "not key=value: %s\n", argv[i]); return 2; }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "model") {
            model = std::atoi(v.c_str());
            if (model < 0 || model > 2) { std::fprintf(stderr, "ported: model=0 modal, 1 sympathetic strings, 2 string\n"); return 2; }
        }
        else if (k == "poly") poly = std::atoi(v.c_str());
        else if (k == "refrate") refrate = std::atoi(v.c_str());
        else if (k == "structure") structure = std::strtof(v.c_str(), nullptr);
        else if (k == "brightness") brightness = std::strtof(v.c_str(), nullptr);
        else if (k == "damping") damping = std::strtof(v.c_str(), nullptr);
        else if (k == "position") position = std::strtof(v.c_str(), nullptr);
        else if (k == "every") every = std::strtof(v.c_str(), nullptr);
        else if (k == "seconds") seconds = std::strtof(v.c_str(), nullptr);
        else if (k == "notes") {
            notes.clear();
            for (const char* p = v.c_str(); *p; ) {
                notes.push_back(std::atoi(p));
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
        } else { std::fprintf(stderr, "unknown key: %s\n", k.c_str()); return 2; }
    }
    modal.Init();
    modal.set_polyphony(poly);
    strings.Init(lines, stretch);
    strings.set_model(model);
    strings.set_polyphony(poly);
    fom::RingsPatch patch = {q16(structure), q16(brightness), q16(damping), q16(position)};
    const int rate = 44100, block = 256;
    const long total = (long)(seconds * rate);
    // ref_rings' strike times: block starts t (multiples of 24 at refrate) with t % period < 24
    std::vector<long> strikes;
    {
        const long rperiod = (long)(every * refrate), rtotal = (long)(seconds * refrate);
        for (long t = 0; rperiod > 0 && t < rtotal; t += 24)
            if (t % rperiod < 24) strikes.push_back((long)((double)t * rate / refrate + 0.5));
    }
    size_t next_strike = 0;
    std::vector<int16_t> out;
    std::vector<int32_t> buf(block * 2);
    size_t next_note = 0;
    for (long t = 0; t < total; ) {
        // render up to the next strike, so that each lands on its exact sample like ref_rings' (the firmware's
        // main loop is coarser, but that is not what this measures)
        long n = block;
        while (next_strike < strikes.size() && strikes[next_strike] <= t) {
            uint32_t note = (uint32_t)notes[next_note++ % notes.size()];
            if (model == 0) modal.Strike(note, 0, 127); else strings.Strike(note, 0, 127);
            ++next_strike;
        }
        if (next_strike < strikes.size() && strikes[next_strike] - t < n) n = strikes[next_strike] - t;
        if (n > total - t) n = total - t;
        std::fill(buf.begin(), buf.end(), 0);
        if (model == 0) modal.Render(patch, 32767, buf.data(), (size_t)n);
        else strings.Render(patch, 32767, buf.data(), (size_t)n);
        for (long i = 0; i < n * 2; ++i) {
            int32_t v = buf[i];
            out.push_back((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
        }
        t += n;
    }
    out.resize(total * 2);
    if (!fomref::write_wav(argv[1], out, 2, rate)) { std::perror(argv[1]); return 1; }
    std::printf("%s: fixed-point Rings model %d, poly %d, %zu notes, %.2f s at 44.1 kHz\n", argv[1], model, poly, notes.size(), seconds);
    return 0;
}
