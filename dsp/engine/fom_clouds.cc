/* SPDX-License-Identifier: MIT */
#include "fom_clouds.h"

#include "fom_clouds_tables.h"
#include "fom_fixed.h"
#ifdef FOM_CLOUDS_DEBUG
#include <cmath>
#include <cstdio>
#endif

namespace fom {

namespace {

/* ---- integer helpers (no float, no 64-bit division) */

inline int32_t mulhi(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }
/* a times b Q30, rounded */
inline int32_t m30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b + (1 << 29)) >> 30); }
inline int32_t clip16(int32_t x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }
/* C's (int32_t) of a float that is x / 2^shift: towards zero */
inline int32_t trunc_shift(int64_t x, int shift) { return x < 0 ? -(int32_t)((-x) >> shift) : (int32_t)(x >> shift); }
inline int32_t knob_q30(uint32_t k) { return (int32_t)(k << 14); }

const int32_t kQ30 = 1 << 30;
const int32_t k0_0005 = 536871;           /* 0.0005, Q30 */
const int32_t k0_01 = 10737418;           /* 0.01 */
const int32_t k0_1 = 107374182;           /* 0.1 */
const int32_t k0_2 = 214748365;           /* 0.2 */
const int32_t k0_625 = 671088640;         /* 0.625, the diffuser's allpasses */
const int32_t k0_7 = 751619277;           /* 0.7, the reverb's diffusion */
const int32_t k1_4 = 1503238554;          /* 1.4 */
const int32_t k0_95 = 1020054733;         /* 0.95 */
const int32_t k0_54 = 579820585;          /* 0.54 */
const int32_t k0_35 = 375809638;          /* 0.35 */
const int32_t k0_63 = 676457349;          /* 0.63 */
const int32_t k0_6 = 644245094;           /* 0.6 */
const int32_t k0_37 = 397284475;          /* 0.37 */

/* AudioBuffer<16 bit>::ReadHermite (Laurent de Soras's), from sample i - 1 (the point between i + 1 and i + 2), Q24.
 * Doubled (no halves lost), in Q11 between the multiplies. */
inline int32_t read_hermite(const int16_t* s, int32_t i, uint32_t frac)
{
    int32_t xm1 = s[i], x0 = s[i + 1], x1 = s[i + 2], x2 = s[i + 3];
    int32_t c = x1 - xm1, v = 2 * (x0 - x1), w = c + v, a = w + v + (x2 - x0), b = w + a;
    int32_t t = (int32_t)(frac << 15);                                 /* Q31 */
    int32_t y = mulhi(a << 12, t) - (b << 11);
    y = (mulhi(y, t) << 1) + (c << 11);
    y = (mulhi(y, t) << 1) + (x0 << 12);
    return y >> 3;
}

inline int32_t read_linear(const int16_t* s, int32_t i, uint32_t frac)
{
    int32_t x0 = s[i], x1 = s[i + 1];
    return (x0 << 9) + (((x1 - x0) * (int32_t)(frac >> 1)) >> 6);
}

/* the FxEngines' delay lines: lengths as Clouds reserves them; each starts one past the previous one's end */
const int32_t kDiffuserLength[8] = { 126, 180, 269, 444, 151, 205, 245, 405 };
const int32_t kReverbLength[10] = { 113, 162, 241, 399, 1653, 2038, 3411, 1913, 1663, 4782 };

const int32_t kDiffuserBase[8] = { 0, 127, 308, 578, 1023, 1175, 1381, 1627 };          /* (ends at 2032) */
const int32_t kReverbBase[10] = { 0, 114, 277, 519, 919, 2573, 4612, 8024, 9938, 11602 };   /* (ends at 16384) */

/* the reverb's 12-bit format: value * 4096 in 16 bits */
inline int32_t dec12(uint16_t v) { return (int32_t)(int16_t)v << 12; }
inline uint16_t comp12(int32_t acc) { return (uint16_t)(int16_t)clip16(trunc_shift(acc, 12)); }

/* ---- float32, emulated: a grain's start, length and rate are computed in float on Clouds, and truncated to
 * integers (a sample, a 1/65536 step), so the port rounds as float32 does there. A few hundred times a second. */

typedef CloudsFloat F;

F fround(int64_t m, int32_t e)            /* to 24 significant bits, ties to even */
{
    if (!m) { F z = { 0, 0 }; return z; }
    bool neg = m < 0;
    uint64_t a = neg ? (uint64_t)-m : (uint64_t)m;
    int32_t top = 0;
    while (top < 63 && (a >> (top + 1))) ++top;
    if (top < 23) { a <<= 23 - top; e -= 23 - top; }  /* (normalized: the mantissa in [2^23, 2^24)) */
    if (top > 23) {
        int32_t sh = top - 23;
        uint64_t q = a >> sh, rem = a & ((((uint64_t)1) << sh) - 1), half = ((uint64_t)1) << (sh - 1);
        if (rem > half || (rem == half && (q & 1u))) ++q;
        if (q >> 24) { q >>= 1; ++sh; }
        a = q;
        e += sh;
    }
    F r = { neg ? -(int64_t)a : (int64_t)a, e };
    return r;
}
inline F fmul(F a, F b) { return fround(a.m * b.m, a.e + b.e); }
F fadd(F a, F b)
{
    if (!a.m) return b;
    if (!b.m) return a;
    if (a.e < b.e) { F t = a; a = b; b = t; }
    int32_t d = a.e - b.e;
    if (d > 38) return a;                 /* (b under half an ulp of a) */
    return fround((a.m << d) + b.m, b.e);
}
inline F fneg(F a) { a.m = -a.m; return a; }
inline F fint(int64_t x) { return fround(x, 0); }
inline bool fless(F a, F b) { return fadd(a, fneg(b)).m < 0; }
inline int32_t ftrunc(F a)                /* (int32_t), towards zero */
{
    if (a.e >= 0) return (int32_t)(a.m << a.e);
    if (a.e <= -40) return 0;
    return a.m < 0 ? -(int32_t)((-a.m) >> -a.e) : (int32_t)(a.m >> -a.e);
}

