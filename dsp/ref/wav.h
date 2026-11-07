// SPDX-License-Identifier: MIT
// Minimal 16-bit PCM WAV reading and writing for the reference renders (host only).
#ifndef FOM_REF_WAV_H
#define FOM_REF_WAV_H

#include <cstdint>
#include <cstdio>
#include <vector>

namespace fomref {

inline bool write_wav(const char* path, const std::vector<int16_t>& interleaved, int channels, int rate)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t data = (uint32_t)(interleaved.size() * 2), u32;
    uint16_t u16;
    std::fwrite("RIFF", 1, 4, f); u32 = 36 + data; std::fwrite(&u32, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f); u32 = 16; std::fwrite(&u32, 4, 1, f);
    u16 = 1; std::fwrite(&u16, 2, 1, f); u16 = (uint16_t)channels; std::fwrite(&u16, 2, 1, f);
    u32 = (uint32_t)rate; std::fwrite(&u32, 4, 1, f); u32 = (uint32_t)(rate * channels * 2); std::fwrite(&u32, 4, 1, f);
    u16 = (uint16_t)(channels * 2); std::fwrite(&u16, 2, 1, f); u16 = 16; std::fwrite(&u16, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    std::fwrite(interleaved.data(), 2, interleaved.size(), f);
    return std::fclose(f) == 0;
}

// Reads a 16-bit PCM WAV; returns false if it is not one. Chunks other than fmt and data are skipped.
inline bool read_wav(const char* path, std::vector<int16_t>* interleaved, int* channels, int* rate)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char id[4];
    uint32_t size;
    bool ok = std::fread(id, 1, 4, f) == 4 && std::fread(&size, 4, 1, f) == 1 && std::fread(id, 1, 4, f) == 4;
    int bits = 0;
    while (ok && std::fread(id, 1, 4, f) == 4 && std::fread(&size, 4, 1, f) == 1) {
        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't') {
            uint8_t fmt[16];
            ok = size >= 16 && std::fread(fmt, 1, 16, f) == 16;
            *channels = fmt[2] | fmt[3] << 8;
            *rate = (int)(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24);
            bits = fmt[14] | fmt[15] << 8;
            if (size > 16) std::fseek(f, size - 16, SEEK_CUR);
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            interleaved->resize(size / 2);
            ok = std::fread(interleaved->data(), 2, interleaved->size(), f) == interleaved->size();
            break;
        } else {
            std::fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
    std::fclose(f);
    return ok && bits == 16 && *channels > 0;
}

}  // namespace fomref

#endif