F fdiv(F a, F b)                          /* a / b, both positive */
{
    uint64_t num = (uint64_t)a.m << 31;   /* (a.m / 2 < 2^23 <= b.m: the quotient fits 32 bits, over 2^30) */
    uint32_t q = Udiv64(num, (uint32_t)b.m);
    uint64_t rem = num - (uint64_t)q * (uint32_t)b.m;
    return fround(((int64_t)q << 1) | (rem ? 1 : 0), a.e - b.e - 32);     /* (a sticky bit for the rounding) */
}
inline int64_t fceil(F a)                 /* the smallest integer >= a, a > 0 */
{
    if (a.e >= 0) return a.m << a.e;
    if (a.e <= -40) return 1;
    return (a.m + (((int64_t)1) << -a.e) - 1) >> -a.e;
}
inline int32_t fq30(F a)                  /* Q30, a in [0, 2) */
{
    int32_t s = a.e + 30;
    return s >= 0 ? (int32_t)(a.m << s) : s > -40 ? (int32_t)(a.m >> -s) : 0;
}
/* stmlib's fast_rsqrt_carmack, x > 1: the bit trick on x's float32 bits, then one Newton step in float32 */
F rsqrt_carmack(F x)
{
    uint32_t bits = ((uint32_t)(x.e + 23 + 127) << 23) | ((uint32_t)x.m & 0x7FFFFFu);
    uint32_t i = 0x5f3759dfu - (bits >> 1);
    F y = { (int64_t)((i & 0x7FFFFFu) | 0x800000u), (int32_t)(i >> 23) - 127 - 23 };
    F x2 = x;
    x2.e -= 1;
    const F threehalfs = { 12582912, -23 };
    return fmul(y, fadd(threehalfs, fneg(fmul(fmul(x2, y), y))));
}

/* v (positive) rounded to 24 significant bits, ties to even */
uint64_t round24(uint64_t v)
{
    int32_t t = 0;
    while (t < 63 && (v >> (t + 1))) ++t;               /* (a shift by 64 is undefined) */
    if (t <= 23) return v;
    uint64_t u = (uint64_t)1 << (t - 23), r = v & (u - 1), q = v - r;
    if (r > (u >> 1) || (r == (u >> 1) && (q & u))) q += u;
    return q;
}

/* A grain's length as Grain::RenderEnvelope has it: the phase climbs from 0 by inc in float32 and the grain ends at
 * the step that reaches 2; the samples before that. Float32's sums keep to each binade's grid, so all but the last
 * steps of a binade are one division (each step on its own only where inc falls on a tie of the grid: rare). */
uint32_t envelope_length(F inc)
{
    const uint64_t two = (uint64_t)1 << 63;                 /* Q62 */
    const uint64_t x = (uint64_t)inc.m << (inc.e + 62);     /* exact: inc >= 2 / 16384 */
    uint64_t phase = 0;
    uint32_t steps = 0;
    while (phase < two) {
        if (phase) {
            int32_t b = 0;
            while (phase >> (b + 1)) ++b;
            int32_t sh = b - 23;                            /* the grid: 2^sh */
            uint64_t ulp = (uint64_t)1 << sh, r = x & (ulp - 1);
            if (r != (ulp >> 1)) {
                uint32_t su = (uint32_t)(x >> sh) + (r > (ulp >> 1) ? 1u : 0u);      /* the rounded step, in grid units */
                uint64_t end = (uint64_t)1 << (b + 1);
                if (end > phase + x) {                      /* the steps whose exact sum stays in the binade */
                    uint32_t d = (uint32_t)((end - phase - x) >> sh) + (((end - phase - x) & (ulp - 1)) ? 1u : 0u);
                    uint32_t k = (d - 1) / su + 1;
                    phase += ((uint64_t)k * su) << sh;
                    steps += k;
                    continue;
                }
            }
        }
        phase = round24(phase + x);
        ++steps;
    }
    return steps - 1;
}

/* stmlib's SemitonesToRatio (semitones Q16): its two tables' float32 values, multiplied as float32 multiplies */
F semitones_to_ratio(int32_t semitones_q16)
{
    int32_t pitch = semitones_q16 + (128 << 16);   /* + 128: exact in float32 here (24 bits) */
    if (pitch < 0) pitch = 0;
    if (pitch > (255 << 16)) pitch = 255 << 16;
    int32_t i = pitch >> 16, j = (pitch & 0xFFFF) >> 8;
    F hi = { kPitchRatioHighM[i], kPitchRatioHighE[i] }, lo = { kPitchRatioLowM[j], kPitchRatioLowE[j] };
    return fmul(hi, lo);
}

}  // namespace

/* ==== a grain ===================================================================================================== */

void CloudsGrain::Start(int32_t pre_delay, int32_t buffer_size, int32_t start, int32_t width, int32_t phase_increment,
                        uint32_t window_shape, int32_t gain_l, int32_t gain_r, bool high)
{
    pre_delay_ = pre_delay;
    first_sample_ = (start + buffer_size) % buffer_size;
    if (first_sample_ < 0) first_sample_ += buffer_size;    /* (Clouds would read before its buffer here) */
    phase_increment_ = phase_increment;
    phase_ = 0;
    env_phase_ = 0;
    if (width < 2) width = 2;
    const F two = { 8388608, -22 };
    F inc = fdiv(two, fint(width));                         /* 2.0f / width */
    env_increment_ = (uint32_t)((inc.m + ((int64_t)1 << (-inc.e - 31))) >> (-inc.e - 30));   /* Q30, rounded */
    remaining_ = envelope_length(inc);
    if (window_shape >= (1u << 29)) {
        smoothness_ = (int32_t)((window_shape - (1u << 29)) << 1);
        slope_ = 0;
    } else {
        smoothness_ = 0;
        slope_ = Udiv64((uint64_t)1 << 45, window_shape + (uint32_t)k0_01);   /* 0.5 / (shape + 0.01), Q16 */
    }
    gain_l_ = gain_l;
    gain_r_ = gain_r;
    cross_l_ = kQ30 - gain_l;
    cross_r_ = kQ30 - gain_r;
    high_ = high;
    active_ = true;
}

void CloudsGrain::OverlapAdd(const int16_t* bl, const int16_t* br, int32_t buffer_size, int32_t* out, int32_t n)
{
    if (!active_) return;
    int32_t k = 0;
    while (pre_delay_ && k < n) { ++k; --pre_delay_; }
    uint32_t ph = env_phase_;
    const uint32_t inc = env_increment_;
    int32_t phase = phase_;
    out += 2 * k;
    for (; k < n; ++k) {
        /* the envelope: a triangle, through the smooth window or a slope */
        if (!remaining_) {
#ifdef FOM_CLOUDS_DEBUG
            std::fprintf(stderr, "E %d %d\n", (int)(n - k - 1), (int)first_sample_);
#endif
            active_ = false; break; }     /* (the step that reaches 2.0, in float32) */
        --remaining_;
        int32_t gain = (int32_t)(ph >= (1u << 31) ? 0 : ph >= (1u << 30) ? (1u << 31) - ph : ph);
        if (smoothness_) {
            if (high_) {
                uint32_t i = (uint32_t)gain >> 18, f = (uint32_t)gain & 0x3FFFFu;
                int32_t w = kCloudsWindowQ16[i];
                if (i < 4096) w += (int32_t)(((int64_t)((int32_t)kCloudsWindowQ16[i + 1] - w) * f) >> 18);
                gain += m30(smoothness_, (w << 14) - gain);
            }
        } else {
            int64_t g = ((int64_t)gain * slope_) >> 16;
            gain = g >= kQ30 ? kQ30 : (int32_t)g;
        }
        ph += inc;
        /* the sample */
        int32_t index = first_sample_ + (phase >> 16);
        if (index >= buffer_size) index -= buffer_size;
        uint32_t frac = (uint32_t)phase & 0xFFFFu;
        int32_t l, r;
        if (high_) { l = read_hermite(bl, index, frac); r = read_hermite(br, index, frac); }
        else { l = read_linear(bl, index, frac); r = read_linear(br, index, frac); }
        l = mulhi(l << 2, gain) << 2;                       /* Q24, times 4 for the gains' mulhi */
        r = mulhi(r << 2, gain) << 2;
        out[0] += mulhi(l, gain_l_) + mulhi(r, cross_r_);
        out[1] += mulhi(r, gain_r_) + mulhi(l, cross_l_);
        out += 2;
        phase += phase_increment_;
    }
    env_phase_ = ph;
    phase_ = phase;
}

/* ==== the processor =============================================================================================== */

void Clouds::Init(int16_t* buffer_l, int16_t* buffer_r, int32_t* diffuser, uint16_t* reverb)
{
    buf_[0] = buffer_l;
    buf_[1] = buffer_r;
    diffuser_ = diffuser;
    reverb_ = reverb;
    for (int32_t i = 0; i < kCloudsBufferSamples; ++i) buf_[0][i] = buf_[1][i] = 0;
    for (int32_t i = 0; i < kCloudsDiffuserSize; ++i) diffuser_[i] = 0;
    for (int32_t i = 0; i < kCloudsReverbSize; ++i) reverb_[i] = 0;
    for (int32_t i = 0; i < 256; ++i) tail_[0][i] = tail_[1][i] = 0;
    write_head_ = crossfade_counter_ = 0;
    for (int32_t i = 0; i < kCloudsMaxGrains; ++i) grains_[i].Init();
    num_grains_.m = 0; num_grains_.e = 0;
    gain_normalization_ = 1 << 29;
    hint_.m = 1 << 23; hint_.e = -13;                       /* 1024.0f */
    grain_rate_phasor_ = 0;
    rng_ = 0x21;                                            /* stmlib's Random */
    freeze_lp_ = dry_wet_ = 0;
    fb_feedback_ = 0xFFFFFFFFu;
    fb_g_ = fb_h_ = 0;
    for (int32_t c = 0; c < 2; ++c) fb_s1_[c] = fb_s2_[c] = lp_decay_[c] = 0;
    for (int32_t i = 0; i < kCloudsBlock * 2; ++i) fb_[i] = in_[i] = out_[i] = 0;
    diffuser_wp_ = reverb_wp_ = 0;
    for (int32_t i = 0; i < 2; ++i) {                       /* CosineOscillator::Start: y1 = c / 4, y0 = 1 / 2 */
        lfo_y1_[i] = (int64_t)kCloudsReverbLfoQ29[i] << 17;
        lfo_y0_[i] = (int64_t)1 << 47;
        lfo_value_[i] = 0;
    }
}

/* AudioBuffer::WriteFade, both channels in step (Clouds keeps two buffers whose heads move together) */
void Clouds::WriteBuffer(const int32_t* in, int32_t n, bool write)
{
    const int32_t size = kCloudsBufferSamples - 8;          /* kInterpolationTail */
    if (!write) {
        /* frozen: keep recording a little into the tail, to crossfade with when recording resumes */
        for (int32_t i = 0; i < n && crossfade_counter_ < 256; ++i) {
            tail_[0][crossfade_counter_] = (int16_t)clip16(trunc_shift((int64_t)in[2 * i] * 32767, 24));
            tail_[1][crossfade_counter_] = (int16_t)clip16(trunc_shift((int64_t)in[2 * i + 1] * 32767, 24));
            ++crossfade_counter_;
        }
    } else if (!crossfade_counter_ && write_head_ >= 8 && write_head_ < size - n) {
        for (int32_t i = 0; i < n; ++i) {                   /* the common case: * 32767 */
            buf_[0][write_head_] = (int16_t)clip16(trunc_shift((int64_t)in[2 * i] * 32767, 24));
            buf_[1][write_head_] = (int16_t)clip16(trunc_shift((int64_t)in[2 * i + 1] * 32767, 24));
            ++write_head_;
        }
    } else {
        for (int32_t i = 0; i < n; ++i) {                   /* Write(): * 32768, the tail copied, the crossfade */
            int32_t gain = 0, t = 0;
            if (crossfade_counter_) {
                --crossfade_counter_;
                t = 256 - crossfade_counter_;
                gain = crossfade_counter_ << 22;            /* counter / 256, Q30 */
            }
            for (int32_t c = 0; c < 2; ++c) {
                int32_t sample = in[2 * i + c];
                if (gain) sample += m30(((int32_t)tail_[c][t < 256 ? t : 255] << 9) - sample, gain);
                int16_t v = (int16_t)clip16(trunc_shift(sample, 9));
                buf_[c][write_head_] = v;
                if (write_head_ < 8) buf_[c][write_head_ + size] = v;
            }
            if (++write_head_ >= size) write_head_ = 0;
        }
    }
}

/* GranularSamplePlayer::Play, with the processor's reading of DENSITY and TEXTURE */
void Clouds::Play(const CloudsPatch& p, int32_t n)
{
    const int32_t size = kCloudsBufferSamples - 8;
    /* DENSITY: the overlap either side of noon, the seeding random (left) or regular (right); in float32 as Clouds
     * computes it, since the grains' timing hangs on its comparisons */
    const F c053 = { 8891924, -24 }, c047 = { 15770583, -25 }, c212 = { 8891924, -22 };
    F dens = fround(p.density, -16), ov = { 0, 0 };
    bool deterministic = p.density < 32768u;
    if (!fless(dens, c053)) ov = fmul(fadd(dens, fneg(c053)), c212);
    else if (!fless(c047, dens)) ov = fmul(fadd(c047, fneg(dens)), c212);
    int32_t overlap = fq30(ov);
    /* TEXTURE: the window */
    const F c1333 = { 11182014, -23 }, c075 = { 12582912, -24 };
    F tex = fround(p.texture, -16), ws = { 8388608, -23 };
    if (fless(tex, c075)) ws = fmul(tex, c1333);
    uint32_t window_shape = (uint32_t)fq30(ws);

    F target = fmul(fint(kCloudsMaxGrains), fmul(fmul(ov, ov), ov));
    /* GetFloat() < p, p = target / hint: the word (as float) under ceil(p 2^32); phasor >= space = hint / target:
     * the phasor (an integer) at ceil(space) */
    uint64_t prob_ceil = 0, space_ceil = (uint64_t)1 << 40;
    if (target.m) {
        F prob = fdiv(target, hint_);
        prob.e += 32;
        prob_ceil = (uint64_t)fceil(prob);
        space_ceil = (uint64_t)fceil(fdiv(hint_, target));
    }
    if (deterministic) prob_ceil = 0; else grain_rate_phasor_ = -1000;
    bool below_target = fless(num_grains_, target);

    int32_t available[kCloudsMaxGrains], num_available = 0;
    for (int32_t i = 0; i < kCloudsMaxGrains; ++i)
        if (!grains_[i].active()) available[num_available++] = i;

    for (int32_t t = 0; t < n; ++t) {
        ++grain_rate_phasor_;
        rng_ = rng_ * 1664525u + 1013904223u;
        /* Random::GetFloat() < p (the word rounded to float), while fewer grains than the target sound */
        bool seed = (uint64_t)FloatGridSigned(rng_) < prob_ceil && below_target;
        if (grain_rate_phasor_ >= 0 && (uint64_t)grain_rate_phasor_ >= space_ceil) seed = true;
        if (!num_available || !seed) continue;
        --num_available;
        CloudsGrain* g = &grains_[available[num_available]];
        bool high = num_available >= 3 * kCloudsMaxGrains / 4;
        /* ScheduleGrain, in float32's arithmetic */
        uint32_t si = p.size >> 8, sf = p.size & 0xFFu;
        F gs = fround(((int64_t)kCloudsGrainSize[si] << 8) +
                      (kCloudsGrainSize[si + 1] - kCloudsGrainSize[si]) * (int32_t)sf, -8);   /* (exact) */
        F ratio = semitones_to_ratio(p.pitch), inv = semitones_to_ratio(-p.pitch);
        rng_ = rng_ * 1664525u + 1013904223u;
        int64_t r = FloatGridSigned(rng_) - ((int64_t)1 << 31);                   /* GetFloat() - 0.5, Q32 */
        uint32_t pan = (uint32_t)(((int64_t)1 << 31) + ((r * (int64_t)p.spread) >> 16));
        int32_t gl, gr;
        if (pan < (1u << 31)) { gl = kQ30; gr = (int32_t)(pan >> 1); }
        else { gr = kQ30; gl = (int32_t)((0u - pan) >> 1); }
        F one = { 1, 0 };
        if (fless(one, ratio)) {                            /* the play head outruns the recording: shorter */
            F lim = fmul(fround(size, -2), inv);            /* size * 0.25 / ratio */
            if (fless(lim, gs)) gs = lim;
        }
        F eaten = fmul(gs, ratio);
        F avail = fadd(fadd(fint(size), fneg(eaten)), fneg(gs));
        F pos = fround(p.position, -16);
        int32_t start = write_head_ - n + t - ftrunc(fadd(fmul(pos, avail), eaten));
        F inc = ratio;
        inc.e += 16;
#ifdef FOM_CLOUDS_DEBUG
        std::fprintf(stderr, "G %d %d %d %d %d %.9g %.9g %.9g\n", (int)t, (int)(write_head_ - n + t), (int)start,
                     ftrunc(gs) & ~1, ftrunc(inc), (double)gs.m * std::ldexp(1.0, gs.e), gl / 1073741824.0, gr / 1073741824.0);
#endif
        g->Start(t, size, start, ftrunc(gs) & ~1, ftrunc(inc), window_shape, gl, gr, high);
        F tenth = { 13421773, -27 };                        /* 0.1f */
        hint_ = fadd(hint_, fmul(tenth, fadd(gs, fneg(hint_))));
        grain_rate_phasor_ = 0;
    }

    for (int32_t i = 0; i < n * 2; ++i) out_[i] = 0;
    for (int32_t i = 0; i < kCloudsMaxGrains; ++i) grains_[i].OverlapAdd(buf_[0], buf_[1], size, out_, n);

    /* the gain: 1 / sqrt(grains - 1), more with the overlap for the narrower windows */
    F active = fint(kCloudsMaxGrains - num_available);
    F err = fadd(active, fneg(num_grains_));
    const F c09 = { 15099494, -24 }, c02 = { 13421773, -26 }, one = { 8388608, -23 }, two = { 8388608, -22 };
    num_grains_ = fadd(num_grains_, fmul(err.m > 0 ? c09 : c02, err));
    F gain_f = one;
    if (fless(two, num_grains_)) gain_f = rsqrt_carmack(fadd(num_grains_, fneg(one)));
    F window_gain = fadd(one, fmul(two, ws));
    if (fless(two, window_gain)) window_gain = two;
    gain_f = fmul(gain_f, fadd(one, fmul(fadd(window_gain, fneg(one)), ov)));   /* Crossfade(1, wg, overlap) */
    int32_t gain = fq30(gain_f) >> 1;                                             /* Q29 */
    for (int32_t t = 0; t < n; ++t) {
        gain_normalization_ += (int32_t)(((int64_t)(gain - gain_normalization_) * k0_01) >> 30);
        out_[2 * t] = (int32_t)(((int64_t)out_[2 * t] * gain_normalization_) >> 29);
        out_[2 * t + 1] = (int32_t)(((int64_t)out_[2 * t + 1] * gain_normalization_) >> 29);
    }
}

/* fx/diffuser.h: four allpasses a side */
void Clouds::Diffuse(int32_t amount, int32_t n)
{
    const int32_t mask = kCloudsDiffuserSize - 1;
    int32_t* buf = diffuser_;
    int32_t wp = diffuser_wp_;
    for (int32_t t = 0; t < n; ++t) {
        wp = (wp - 1) & mask;
        for (int32_t c = 0; c < 2; ++c) {
            int32_t acc = out_[2 * t + c];
            for (int32_t k = 4 * c; k < 4 * c + 4; ++k) {
                int32_t base = kDiffuserBase[k];
                int32_t r = buf[(wp + base + kDiffuserLength[k] - 1) & mask];
                acc += m30(r, k0_625);
                buf[(wp + base) & mask] = acc;
                acc = m30(acc, -k0_625) + r;
            }
            out_[2 * t + c] += m30(acc - out_[2 * t + c], amount);
        }
    }
    diffuser_wp_ = wp;
}

/* the reverb's LFOs: CosineOscillator (approximate) in float32's steps, Q30 out */
int32_t Clouds::ReverbLfo(int32_t i, bool next)
{
    if (next) {
        int64_t t = lfo_y0_[i];
        int64_t delta = ((int64_t)1 << 30) - kCloudsReverbLfoQ29[i];
        int64_t prod = FloatGridSigned(2 * t - ((delta * t) >> 29));
        lfo_y0_[i] = FloatGridSigned(prod - lfo_y1_[i]);
        lfo_y1_[i] = t;
    }
    return (int32_t)(FloatGridSigned(lfo_y1_[i] + ((int64_t)1 << 47)) >> 18);
}

/* fx/reverb.h: Griesinger's topology (Dattorro), 4 allpasses in, a loop of 2 x (2 allpasses + a delay) */
void Clouds::Reverb(int32_t amount, int32_t krt, int32_t klp, int32_t n)
{
    const int32_t mask = kCloudsReverbSize - 1;
    const int32_t kap = k0_7, gain = k0_2;
    uint16_t* buf = reverb_;
    int32_t wp = reverb_wp_;
    int32_t lp1 = lp_decay_[0], lp2 = lp_decay_[1];
    const int32_t* base = kReverbBase;
#define TAIL(k) buf[(wp + base[k] + kReverbLength[k] - 1) & mask]
#define HEAD(k) buf[(wp + base[k]) & mask]
    for (int32_t t = 0; t < n; ++t) {
        wp = (wp - 1) & mask;
        bool next = (wp & 31) == 0;
        lfo_value_[0] = ReverbLfo(0, next);
        lfo_value_[1] = ReverbLfo(1, next);
        int32_t* io = &out_[2 * t];
        /* smear AP1 inside the loop: Interpolate(ap1, 10 + 60 lfo1), written back 100 samples in */
        int64_t off = ((int64_t)10 << 30) + (int64_t)60 * lfo_value_[0];
        int32_t oi = (int32_t)(off >> 30), of = (int32_t)(off & (kQ30 - 1));
        int32_t a = dec12(buf[(wp + oi + base[0]) & mask]), b = dec12(buf[(wp + oi + base[0] + 1) & mask]);
        int32_t acc = a + m30(b - a, of);
        buf[(wp + base[0] + 100) & mask] = comp12(acc);
        acc = m30(io[0] + io[1], gain);
        for (int32_t k = 0; k < 4; ++k) {                   /* diffuse */
            int32_t r = dec12(TAIL(k));
            acc += m30(r, kap);
            HEAD(k) = comp12(acc);
            acc = m30(acc, -kap) + r;
        }
        int32_t apout = acc;
        /* the loop, left */
        acc = apout;
        off = ((int64_t)4680 << 30) + (int64_t)100 * lfo_value_[1];
        oi = (int32_t)(off >> 30); of = (int32_t)(off & (kQ30 - 1));
        a = dec12(buf[(wp + oi + base[9]) & mask]); b = dec12(buf[(wp + oi + base[9] + 1) & mask]);
        acc += m30(a + m30(b - a, of), krt);
        lp1 += m30(acc - lp1, klp); acc = lp1;
        int32_t r = dec12(TAIL(4)); acc += m30(r, -kap); HEAD(4) = comp12(acc); acc = m30(acc, kap) + r;
        r = dec12(TAIL(5)); acc += m30(r, kap); HEAD(5) = comp12(acc); acc = m30(acc, -kap) + r;
        HEAD(6) = comp12(acc); acc *= 2;
        io[0] += m30(acc - io[0], amount);
        /* the loop, right */
        acc = apout;
        acc += m30(dec12(TAIL(6)), krt);
        lp2 += m30(acc - lp2, klp); acc = lp2;
        r = dec12(TAIL(7)); acc += m30(r, kap); HEAD(7) = comp12(acc); acc = m30(acc, -kap) + r;
        r = dec12(TAIL(8)); acc += m30(r, -kap); HEAD(8) = comp12(acc); acc = m30(acc, kap) + r;
        HEAD(9) = comp12(acc); acc *= 2;
        io[1] += m30(acc - io[1], amount);
    }
#undef TAIL
#undef HEAD
    lp_decay_[0] = lp1;
    lp_decay_[1] = lp2;
    reverb_wp_ = wp;
}

void Clouds::Run(const CloudsPatch& p, const int32_t* in, int32_t n)
{
    for (int32_t i = 0; i < 2 * n; ++i) in_[i] = in[i];
    /* feedback, high-passed against build-ups at very low frequencies; not while frozen */
    freeze_lp_ += m30((p.freeze ? kQ30 : 0) - freeze_lp_, k0_0005);
    int32_t feedback = knob_q30(p.feedback);
    if (p.feedback != fb_feedback_) {
        /* Svf::set_f_q<FREQUENCY_FAST>((20 + 100 fb^2) / 32000, 1): g = tan(pi f) (approximate), h = 1 / (1 + g + g^2) */
        fb_feedback_ = p.feedback;
        uint64_t f2 = ((uint64_t)p.feedback * p.feedback);                        /* Q32 */
        uint64_t num = ((uint64_t)20 << 32) + 100 * f2;                            /* Q32 Hz */
        uint32_t f = Udiv64(num << 6, 32000u);                                     /* f / fs, Q38 */
        uint32_t fq30 = f >> 8;
        int64_t ff = ((int64_t)fq30 * fq30) >> 30;                                 /* Q30 */
        /* pi + f^2 (3.260e-01 pi^3 + 1.823e-01 pi^5 f^2), Q28 */
        int64_t poly = 843314880 + ((ff * (2713358080LL + ((ff * 14975312896LL) >> 30))) >> 30);
        fb_g_ = (int32_t)(((int64_t)f * poly) >> 35);                              /* Q31 */
        int32_t g30 = fb_g_ >> 1;
        fb_h_ = (int32_t)Udiv64((uint64_t)1 << 60, (uint32_t)(kQ30 + g30 + (int32_t)(((int64_t)g30 * g30) >> 30)));
    }
    const int32_t g = fb_g_, h = fb_h_;
    for (int32_t c = 0; c < 2; ++c) {
        int32_t s1 = fb_s1_[c], s2 = fb_s2_[c];
        for (int32_t t = 0; t < n; ++t) {
            int32_t x = fb_[2 * t + c];
            int32_t gs1 = (int32_t)(((int64_t)s1 * g + (1 << 30)) >> 31);
            int32_t hp = m30(x - s1 - gs1 - s2, h);
            int32_t ghp = (int32_t)(((int64_t)hp * g + (1 << 30)) >> 31);
            int32_t bp = ghp + s1;
            s1 = ghp + bp;
            int32_t gbp = (int32_t)(((int64_t)bp * g + (1 << 30)) >> 31);
            int32_t lp = gbp + s2;
            s2 = gbp + lp;
            fb_[2 * t + c] = hp;
        }
        fb_s1_[c] = s1;
        fb_s2_[c] = s2;
    }
    int32_t fb_gain = m30(feedback, kQ30 - freeze_lp_);
    if (fb_gain) {
        int32_t k = m30(fb_gain, k1_4);
        for (int32_t i = 0; i < 2 * n; ++i)
            in_[i] += m30(fb_gain, SoftLimitQ24(m30(fb_[i], k) + in_[i]) - in_[i]);
    }

    WriteBuffer(in_, n, !p.freeze);
    Play(p, n);

    int32_t texture = knob_q30(p.texture);
    Diffuse(texture > 3 * (kQ30 / 4) ? (texture - 3 * (kQ30 / 4)) * 4 : 0, n);

    for (int32_t i = 0; i < 2 * n; ++i) fb_[i] = out_[i];    /* what is fed back: not the reverb */

    int32_t ra = m30(knob_q30(p.reverb), k0_95);
    ra += m30((int32_t)(((int64_t)feedback * ((int64_t)2 * kQ30 - feedback)) >> 30), freeze_lp_);   /* fb (2 - fb) */
    if (ra < 0) ra = 0;
    if (ra > kQ30) ra = kQ30;
    Reverb(m30(ra, k0_54), k0_35 + m30(k0_63, ra), k0_6 + m30(k0_37, feedback), n);
}

void Clouds::Process(const CloudsPatch& p, const int32_t* in, int32_t* wet, int32_t n)
{
    Run(p, in, n);
    int32_t target = knob_q30(p.blend);
    int32_t inc = (target - dry_wet_) / n;
    int32_t dw = dry_wet_;
    for (int32_t t = 0; t < n; ++t) {
        dw += inc;
        uint32_t i = (uint32_t)dw >> 26, f = (uint32_t)dw & 0x3FFFFFFu;
        int32_t fi = kCloudsXfadeInQ30[i];
        if (i < 16) fi += (int32_t)(((int64_t)(kCloudsXfadeInQ30[i + 1] - fi) * f) >> 26);
        int32_t k = m30(fi, k0_6);                                    /* 1.2 / 2: post gain, SoftConvert's half */
        wet[2 * t] = SoftLimitQ24(m30(out_[2 * t], k));
        wet[2 * t + 1] = SoftLimitQ24(m30(out_[2 * t + 1], k));
    }
    dry_wet_ = dw;
}

void Clouds::ProcessShort(const CloudsPatch& p, const int16_t* input, int16_t* output, int32_t n)
{
    int32_t in[kCloudsBlock * 2] = { 0 };
    for (int32_t i = 0; i < 2 * n; ++i) in[i] = (int32_t)input[i] << 9;
    Run(p, in, n);
    int32_t target = knob_q30(p.blend);
    int32_t inc = (target - dry_wet_) / n;
    int32_t dw = dry_wet_;
    for (int32_t t = 0; t < n; ++t) {
        dw += inc;
        uint32_t i = (uint32_t)dw >> 26, f = (uint32_t)dw & 0x3FFFFFFu;
        int32_t fi = kCloudsXfadeInQ30[i], fo = kCloudsXfadeOutQ30[i];
        if (i < 16) {
            fi += (int32_t)(((int64_t)(kCloudsXfadeInQ30[i + 1] - fi) * f) >> 26);
            fo += (int32_t)(((int64_t)(kCloudsXfadeOutQ30[i + 1] - fo) * f) >> 26);
        }
        int32_t k = m30(fi, 1288490189);                              /* 1.2 fade_in */
        for (int32_t c = 0; c < 2; ++c) {
            int32_t x = m30(in[2 * t + c], fo) + m30(out_[2 * t + c], k);
            output[2 * t + c] = (int16_t)clip16(trunc_shift(SoftLimitQ24(x >> 1), 9));
        }
    }
    dry_wet_ = dw;
}

/* ==== at 44.1 kHz ================================================================================================= */

void CloudsFx::Init(int16_t* buffer_l, int16_t* buffer_r, int32_t* diffuser, uint16_t* reverb)
{
    clouds_.Init(buffer_l, buffer_r, diffuser, reverb);
    for (int32_t c = 0; c < 2; ++c)
        for (int32_t i = 0; i < 32; ++i) { down_hist_[c][i] = 0; up_hist_[c][i] = 0; }
    down_pos_ = down_phase_ = up_pos_ = up_phase_ = 0;
    down_count_ = 1;
    block_fill_ = 0;
    fifo_read_ = 0;
    fifo_write_ = 50;                                       /* 50 frames of silence ahead: the blocks come in bursts */
    for (int32_t i = 0; i < 128 * 2; ++i) fifo_[i] = 0;
    fade_out_ = kQ30;
}

void CloudsFx::Process(const CloudsPatch& p, int32_t* io, size_t n)
{
    const int32_t taps = kCloudsSrcTaps;
    /* the dry side of the crossfade, at BLEND */
    int32_t dw = knob_q30(p.blend);
    uint32_t xi = (uint32_t)dw >> 26, xf = (uint32_t)dw & 0x3FFFFFFu;
    int32_t fo_target = kCloudsXfadeOutQ30[xi];
    if (xi < 16) fo_target += (int32_t)(((int64_t)(kCloudsXfadeOutQ30[xi + 1] - fo_target) * xf) >> 26);
    for (size_t s = 0; s < n; ++s) {
        int32_t dry[2] = { io[2 * s], io[2 * s + 1] };
        /* 44.1 -> 32 kHz */
        for (int32_t c = 0; c < 2; ++c) {
            int16_t v = (int16_t)clip16(dry[c]);
            down_hist_[c][down_pos_] = down_hist_[c][down_pos_ + 16] = v;
        }
        int32_t newest = down_pos_ + 16;
        down_pos_ = (down_pos_ + 1) & 15;
        if (--down_count_ == 0) {
            const int16_t* h = kCloudsSrcDown[down_phase_];
            for (int32_t c = 0; c < 2; ++c) {
                const int16_t* x = &down_hist_[c][newest];
                int32_t acc = 0;
                for (int32_t k = 0; k < taps; ++k) acc += h[k] * x[-k];
                block_in_[2 * block_fill_ + c] = acc >> 6;                         /* Q30 -> Q24 */
            }
            down_phase_ += 441;
            if (down_phase_ >= 640) { down_phase_ -= 640; down_count_ = 2; }
            else { down_phase_ -= 320; down_count_ = 1; }
            if (++block_fill_ == kCloudsBlock) {
                block_fill_ = 0;
                clouds_.Process(p, block_in_, block_wet_, kCloudsBlock);
                /* 32 -> 44.1 kHz, into the FIFO */
                for (int32_t t = 0; t < kCloudsBlock; ++t) {
                    for (int32_t c = 0; c < 2; ++c)
                        up_hist_[c][up_pos_] = up_hist_[c][up_pos_ + 16] = block_wet_[2 * t + c] >> 9;
                    int32_t up_newest = up_pos_ + 16;
                    up_pos_ = (up_pos_ + 1) & 15;
                    for (;;) {
                        const int16_t* hu = kCloudsSrcUp[up_phase_];
                        uint32_t w = (fifo_write_ & 127u) * 2;
                        for (int32_t c = 0; c < 2; ++c) {
                            const int32_t* x = &up_hist_[c][up_newest];
                            int32_t acc = 0;
                            for (int32_t k = 0; k < taps; ++k) acc += hu[k] * x[-k];
                            fifo_[w + c] = (acc + (1 << 14)) >> 15;
                        }
                        ++fifo_write_;
                        up_phase_ += 320;
                        if (up_phase_ >= 441) { up_phase_ -= 441; break; }
                    }
                }
            }
        }
        /* out: the wet signal (twice: the module's output stage halves) and the dry one */
        int32_t wl = 0, wr = 0;
        if (fifo_read_ != fifo_write_) {
            uint32_t r = (fifo_read_ & 127u) * 2;
            wl = fifo_[r]; wr = fifo_[r + 1];
            ++fifo_read_;
        }
        fade_out_ += (fo_target - fade_out_) >> 6;
        io[2 * s] = 2 * wl + m30(dry[0], fade_out_);
        io[2 * s + 1] = 2 * wr + m30(dry[1], fade_out_);
    }
}

}  // namespace fom
