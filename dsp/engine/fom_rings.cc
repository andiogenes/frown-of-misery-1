/* SPDX-License-Identifier: MIT */
#include "fom_rings.h"

#include "fom_rings_tables.h"
#ifdef FOM_RINGS_DEBUG
#include <cstdio>
#endif

namespace fom {

namespace {

/* ---- integer helpers (no float, no 64-bit division: the chip has neither) */

const int32_t INT32_MIN_ = (int32_t)0x80000000u;

/* n / d for a quotient that fits 32 bits: shift and subtract, constant shifts only (no __udivdi3, no __lshrdi3) */
uint32_t udiv64(uint64_t n, uint32_t d)
{
    /* the quotient fits 32 bits, so the high word of n is already below d: 32 steps, not 64 (each costs a
     * handful of instructions on a 32-bit chip) */
    uint64_t r;
    uint32_t q = 0, lo;
    int i;
    if (!d) return 0xFFFFFFFFu;
    if ((n >> 32) >= d) return 0xFFFFFFFFu;                            /* (would not fit: saturate) */
    if (!(n >> 32)) return (uint32_t)n / d;                            /* a 32-bit division does it */
    r = n >> 32;
    lo = (uint32_t)n;
    for (i = 31; i >= 0; --i) {
        r = (r << 1) | (lo >> 31);
        lo <<= 1;
        q <<= 1;
        if (r >= d) { r -= d; q |= 1u; }
    }
    return q;
}

inline int32_t mulq(int32_t a, int32_t b, int shift) { return (int32_t)(((int64_t)a * b) >> shift); }
inline int32_t mulhi(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }

/* 2^x, x in Q16 (-16..15), Q16 */
uint32_t exp2_q16(int32_t x)
{
    int32_t i = x >> 16;                                   /* floor */
    uint32_t frac = (uint32_t)x & 0xFFFFu, k = frac >> 8, t = frac & 0xFFu;
    uint32_t a = kExp2FracQ30[k], b = kExp2FracQ30[k + 1];
    uint32_t m = a + (uint32_t)(((uint64_t)(b - a) * t) >> 8);   /* 2^frac, Q30 */
    int sh = 14 - i;                                       /* Q30 * 2^i -> Q16 */
    if (sh >= 32) return 0;
    if (sh <= 0) return 0xFFFFFFFFu;
    return m >> sh;
}

/* Interpolate(table, x, 256) with x in 0..65535 */
template <typename T> inline int64_t lut(const T* t, uint32_t x)
{
    uint32_t i = x >> 8, f = x & 0xFFu;
    return (int64_t)t[i] + ((((int64_t)t[i + 1] - t[i]) * f) >> 8);
}

/* stmlib's OnePole::tan<FREQUENCY_FAST>: f (pi + f^2 (a + b f^2)), f in Q32 -> Q26 (up to 0.485: g < 21.3) */
int32_t tan_fast_q26(uint32_t f)
{
    /* a = 0.326 pi^3, b = 0.1823 pi^5 in Q24; pi in Q28 */
    const int64_t kA = 169584874, kB = 935957068, kPi = 843314857;
    int64_t fq = f;                                        /* Q32, < 0.485 */
    int64_t f2 = (fq * fq) >> 32;                          /* Q32 */
    int64_t poly = kA + ((kB * f2) >> 32);                 /* Q24 */
    int64_t inner = kPi + ((f2 * poly) >> 28);             /* Q28 */
    return (int32_t)((fq * inner) >> 34);                  /* Q32 * Q28 >> 34 = Q26 */
}

/* stmlib's FREQUENCY_DIRTY: f (pi + a f^2), Q32 -> Q24 */
int32_t tan_dirty_q24(uint32_t f)
{
    const int64_t kA = 194346347, kPi = 52707179;          /* a = 0.3736 pi^3, Q24; pi, Q24 */
    int64_t fq = f >> 8;                                   /* Q24 */
    int64_t inner = kPi + ((kA * ((fq * fq) >> 24)) >> 24);
    return (int32_t)((fq * inner) >> 24);
}

/* stmlib's SoftLimit: x (27 + x^2) / (27 + 9 x^2), Q24 -> Q24. The gain (27 + x^2) / (27 + 9 x^2) from a table for
 * |x| < 4 (it runs for every sample of both outputs: a division each would cost more than it is worth); out there,
 * rarely, through a 32-bit division, |x| clamped to 8 (SoftLimit is ~ +-1.2 there anyway). */
int32_t soft_limit(int32_t x)
{
    int32_t ax = x < 0 ? (x == INT32_MIN_ ? 0x7FFFFFFF : -x) : x;
    uint32_t gain;
    if (ax < (4 << 24)) {
        uint32_t i = (uint32_t)ax >> 18, f = ((uint32_t)ax >> 2) & 0xFFFFu;   /* x * 64: index, fraction Q16 */
        uint32_t a = kSoftLimitGainQ16[i], b = kSoftLimitGainQ16[i + 1];
        gain = a - (uint32_t)(((uint64_t)(a - b) * f) >> 16);         /* (the gain falls with |x|) */
    } else {
        if (ax > (8 << 24)) ax = 8 << 24;
        uint32_t x2 = (uint32_t)(((int64_t)ax * ax) >> 40);           /* Q8 */
        gain = ((27u * 256u + x2) << 16) / (27u * 256u + 9u * x2);
    }
    int32_t y = (int32_t)(((int64_t)ax * gain) >> 16);
    return x < 0 ? -y : y;
}

const int32_t kQ24 = 1 << 24;

}  // namespace

/* ---- one voice */

void ModalVoice::Init()
{
    for (int32_t i = 0; i < kMaxModes; ++i) { g_[i] = rg_[i] = h_[i] = 0; s1_[i] = s2_[i] = 0; amp_[i] = 0; }
    num_modes_ = 0;
    resolution_ = kMaxModes;
    frequency_ = kNoteFreqQ32[57];                         /* 220 Hz, Resonator::Init's */
    computed_.structure = computed_.brightness = computed_.damping = computed_.position = 0xFFFFFFFFu;
    dirty_ = true;
    idle_ = true;
    ex_s1_ = ex_s2_ = 0;
    strike_ = 0;
    pending_ = 0;
    has_pending_ = false;
    position_now_ = 0xFFFFFFFFu;
    silent_ = 0;
}

/* Resonator::ComputeFilters: the partials' frequencies (stretched by STRUCTURE), their Qs (DAMPING, brightness
 * losses). Returns the number of modes under 0.49 fs. brightness here is Rings' patch.brightness^2. */
int32_t ModalVoice::ComputeFilters(const RingsPatch& patch)
{
    const int64_t one30 = (int64_t)1 << 30;
    uint32_t structure = patch.structure, b = patch.brightness;
    /* stiffness and stretch in Q30, multiplied with rounding to nearest: truncating (toward -inf for a negative
     * stiffness) biased every step, and over 60 partials that put the top ones 1.6e-4 (0.3 cent) flat */
    int64_t stiffness = lut(kRingsStiffnessQ24, structure) << 6;      /* Q30 */
    const int64_t half = (int64_t)1 << 29;
    /* q = 500 * lut_4_decades(damping), Q8, times 44100 / 48000: the same decay times in seconds as at 48 kHz */
#ifdef FOM_RINGS_TEST_44K   /* (diagnostics: against Rings compiled at 44.1 kHz, which does not compensate) */
    int64_t q = lut(kRingsQBaseQ8, patch.damping);
#else
    int64_t q = (lut(kRingsQBaseQ8, patch.damping) * 30106) >> 15;
#endif
    int64_t s30 = (int64_t)structure << 14;                           /* structure, Q30 */
    int64_t b30 = (((int64_t)b * b) >> 16) << 14;                     /* brightness = b^2, Q30 */
    int64_t att = one30 - s30;                                        /* (1 - structure)^8 */
    att = (att * att) >> 30; att = (att * att) >> 30; att = (att * att) >> 30;
    int64_t brightness = (b30 * (one30 - ((att * 214748365) >> 30))) >> 30;   /* * (1 - 0.2 att) */
    int64_t q_loss = ((((brightness * (2 * one30 - brightness)) >> 30) * 912680550) >> 30) + 161061274;  /* *0.85+0.15 */
    int64_t q_loss_rate = (((s30 * (2 * one30 - s30)) >> 30) * 107374182) >> 30;   /* structure (2 - structure) 0.1 */
    int64_t stretch = (int64_t)1 << 30;                               /* Q30 */
    int64_t harmonic = 0;                                             /* (i + 1), the partial's number */
    int32_t num_modes = 0, n = resolution_ < kMaxModes ? resolution_ : kMaxModes;
    for (int32_t i = 0; i < n; ++i) {
        harmonic += 1;
        /* the partial frequency (i + 1) f stretch, Q32: f * stretch first, at full precision (f < 2^31, stretch
         * < 2^31), then the partial's number. Precision matters here: at STRUCTURE 0 the partials crowd together
         * and beat, and a few 1e-4 of error put them out of phase within half a second. */
        int64_t pf = harmonic * (((int64_t)frequency_ * ((stretch + 32) >> 6)) >> 24);
        /* Rings lets the modes reach 0.49 fs; here 0.485 (21.4 kHz), so that r + g = r + tan(pi f) < 22.3 fits
         * Q26. Only the top 0.2 kHz of ultrasonic partials is lost. */
        const int64_t kMaxF = 2083059139;
        if (pf >= kMaxF) pf = kMaxF; else num_modes = i + 1;
        int32_t g = tan_fast_q26((uint32_t)pf);
        /* resonance 1 + pf q (Q8); r = 1 / resonance, Q31 */
        uint64_t res = (uint64_t)256 + (uint64_t)(((uint64_t)pf * (uint64_t)q) >> 32);
        uint32_t r31 = udiv64((uint64_t)1 << 39, (uint32_t)(res > 0xFFFFFFFFull ? 0xFFFFFFFFull : res));
        int64_t r26 = r31 >> 5;
        int64_t den = ((int64_t)1 << 20) + ((r26 * g) >> 32) + (((int64_t)g * g) >> 32);   /* Q20, < 475 */
        g_[i] = g;
        rg_[i] = (int32_t)(r26 + g);
        uint32_t h = udiv64((uint64_t)1 << 51, (uint32_t)den);         /* Q31, < 1 (den rounds to 1 for tiny g) */
        h_[i] = (int32_t)(h > 0x7FFFFFFFu ? 0x7FFFFFFFu : h);
        /* stiffness: shrinks to keep the partials from folding (negative) or to add a few high ones (positive) */
        stretch += stiffness;
        stiffness = (stiffness * (stiffness < 0 ? 998579896 : 1052266988) + half) >> 30;   /* * .93 or .98 */
        q_loss += (q_loss_rate * (one30 - q_loss)) >> 30;
        q = (q * q_loss) >> 30;
    }
    return num_modes;
}

void ModalVoice::Render(const RingsPatch& patch, bool active, int32_t* odd, int32_t* even, size_t n)
{
    /* ---- the excitation: Part::Process's cutoff, RenderModalVoice's pulse, the excitation filter */
    uint32_t b = patch.brightness;
    uint32_t cut = (uint32_t)(((uint64_t)b * (131072u - b)) >> 16);   /* b (2 - b), Q16 */
    uint32_t fc;                                                       /* the excitation filter's cutoff, Q32 */
    if (active) {
        /* frequency * 2^((cut - 0.5) * 96 / 12) */
        uint32_t ratio = exp2_q16((int32_t)(((int64_t)cut - 32768) * 8));
        uint64_t f = ((uint64_t)frequency_ * ratio) >> 16;
        fc = f > 2143188681u ? 2143188681u : (uint32_t)f;              /* min(.., 0.499) */
    } else {
        fc = 973915u;                                                  /* 10 Hz */
    }
    if (fc < 97392u) fc = 97392u;                                      /* >= 1 Hz: the pulse stays in 64 bits */

    if (strike_ && active) {
        /* the pulse, 0.25 * SemitonesToRatio(24 fc^2) / fc = 0.25 * 2^(2 fc^2) / fc, injected as the filter's first
         * sample (no overflow, any fc) */
        uint64_t fc2 = ((uint64_t)fc * fc) >> 32;                      /* Q32 */
        uint32_t boost = exp2_q16((int32_t)((fc2 * 2) >> 16));         /* Q16 */
        uint32_t inv = udiv64((uint64_t)1 << 40, fc);                  /* 1 / fc, Q8 (<= 44100) */
        int64_t pulse = ((int64_t)inv * boost) >> 2;                   /* 0.25 * boost / fc, Q24 */
        pulse = (pulse * strike_) >> 15;
        /* the filter's state after an input sample of `pulse` (ex states start from where they are) */
        int32_t g = tan_dirty_q24(fc);
        const int64_t r = 11184811;                                    /* 1 / 1.5, Q24 */
        int64_t den = kQ24 + ((r * g) >> 24) + (((int64_t)g * g) >> 24);
        int64_t h = udiv64((uint64_t)1 << 48, (uint32_t)den);          /* Q24 */
        int64_t hp = (((pulse - (((r + g) * ex_s1_) >> 24) - ex_s2_)) * h) >> 24;
        int64_t bp = ((g * hp) >> 24) + ex_s1_;
        ex_s1_ = ((g * hp) >> 24) + bp;
        int64_t lp = ((g * bp) >> 24) + ex_s2_;
        ex_s2_ = ((g * bp) >> 24) + lp;
        strike_ = 0;
        idle_ = false;
        silent_ = 0;
        /* the first output sample is lp: fed below as the pending input */
        pending_ = (int32_t)lp;
        has_pending_ = true;
    }
    strike_ = 0;
    if (idle_) return;

    /* ---- the modes' coefficients, when the note or a knob moved */
    if (dirty_ || patch.structure != computed_.structure || patch.brightness != computed_.brightness ||
        patch.damping != computed_.damping) {
        num_modes_ = ComputeFilters(patch);
        computed_ = patch;
        dirty_ = false;
    }
    /* ---- the pickup amplitudes along the modes: CosineOscillator<APPROXIMATE>(position) */
    if (patch.position != position_now_) {
        position_now_ = patch.position;
        int64_t f = ((int64_t)patch.position << 14) - ((int64_t)1 << 28);    /* position - 0.25, Q30 */
        int64_t sign = 16;
        if (f < 0) f = -f;
        else if (f > ((int64_t)1 << 29)) f -= (int64_t)1 << 29;
        else sign = -16;
        int64_t c = sign * ((f * (((int64_t)1 << 30) - 2 * f)) >> 30);       /* iir coefficient, Q30 */
        int64_t y1 = c >> 2, y0 = (int64_t)1 << 29;                           /* initial amplitude, 0.5 */
        for (int32_t i = 0; i < kMaxModes; ++i) {
            int64_t t = y0;
            y0 = ((c * y0) >> 30) - y1;
            y1 = t;
            int64_t a = (t + ((int64_t)1 << 29)) >> 15;                       /* + 0.5, Q15 */
            amp_[i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
        }
    }

    /* ---- the excitation filter's coefficients for this block */
    int32_t eg = tan_dirty_q24(fc);
    const int64_t er = 11184811;
    int64_t eden = kQ24 + ((er * eg) >> 24) + (((int64_t)eg * eg) >> 24);
    int64_t eh = udiv64((uint64_t)1 << 48, (uint32_t)eden);
    bool ex_live = ex_s1_ || ex_s2_ || has_pending_;

    /* ---- the resonators' input for this run (n <= 32): the excitation filter's output * 0.125 */
    int32_t xin[32];
    bool any_input = false;
    if (n > 32) n = 32;
    for (size_t k = 0; k < n; ++k) {
        int32_t x = 0;                                                 /* Q24 */
        if (has_pending_) {
            x = pending_;
            has_pending_ = false;
        } else if (ex_live) {
            int64_t hp = ((-(((er + eg) * ex_s1_) >> 24) - ex_s2_) * eh) >> 24;
            int64_t bp = ((eg * hp) >> 24) + ex_s1_;
            ex_s1_ = ((eg * hp) >> 24) + bp;
            int64_t lp = ((eg * bp) >> 24) + ex_s2_;
            ex_s2_ = ((eg * bp) >> 24) + lp;
            x = (int32_t)lp;
        }
        xin[k] = x >> 3;                                               /* Resonator::Process: input * 0.125 */
        any_input |= xin[k] != 0;
    }

    /* ---- the modes: mode-outer, sample-inner, so a mode's state and coefficients stay in registers for the whole
     * run (Rings loops the other way; every mode is independent, so the result is the same). Odd modes to o[],
     * even to e[] (Resonator::Process's odd / even), each scaled by its pickup amplitude. A mode that has decayed
     * under ~-130 dB with no input coming sleeps: high partials die fast, so a ringing note soon needs far fewer
     * than its 60 modes. */
    int32_t o[32], e[32];
    for (size_t k = 0; k < n; ++k) o[k] = e[k] = 0;
    const int32_t modes = num_modes_;
    for (int32_t i = 0; i < modes; ++i) {
        int32_t s1 = s1_[i], s2 = s2_[i];
        if (!any_input && s1 < 4 && s1 > -4 && s2 < 4 && s2 > -4) {   /* asleep */
            s1_[i] = s2_[i] = 0;
            continue;
        }
        const int32_t g = g_[i], rg = rg_[i], h = h_[i], a = amp_[i];
        int32_t* __restrict d = (i & 1) ? e : o;
        for (size_t k = 0; k < n; ++k) {
            /* stmlib Svf, band-pass */
            /* (x - rg s1 - s2) h: the high word of the product, doubled. Written as (a * h) >> 31 the compiler
             * folds the shift into h, which then needs 64 bits, and makes it three multiplies. */
            int32_t hp = mulhi(xin[k] - mulq(rg, s1, 26) - s2, h) << 1;
            int32_t ghp = mulq(g, hp, 26);
            int32_t bp = ghp + s1;
            s1 = ghp + bp;
            int32_t gbp = mulq(g, bp, 26);
            int32_t lp = gbp + s2;
            s2 = gbp + lp;
            d[k] += (int32_t)(((int64_t)a * bp) >> 15);               /* Q15 * Q24 >> 15 = Q24 */
        }
        s1_[i] = s1;
        s2_[i] = s2;
    }
    int32_t peak = 0;
    for (size_t k = 0; k < n; ++k) {
        int32_t ov = o[k], ev = e[k];
        odd[k] += ov;
        even[k] += ev;
        int32_t a = ov < 0 ? -ov : ov, c = ev < 0 ? -ev : ev;
        if (a > peak) peak = a;
        if (c > peak) peak = c;
    }
    if (ex_live && ex_s1_ > -64 && ex_s1_ < 64 && ex_s2_ > -64 && ex_s2_ < 64) ex_s1_ = ex_s2_ = 0;
    /* asleep when it has been under -96 dB for half a second: the modes are cleared and cost nothing */
    if (peak < 256) {
        silent_ += (uint32_t)n;
        if (silent_ > 22050u && !ex_s1_ && !ex_s2_) {
            for (int32_t i = 0; i < kMaxModes; ++i) s1_[i] = s2_[i] = 0;
            idle_ = true;
        }
    } else {
        silent_ = 0;
    }
}

/* ---- the part */

void RingsModal::Init()
{
    for (int32_t i = 0; i < kMaxPolyphony; ++i) { voice_[i].Init(); note_[i] = 60; }
    polyphony_ = 0;
    active_ = 0;
    fine_ = 0;
    peak_ = kQ24 / 2;                                                  /* Limiter::Init */
    set_polyphony(1);
}

void RingsModal::set_polyphony(int32_t p)
{
    p = p >= 4 ? 4 : p >= 2 ? 2 : 1;
    if (p == polyphony_) return;
    polyphony_ = p;
    for (int32_t i = 0; i < kMaxPolyphony; ++i) {
        voice_[i].Init();
        voice_[i].set_resolution(64 / p - 4);                          /* Part::ConfigureResonators */
    }
    if (active_ >= polyphony_) active_ = 0;
}

static uint32_t note_freq(uint32_t note, int32_t fine)                  /* fine: 1/128 semitone */
{
    uint64_t f = kNoteFreqQ32[note > 127u ? 127u : note];
    if (fine) f = (f * exp2_q16(fine * 65536 / 1536)) >> 16;
    return f > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)f;
}

void RingsModal::Strike(uint32_t note, int32_t fine, uint32_t velocity)
{
    fine_ = fine;
    /* Part::Process on a strum: the next voice takes it (Rings' round robin; its ping pattern only for 3 voices) */
    if (polyphony_ > 1) active_ = (active_ + 1) % polyphony_;
    note_[active_] = (uint8_t)(note > 127u ? 127u : note);
    voice_[active_].set_frequency(note_freq(note_[active_], fine_));
    voice_[active_].Strike((int32_t)(velocity > 127u ? 127u : velocity) * 258);
}

bool RingsModal::sounding() const
{
    for (int32_t i = 0; i < polyphony_; ++i)
        if (!voice_[i].idle()) return true;
    return false;
}

void RingsModal::Render(const RingsPatch& patch, int32_t level, int32_t* out, size_t n)
{
    for (size_t s0 = 0; s0 < n; s0 += 32) {
        size_t m = n - s0 < 32 ? n - s0 : 32;
        for (size_t k = 0; k < m; ++k) odd_[k] = even_[k] = 0;
        for (int32_t v = 0; v < polyphony_; ++v) {
            voice_[v].set_frequency(note_freq(note_[v], fine_));
            if (polyphony_ == 1) {
                voice_[v].Render(patch, v == active_, odd_, even_, m);
            } else {
                /* odd and even voices to their own outputs: out_buffer - aux_buffer */
                int32_t o[32], e[32];
                for (size_t k = 0; k < m; ++k) o[k] = e[k] = 0;
                voice_[v].Render(patch, v == active_, o, e, m);
                int32_t* d = (v & 1) ? even_ : odd_;
                for (size_t k = 0; k < m; ++k) d[k] += o[k] - e[k];
            }
        }
        /* Limiter::Process with Rings' modal gain 1.4: a peak follower (attack 0.05, release 0.00002 per 48 kHz
         * sample, here per 44.1 kHz one), gain 1 / peak above 1, then 0.8 and SoftLimit. */
        for (size_t k = 0; k < m; ++k) {
            int32_t l = mulq(odd_[k], 23488102, 24), r = mulq(even_[k], 23488102, 24);   /* * 1.4 */
            int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r, d = r - l, ad = d < 0 ? -d : d;
            int32_t pk = al > ar ? al : ar;
            if (ad > pk) pk = ad;
            int32_t err = pk - peak_;
#ifdef FOM_RINGS_TEST_44K
            peak_ += err > 0 ? mulq(err, 3355443, 26) : mulq(err, 1342, 26);   /* .05 / .00002, as Rings per sample */
#else
            peak_ += err > 0 ? mulq(err, 3643983, 26) : mulq(err, 1461, 26);   /* .0543 / .0000218 */
#endif
            int32_t gain = peak_ <= kQ24 ? kQ24 : (int32_t)(((1u << 31) / ((uint32_t)peak_ >> 9)) << 8);   /* 2^16/p << 8 = 1/p, Q24 */
            int32_t gl = mulq(mulq(l, gain, 24), 13421773, 24), gr = mulq(mulq(r, gain, 24), 13421773, 24);   /* * .8 */
#ifdef FOM_RINGS_TEST_NOLIMIT   /* (diagnostics: the resonators alone, times Rings' gain 1.4) */
            int32_t yl = l >> 9, yr = r >> 9;
            (void)gl; (void)gr;
#else
            int32_t yl = soft_limit(gl) >> 9, yr = soft_limit(gr) >> 9;   /* Q24 -> Q15 */
#endif
            out[2 * (s0 + k)] += mulq(yl, level, 15);
            out[2 * (s0 + k) + 1] += mulq(yr, level, 15);
        }
    }
}


/* ==== the string models ============================================================================================ */

namespace {

/* stmlib's Random: one generator for the whole part (Rings' is a static: the plucker and the dispersion noise draw
 * from it in turn), the same LCG and seed, so the noise matches Rings' sample for sample. */
uint32_t g_rng = 0x21;
inline uint32_t rng_word() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }
/* 2 * Random::GetFloat() - 1, Q24. GetFloat converts the word to a float first: rounded to 24 bits, as here. */
inline int32_t rng_noise()
{
    uint32_t w = rng_word();
    uint64_t v = w;
    if (w >= (1u << 24)) {
        int msb = 31;
        while (!(w >> msb)) --msb;
        int drop = msb - 23;
        uint32_t q = w >> drop, rem = w & ((1u << drop) - 1u), half = 1u << (drop - 1);
        if (rem > half || (rem == half && (q & 1u))) ++q;               /* round to nearest, ties to even */
        v = (uint64_t)q << drop;
    }
    return (int32_t)(v >> 7) - (1 << 24);                              /* v / 2^32 * 2 - 1, Q24 */
}

/* stmlib's tan<FREQUENCY_ACCURATE>, f in Q32 (< 0.5) -> Q19 (it reaches ~2000 near Nyquist) */
int32_t tan_accurate_q19(uint32_t f)
{
    /* a..e times pi^3..pi^11, and pi, Q20 */
    const int64_t kA = 10837416, kB = 42803606, kC = 169036022, kD = 90662018, kE = 2935890615LL, kPi = 3294199;
    int64_t f2 = ((int64_t)f * f) >> 32;                               /* Q32 */
    int64_t p = kE;
    p = kD + ((f2 * p) >> 32);
    p = kC + ((f2 * p) >> 32);
    p = kB + ((f2 * p) >> 32);
    p = kA + ((f2 * p) >> 32);
    int64_t inner = kPi + ((f2 * p) >> 32);                            /* Q20 */
    int64_t g = ((int64_t)f * inner) >> 33;                            /* Q32 * Q20 >> 33 = Q19 */
    return (int32_t)(g > 0x7FFFFFFF ? 0x7FFFFFFF : g);
}

/* 2^x for x in [0, 1) (Q16), Q30: the table and a linear step (~1e-6 relative) */
uint32_t exp2_frac_q30(uint32_t x)
{
    uint32_t i = x >> 8, t = x & 0xFFu;
    uint32_t a = kExp2FracQ30[i], b = kExp2FracQ30[i + 1];
    return a + (uint32_t)(((uint64_t)(b - a) * t + 128) >> 8);
}

/* stmlib's SemitonesToRatio, Q30, from semitones Q24. Rings' is a pair of tables that truncate the fraction to 1/256
 * semitone, and Rings uses it for every pitch and time of the string models (the sympathetic tunings, the glide, the
 * damping filter, even the loss per period): the port truncates the same way, or its strings drift from Rings'. */
uint64_t st_ratio_q30(int64_t semis_q24)
{
    /* Rings computes semitones + 128 in float32 first: rounded to the float grid for its size (2^-16 semitone from
     * 128 to 256, 2^-17 below), which decides which side of a 1/256 step a note on a semitone falls */
    int64_t pitch = semis_q24 + ((int64_t)128 << 24);
    if (pitch >= ((int64_t)1 << 24)) {
        /* pitch in [2^e, 2^(e+1)) semitones: float spacing 2^(e - 23) semitones = 2^(e + 1) in Q24 */
        int32_t e = 0;
        while ((pitch >> 24) >> (e + 1)) ++e;
        int32_t sh = e + 1;
        pitch = ((pitch + ((int64_t)1 << (sh - 1))) >> sh) << sh;     /* round to nearest */
        semis_q24 = pitch - ((int64_t)128 << 24);
    }
    int64_t q = semis_q24 >> 16;                                       /* 1/256 semitone, floor */
    if (q < -32512) q = -32512;                                        /* -127 semitones */
    if (q > 32512) q = 32512;
    int32_t qp = (int32_t)q + 33792;                                   /* + 11 octaves: positive */
    int32_t k = qp / 3072, r = qp - k * 3072;
    uint64_t m = exp2_frac_q30((uint32_t)(((uint32_t)r << 16) / 3072u));
    int32_t sh = k - 11;
    return sh >= 0 ? m << sh : m >> -sh;
}

/* a note in semitones Q24 (MIDI) -> frequency / fs, Q32: the integer note from the table, the fraction through
 * SemitonesToRatio's truncation (Rings: SemitonesToRatio(note - 69) * a3) */
uint32_t note_freq_q24(int32_t note)
{
    if (note < 0) note = 0;
    if (note > (127 << 24) || note < 0) note = 127 << 24;
    /* as Rings: a3 * SemitonesToRatio(note - 69), a3 = 440 / fs (the table's A4) */
    uint64_t f = ((uint64_t)kNoteFreqQ32[69] * st_ratio_q30((int64_t)note - ((int64_t)69 << 24))) >> 30;
    return f > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)f;
}
inline uint32_t note_freq_q16(int32_t note) { return note_freq_q24(note < (128 << 16) ? note << 8 : 127 << 24); }

/* Part::Squash in Q31: 1 - squash is a few 1e-6 near the top, and that decides which side of a 1/256-semitone step a
 * sympathetic string lands. Returns the distance from the nearer end and which end (squash = top ? 1 - d : d). */
uint32_t squash_q31(uint32_t x, bool* top)
{
    uint64_t y;
    *top = x >= (1u << 30);
    y = *top ? ((uint64_t)1 << 31) - 2 * (uint64_t)(x - (1u << 30))   /* 2 - 2x, x in [.5, 1): (0, 1], Q31 */
             : 2 * (uint64_t)x;                                        /* 2x, x in [0, .5): [0, 1) */
    for (int k = 0; k < 4; ++k) y = (y * y) >> 31;
    return (uint32_t)(y >> 1);                                         /* 0.5 y^16 */
}

/* a * b >> shift, rounded to nearest: the string models' filters are recursive with poles near 1 (the input's DC
 * blocker's time constant is ~4400 samples), and a truncating shift biases every sample by up to one LSB, which
 * they pile up: the DC blocker's tail came out half as long. */
inline int64_t mulr(int64_t a, int64_t b, int shift) { return (a * b + ((int64_t)1 << (shift - 1))) >> shift; }
/* the same with 32-bit operands: one hardware multiply instead of a 64 x 64 product's three. Identical results
 * wherever both operands fit 32 bits, which the strings' samples (Q24) and coefficients (Q16..Q30) do. */
inline int64_t mrw(int32_t a, int32_t b, int shift) { return ((int64_t)a * b + ((int64_t)1 << (shift - 1))) >> shift; }
/* d / 24 (Rings' block), rounded, as a multiply: no 64-bit division on the chip. |d| < 2^35. */
inline int64_t div24(int64_t d) { return (d * 178956971 + ((int64_t)1 << 31)) >> 32; }

const int32_t kDcPartLeak = 243479;    /* 10 / 44100, Q30: 1 - the pole of Part's input DC blocker */
const int32_t kDcStringLeak = 486958;  /* 20 / 44100, Q30: the dispersion path's (y pole = y - y leak: no overflow) */

}  // namespace

/* x (Q32, positive) rounded to float32's grid, ties to even: Rings accumulates the slowed-down string's phase in
 * float, and which samples the string advances on (each one draws from the random generator, with dispersion)
 * follows that rounding; a 1e-5 drift over 26000 samples moved one, and every later noise burst with it. */
static uint64_t float_grid(uint64_t x)
{
    if (x < ((uint64_t)1 << 24)) return x;
    int m = 0;                                                         /* the top bit, by halves */
    uint64_t y = x;
    if (y >> 32) { y >>= 32; m += 32; }
    if (y >> 16) { y >>= 16; m += 16; }
    if (y >> 8) { y >>= 8; m += 8; }
    if (y >> 4) { y >>= 4; m += 4; }
    if (y >> 2) { y >>= 2; m += 2; }
    if (y >> 1) { m += 1; }
    int drop = m - 23;
    uint64_t q = x >> drop, rem = x & (((uint64_t)1 << drop) - 1), half = (uint64_t)1 << (drop - 1);
    if (rem > half || (rem == half && (q & 1))) ++q;
    return q << drop;
}

/* the same for a signed value (Q48 here): float32's rounding of a result */
static int64_t float_grid_signed(int64_t x)
{
    return x < 0 ? -(int64_t)float_grid((uint64_t)-x) : (int64_t)float_grid((uint64_t)x);
}

/* ---- one string */

void RingsString::Init(bool dispersion)
{
    dispersion_on_ = dispersion;
    for (int32_t i = 0; i < kStringDelay; ++i) line_[i] = 0;
    if (stretch_) for (int32_t i = 0; i < kStringDelay / 2; ++i) stretch_[i] = 0;
    write_ = stretch_write_ = 0;
    fir_x_ = fir_x__ = fir_brightness_ = fir_brightness_inc_ = fir_damping_ = fir_damping_inc_ = 0;
    iir_g_ = iir_rg_ = iir_h_ = iir_s1_ = iir_s2_ = 0;
    frequency_ = kNoteFreqQ32[57];                                     /* 220 Hz */
    dispersion_ = 16384; brightness_ = 32768; damping_ = 19661; position_ = 52429;   /* .25 .5 .3 .8 */
    delay_ = (int64_t)udiv64((uint64_t)1 << 48, frequency_) << 8;
    clamped_position_ = 0;
    previous_dispersion_ = 0;
    dispersion_noise_ = 0;
    curved_bridge_ = 0;
    previous_damping_compensation_ = 0;
    out_sample_[0] = out_sample_[1] = aux_sample_[0] = aux_sample_[1] = 0;
    dc_x_ = dc_y_ = 0;
    src_phase_ = 0;
    cache_f_ = cache_eff_ = cache_rt60_ = 0xFFFFFFFFu;
    cache_g_ = -1;
    cache_period_ = 0;
    cache_x_ = 0;
    cache_h_ = 0;
}

void RingsString::set_frequency(uint32_t f, int32_t glide_q30)
{
    int64_t d = (int64_t)f - frequency_;
    frequency_ = (uint32_t)((int64_t)frequency_ + ((d * glide_q30) >> 30));
}

/* DelayLine<float, 2048>::ReadHermite / Read: the line is written backwards, read at write + delay */
int32_t RingsString::ReadHermite(uint32_t delay) const
{
    const uint32_t m = kStringDelay - 1;
    uint32_t t = write_ + (delay >> 16) + kStringDelay;
    int32_t xm1 = line_[(t - 1) & m], x0 = line_[t & m], x1 = line_[(t + 1) & m], x2 = line_[(t + 2) & m];
    int32_t f = (int32_t)(delay & 0xFFFFu);                            /* Q16 */
    int32_t c = (x1 - xm1) / 2, v = x0 - x1, w = c + v;
    int32_t a = w + v + (x2 - x0) / 2, b_neg = w + a;
    int32_t y = (int32_t)(mrw((int32_t)(mrw(a, f, 16) - b_neg), f, 16) + c);
    return (int32_t)(mrw(y, f, 16) + x0);
}

int32_t RingsString::Read(uint32_t delay) const
{
    const uint32_t m = kStringDelay - 1;
    uint32_t t = write_ + (delay >> 16);
    int64_t a = line_[t & m], b = line_[(t + 1) & m];
    return (int32_t)(a + mrw((int32_t)(b - a), (int32_t)(delay & 0xFFFFu), 16));
}

void RingsString::Process(const int32_t* in, int32_t* out, int32_t* aux, int32_t n)
{
    if (dispersion_on_) ProcessInternal<true>(in, out, aux, n);
    else ProcessInternal<false>(in, out, aux, n);
}

template <bool kDispersion>
void RingsString::ProcessInternal(const int32_t* in, int32_t* out, int32_t* aux, int32_t n)
{
    const int64_t one16 = 65536, one30 = (int64_t)1 << 30;
    /* the period, clamped to the line; under fs / 2044 the string runs slower and is interpolated (src) */
    /* the block's divisions, each ~32 steps of 64-bit arithmetic on this chip: only when their inputs moved
     * (the note and the knobs change rarely; a gliding sympathetic string settles in a few hundred blocks) */
    if (frequency_ != cache_f_) {
        cache_f_ = frequency_;
        cache_period_ = (uint32_t)udiv64((uint64_t)1 << 48, frequency_ ? frequency_ : 1u);
    }
    uint32_t period = cache_period_;                                   /* 1 / f, Q16 */
    uint32_t delay = period < (4u << 16) ? (4u << 16) : period > (2044u << 16) ? (2044u << 16) : period;
    uint64_t src_ratio = float_grid(((uint64_t)delay * frequency_) >> 16);                 /* Q32, a float product */
    const uint64_t kOne = (uint64_t)1 << 32;
    if (src_ratio >= 4294537799ull) { src_phase_ = kOne; src_ratio = kOne; }               /* >= 0.9999 */
    int64_t pc = (int64_t)position_ - 32768;
    uint32_t clamped_position = (uint32_t)(32768 - ((((pc < 0 ? -pc : pc)) * 16441672 + (1 << 23)) >> 24));   /* .5 - .98 |p - .5| */

    /* the interpolated parameters (ParameterInterpolator: Next() steps, then returns; the end is stored back) */
    /* (n is always Rings' 24: RingsStrings renders whole blocks) */
    int64_t delay_v = delay_, delay_inc = div24(((int64_t)delay << 8) - delay_);
    int64_t pos_v = clamped_position_, pos_inc = div24(((int64_t)clamped_position << 8) - clamped_position_);
    int64_t disp_v = previous_dispersion_, disp_inc = div24(((int64_t)dispersion_ << 8) - previous_dispersion_);

    /* the decay: rt60 = 0.07 s * 2^(8 lf), lf = d (2 - d); the loss per period 2^(-10 delay / rt60) */
    uint32_t d = damping_, b = brightness_;
    uint32_t lf = (uint32_t)(((uint64_t)d * (131072u - d)) >> 16);    /* Q16 */
    uint32_t rt60_q4 = (uint32_t)(((uint64_t)49392 * st_ratio_q30((int64_t)lf * 96 << 8)) >> 30);   /* .07 fs 16 */
    uint32_t eff = src_ratio == kOne ? delay : period;                 /* delay / src_ratio */
    if (eff != cache_eff_ || rt60_q4 != cache_rt60_) {
        cache_eff_ = eff;
        cache_rt60_ = rt60_q4;
        cache_x_ = (int64_t)udiv64((uint64_t)eff * 10u << 12, rt60_q4 ? rt60_q4 : 1u);
    }
    int64_t x = cache_x_;                                              /* octaves, Q24 */
    if (x > 177602560) x = 177602560;                                  /* -127 semitones */
    /* SemitonesToRatio(-12 x): the loss per period, truncated to 1/256 semitone as Rings' is */
    int64_t coef = (int64_t)st_ratio_q30(-12 * x);                     /* Q30 */
    int64_t bright = (((int64_t)b * b) >> 16) << 14;                   /* b^2, Q30 */
    /* the IIR damping's cutoff, semitones above the note: min(24 + 48 d^2 + 24 b^2, 84) */
    int64_t cut = (24 << 16) + ((48 * (((int64_t)d * d) >> 16))) + ((24 * (((int64_t)b * b) >> 16)));
    if (cut > (84 << 16)) cut = 84 << 16;
    /* (the ratio reaches 2^10.7 here: Q30 times a Q32 frequency would overflow 64 bits, so the ratio goes in Q20) */
    uint64_t df = ((uint64_t)frequency_ * (st_ratio_q30(cut << 8) >> 10)) >> 20;   /* Q32 */
    if (df > 2143188681u) df = 2143188681u;                            /* 0.499 */
    if (d >= 62259u) {                                                 /* d >= 0.95: crossfade to infinite decay */
        int64_t t = (int64_t)(d - 62259u) * 20;                        /* Q16 */
        coef += (t * (one30 - coef)) >> 16;
        bright += (t * (one30 - bright)) >> 16;
        df += (uint64_t)((t * (int64_t)(2147054162u - df)) >> 16);     /* 0.4999 */
        cut += (t * ((128 << 16) - cut)) >> 16;
    }
    fir_damping_inc_ = (int32_t)div24(coef - fir_damping_);
    fir_brightness_inc_ = (int32_t)div24(bright - fir_brightness_);
    /* the IIR (Q 0.5: r = 2, 1 + r g + g^2 = (1 + g)^2) */
    iir_g_ = tan_accurate_q19((uint32_t)df);
    iir_rg_ = iir_g_ + (2 << 19);
    {
        if (iir_g_ != cache_g_) {
            cache_g_ = iir_g_;
            uint32_t inv = udiv64((uint64_t)1 << 49, (uint32_t)(iir_g_ + (1 << 19)));   /* 1 / (1 + g), Q30 */
            cache_h_ = (int32_t)(((uint64_t)inv * inv) >> 30);
        }
        iir_h_ = cache_h_;
    }
    /* the delay the damping filters add, taken off the loop: 1 - lut_svf_shift(cutoff) */
    int64_t comp = one30 - lut(kRingsSvfShiftQ30, (uint32_t)(cut >> 8) > 65535u ? 65535u : (uint32_t)(cut >> 8));
    int64_t comp_v = previous_damping_compensation_, comp_inc = div24(comp - previous_damping_compensation_);
#ifdef FOM_RINGS_DEBUG
    {
        static int printed = 0;
        if (printed++ < 1)
            fprintf(stderr, "string: period %.4f delay %.4f src %.5f rt60 %.1f x %.6f coef %.9f bright %.6f cut %.4f "
                    "df %.6f g %.6f h %.8f comp %.6f\n", period / 65536.0, delay / 65536.0, src_ratio / 4294967296.0,
                    rt60_q4 / 16.0, x / 16777216.0, coef / 1073741824.0, bright / 1073741824.0, cut / 65536.0,
                    df / 4294967296.0, iir_g_ / 524288.0, iir_h_ / 1073741824.0, comp / 1073741824.0);
    }
#endif
    /* dispersion: the noise's low-pass and gain, per block */
    int64_t noise_filter = 0, noise_gain = 0;
    if (kDispersion) {
        noise_filter = (int64_t)st_ratio_q30((((int64_t)b - 65536) * 48) << 8);   /* SemitonesToRatio((b - 1) 48) */
        /* 1 / (.2 + nf) reaches 3.8: Q28 (in Q30 it did not fit the division's 32-bit quotient) */
        noise_gain = (int64_t)udiv64((uint64_t)1 << 58, (uint32_t)(214748365 + noise_filter));
    }

    for (int32_t k = 0; k < n; ++k) {
        bool advance = true;
        if (src_ratio != kOne) {                                       /* slowed down (under 21.6 Hz): rare */
            src_phase_ = float_grid(src_phase_ + src_ratio);
            advance = src_phase_ > kOne;
            if (advance) src_phase_ -= kOne;                           /* (exact in float too) */
        }
        if (advance) {
            delay_v += delay_inc;
            pos_v += pos_inc;
            comp_v += comp_inc;
            int64_t dl = (delay_v + 128) >> 8;                         /* Q16 */
            int64_t comb_delay = mrw((int32_t)dl, (int32_t)pos_v, 24);
            dl = mrw((int32_t)dl, (int32_t)comp_v, 30);                 /* the IIR's delay */
            dl -= one16;                                               /* the FIR's */
            int32_t s;
            if (kDispersion) {
                int64_t noise = ((int64_t)rng_noise() * noise_gain) >> 28;
                dispersion_noise_ += (int32_t)((noise_filter * (noise - dispersion_noise_)) >> 30);
                disp_v += disp_inc;
                int64_t disp = (disp_v + 128) >> 8;                    /* Q16 */
                int64_t stretch_point = disp <= 0 ? 0 : ((((disp * (131072 - disp)) >> 16) * 31130) >> 16);   /* .475 */
                int64_t noise_amount = disp > 49152 ? 4 * (disp - 49152) : 0;
                int64_t bridge = disp < 0 ? -disp : 0;
                noise_amount = (((noise_amount * noise_amount) >> 16) * 1638) >> 16;   /* * .025 */
                int64_t ac_blocking = bridge;
                bridge = (((bridge * bridge) >> 16) * 655) >> 16;      /* * .01 */
                int64_t ad = disp < 0 ? -disp : disp;
                /* -.618 d / (.15 + |d|), Q16, through a 32-bit division */
                int64_t ap_gain = -(int64_t)((int32_t)(40501 * (int32_t)(disp >> 2)) / (int32_t)((9830 + ad) >> 2));
                int64_t fm = one16 + ((((int64_t)dispersion_noise_ >> 8) * noise_amount) >> 16) -
                             ((((int64_t)curved_bridge_ >> 8) * bridge) >> 16);   /* Q16 */
                dl = (dl * fm) >> 16;
                int64_t ap_delay = (dl * stretch_point) >> 16, main_delay = dl - ap_delay;
                if (ap_delay >= (4 << 16) && main_delay >= (4 << 16) && stretch_) {
                    s = ReadHermite((uint32_t)main_delay);
                    /* DelayLine<float, 1024>::Allpass with an integer delay */
                    const uint32_t m2 = kStringDelay / 2 - 1;
                    int64_t read = stretch_[(stretch_write_ + (uint32_t)(ap_delay >> 16)) & m2];
                    int64_t write = s + ((ap_gain * read) >> 16);
                    stretch_[stretch_write_] = (int32_t)write;
                    stretch_write_ = (stretch_write_ - 1u) & m2;
                    s = (int32_t)(((-write * ap_gain) >> 16) + read);
                } else {
                    s = ReadHermite((uint32_t)(dl < 0 ? 0 : dl));
                }
                /* the DC blocker, mixed in by the bridge's curving; then the curved bridge from the result */
                dc_y_ = dc_y_ - mulr(dc_y_, kDcStringLeak, 30) + ((int64_t)(s - dc_x_) << 16);
                dc_x_ = s;
                int32_t s_ac = (int32_t)((dc_y_ + 32768) >> 16);
                s += (int32_t)((ac_blocking * (s_ac - s)) >> 16);
                int32_t value = (s < 0 ? -s : s) - 419430;            /* |s| - .025 */
                int32_t av = value < 0 ? -value : value;
                curved_bridge_ = s > 0 ? av + value : -(((av + value) * 3) >> 1);   /* sign 1 or -1.5 */
            } else {
                s = ReadHermite((uint32_t)(dl < 0 ? 0 : dl));
            }
#ifdef FOM_RINGS_DEBUG
            int32_t dbg_read = s;
#endif
            s += in[k];
            /* DampingFilter: damping (h0 x_ + h1 (x + x__)), h0 = (1 + b) / 2, h1 = (1 - b) / 4 */
            {
                int64_t h0 = (one30 + fir_brightness_) >> 1, h1 = (one30 - fir_brightness_) >> 2;
                int64_t y = mrw((int32_t)h0, fir_x_, 30) + mrw((int32_t)h1, s + fir_x__, 30);
                y = mrw((int32_t)y, fir_damping_, 30);
                fir_x__ = fir_x_;
                fir_x_ = s;
                fir_brightness_ += fir_brightness_inc_;
                fir_damping_ += fir_damping_inc_;
                s = (int32_t)y;
            }
#ifdef FOM_RINGS_DEBUG
            int32_t dbg_fir = s;
#endif
            /* the IIR low-pass (stmlib Svf) */
            {
                int64_t hp = mrw((int32_t)((int64_t)s - mrw(iir_rg_, iir_s1_, 19) - iir_s2_), iir_h_, 30);
                int64_t ghp = mrw(iir_g_, (int32_t)hp, 19);
                int64_t bp = ghp + iir_s1_;
                iir_s1_ = (int32_t)(ghp + bp);
                int64_t gbp = mrw(iir_g_, (int32_t)bp, 19);
                int64_t lp = gbp + iir_s2_;
                iir_s2_ = (int32_t)(gbp + lp);
                s = (int32_t)lp;
            }
#ifdef FOM_RINGS_DEBUG
            {
                static long dbg_n = 0;
                if (dbg_n < 400 || (dbg_n % 5000) == 0 || ((dbg_n / 24) % 8 == 3 && dbg_n < 60000))
                    fprintf(stderr, "%ld d %.5f read %.7f in %.7f out %.7f comb %.5f fir %.9f\n", dbg_n, dl / 65536.0,
                            dbg_read / 16777216.0, in[k] / 16777216.0, s / 16777216.0, comb_delay / 65536.0,
                            dbg_fir / 16777216.0);
                dbg_n++;
            }
#endif
            line_[write_] = s;
            write_ = (write_ - 1u) & (kStringDelay - 1);
            out_sample_[1] = out_sample_[0];
            aux_sample_[1] = aux_sample_[0];
            out_sample_[0] = s;
            aux_sample_[0] = Read((uint32_t)(comb_delay < 0 ? 0 : comb_delay));
        }
        if (src_ratio == kOne) {                                       /* the phase is 1: the new samples */
            out[k] += out_sample_[0];
            aux[k] += aux_sample_[0];
        } else {
            int64_t ph = (int64_t)(src_phase_ >> 16);                  /* Q16 */
            out[k] += (int32_t)(out_sample_[1] + mulr((int64_t)out_sample_[0] - out_sample_[1], ph, 16));
            aux[k] += (int32_t)(aux_sample_[1] + mulr((int64_t)aux_sample_[0] - aux_sample_[1], ph, 16));
        }
    }
    delay_ = delay_v;
    clamped_position_ = pos_v;
    previous_dispersion_ = disp_v;
    previous_damping_compensation_ = comp_v;
}

/* ---- the plucker */

void RingsPlucker::Init()
{
    for (int32_t i = 0; i < 256; ++i) comb_[i] = 0;
    comb_write_ = 0;
    remaining_ = 0;
    comb_period_ = 0;
    comb_gain_ = 0;
    level_ = 32768;
    g_ = h_ = 0;
    s1_ = s2_ = 0;
}

void RingsPlucker::Trigger(uint32_t f, uint32_t cutoff, uint32_t position, int32_t level)
{
    uint64_t ratio = ((uint64_t)position * 58982 >> 16) + 3277;        /* position * .9 + .05, Q16 */
    uint64_t period = udiv64((uint64_t)1 << 48, f ? f : 1u);           /* 1 / f, Q16 */
    uint64_t comb = (period * ratio) >> 16;
    remaining_ = (uint32_t)(comb >> 16);
    while (comb >= (255u << 16)) comb >>= 1;
    comb_period_ = (uint32_t)comb;
    comb_gain_ = (int32_t)((((int64_t)65536 - position) * 858993459) >> 16);   /* (1 - p) * .8, Q30 */
    level_ = level;
    uint32_t c = cutoff > 2143188681u ? 2143188681u : cutoff;
    g_ = tan_dirty_q24(c);
    int64_t den = kQ24 + (int64_t)g_ + (((int64_t)g_ * g_) >> 24);    /* Q 1: r = 1 */
    h_ = (int32_t)udiv64((uint64_t)1 << 48, (uint32_t)den);
}

void RingsPlucker::Process(int32_t* out, int32_t n)
{
    for (int32_t k = 0; k < n; ++k) {
        int64_t in = 0;
        if (remaining_) { in = rng_noise(); --remaining_; }
        uint32_t t = comb_write_ + (comb_period_ >> 16);
        int64_t a = comb_[t & 255u], b = comb_[(t + 1) & 255u];
        int64_t rd = a + mrw((int32_t)(b - a), (int32_t)(comb_period_ & 0xFFFFu), 16);
        int32_t y = (int32_t)(in + mrw(comb_gain_, (int32_t)rd, 30));
        out[k] = y;
        comb_[comb_write_] = y;
        comb_write_ = (comb_write_ - 1u) & 255u;
    }
    /* the low-pass over the block (stmlib Svf, FREQUENCY_DIRTY, Q 1) */
    for (int32_t k = 0; k < n; ++k) {
        int64_t hp = mulr((int64_t)out[k] - mulr(kQ24 + (int64_t)g_, s1_, 24) - s2_, h_, 24);
        int64_t ghp = mulr(g_, hp, 24);
        int64_t bp = ghp + s1_;
        s1_ = ghp + bp;
        int64_t gbp = mulr(g_, bp, 24);
        int64_t lp = gbp + s2_;
        s2_ = gbp + lp;
        out[k] = level_ == 32768 ? (int32_t)lp : (int32_t)((lp * level_) >> 15);
    }
}

/* ---- the part, string models */

void RingsStrings::Init(int32_t* lines, int32_t* stretch)
{
    lines_ = lines;
    stretch_ = stretch;
    model_ = SYMPATHETIC;
    polyphony_ = 1;
    active_ = 0;
    fine_ = 0;
    dirty_ = true;
    /* Part::Init: every note 0 above the tonic (C0); set_polyphony gives a new voice note_[0] + 0.05 i. The idle
     * voices' notes matter: their strings run slowed down below 21.6 Hz, and with dispersion every step they take
     * draws from the random generator the plucker shares. */
    for (int32_t i = 0; i < kMaxPolyphony; ++i) { note_[i] = 12 << 16; dc_x_[i] = dc_y_[i] = 0; }
    strum_pending_ = false;
    peak_ = kQ24 / 2;
    pos_ = kRingsBlock;
    silent_ = 1u << 30;
    g_rng = 0x21;
}

void RingsStrings::set_model(int32_t model)
{
    model = model == STRING ? STRING : SYMPATHETIC;
    if (model != model_) { model_ = model; dirty_ = true; }
}

void RingsStrings::set_polyphony(int32_t p)
{
    p = p >= 4 ? 4 : p >= 2 ? 2 : 1;
    if (p == polyphony_) return;
    for (int32_t i = polyphony_; i < p; ++i) note_[i] = note_[0] + i * 3277;   /* Part::set_polyphony: + 0.05 i */
    polyphony_ = p;
    dirty_ = true;
}

/* Part::ConfigureResonators for the string models */
void RingsStrings::Configure()
{
    /* The strings' LFOs (CosineOscillator<APPROXIMATE>, f = 24 / fs * {.5 .4 .35 .23 .211 .2 .171 0}, stepped once a
     * block): their coefficient is 2 - 32 f^2, within 1e-6 of 2, and in Rings' float its rounding (2^-22 at 2) is a
     * tenth of the distance: Rings' LFO rates are set by float rounding. So these are Rings' own float values (Q29,
     * exact), computed for 44.1 kHz: dsp/ref check that the strings' positions follow Rings' (rings_matrix.py). */
    static const int32_t kLfoCQ29[kNumStrings] = {1073740480, 1073740928, 1073741248, 1073741568,
                                                   1073741568, 1073741632, 1073741632, 1073741824};
    for (int32_t i = 0; i < kNumStrings; ++i) {
        string_[i].set_lines(lines_ + i * kStringDelay, i < kMaxPolyphony ? stretch_ + i * (kStringDelay / 2) : 0);
        string_[i].Init(model_ == STRING);
        lfo_c_[i] = kLfoCQ29[i];
        lfo_y1_[i] = (int64_t)kLfoCQ29[i] << 17;                       /* Start(): c / 4 (Q29 -> Q48, / 4), 0.5 */
        lfo_y0_[i] = (int64_t)1 << 47;
    }
    for (int32_t i = 0; i < kMaxPolyphony; ++i) plucker_[i].Init();
    if (active_ >= polyphony_) active_ = 0;
    dirty_ = false;
}

void RingsStrings::Strike(uint32_t note, int32_t fine, uint32_t velocity)
{
    fine_ = fine;
    strum_pending_ = true;
    strum_note_ = (uint8_t)(note > 127u ? 127u : note);
    strum_vel_ = (uint8_t)(velocity > 127u ? 127u : velocity ? velocity : 1u);
}

/* Part::ComputeSympatheticStringsNotes (tonic = 12, C0, as the module's). In: the note Q16, STRUCTURE Q16; out:
 * semitones Q24. Worked in Q32: SemitonesToRatio truncates to 1/256 semitone, and a few 1e-6 off can land a string on
 * the other side of a step (0.4 cent) from Rings'. */
void RingsStrings::ComputeSympatheticNotes(int32_t note, uint32_t parameter, int32_t* dest, int32_t n) const
{
    const int64_t one = (int64_t)1 << 32;
    const int64_t nt = (int64_t)note << 16;                            /* Q32 */
    const int64_t fifth = 30148737683LL;                               /* 7.01955 semitones, Q32 */
    const int64_t notes[9] = {12 * one, nt - 12 * one, nt - fifth, nt, nt + fifth, nt + 12 * one,
                              nt + 12 * one + fifth, nt + 24 * one, nt + 24 * one};
    const int64_t detunings[4] = {55834575, 47244640, 30064771, 73014444};   /* .013 .011 .007 .017, Q32 */
    int32_t num_detuned = (n - 1) >> 1, first_detuned = n - num_detuned;
    int64_t param = (int64_t)parameter << 16;                          /* Q32 */
    int64_t out[kNumStrings];
    for (int32_t i = 0; i < first_detuned; ++i) {
        int64_t x = (int64_t)3 << 32;
        if (i != 0) {
            x = param * 7;
            param += ((one - param) * 858993459) >> 32;               /* += (1 - p) * .2 (Q32 * .2 Q32) */
        }
        int64_t integral = x >> 32;
        if (integral > 7) {
            out[i] = notes[8];
        } else {
            bool top;
            uint32_t m = squash_q31((uint32_t)((x & 0xFFFFFFFF) >> 1), &top);   /* from the nearer end, Q31 */
            int64_t a = notes[integral], b = notes[integral + 1], hi = (b - a) >> 16;   /* Q16 */
            int64_t d = (hi * m) >> 15;                                /* Q32 */
            out[i] = top ? b - d : a + d;
        }
        if (i + first_detuned < n) out[i + first_detuned] = out[i] + detunings[i & 3];
    }
    for (int32_t i = 0; i < n; ++i) dest[i] = (int32_t)(out[i] >> 8);   /* Q24 */
}

void RingsStrings::RenderBlock(const RingsPatch& patch)
{
    if (dirty_) Configure();
    bool strum = strum_pending_;
    uint32_t strum_vel = strum_vel_;
    strum_pending_ = false;
    if (strum) {                                                       /* Part::Process on a strum */
        if (polyphony_ > 1) active_ = (active_ + 1) % polyphony_;
        note_[active_] = (int32_t)strum_note_ << 16;
        silent_ = 0;
    }
    if (silent_ > 88200u) {                                            /* two seconds under -96 dB: asleep */
        for (int32_t k = 0; k < kRingsBlock * 2; ++k) out_[k] = 0;
        silent_ += kRingsBlock;
        return;
    }
    const int32_t n = kRingsBlock;
    int32_t out[kRingsBlock], aux[kRingsBlock];
    for (int32_t k = 0; k < n; ++k) out[k] = aux[k] = 0;
    uint32_t b = patch.brightness, s = patch.structure, dmp = patch.damping, pos = patch.position;
    uint32_t cut = (uint32_t)(((uint64_t)b * (131072u - b)) >> 16);   /* b (2 - b) */
    int32_t num_strings = model_ == SYMPATHETIC ? 2 * kMaxPolyphony / polyphony_ : 1;
    /* dispersion from STRUCTURE: (s - .24) * 4.166 under .24, (s - .26) * 1.35135 over .26 */
    int32_t dispersion = 0;
    if (s < 15729u) dispersion = (int32_t)(((int64_t)s - 15729) * 273023 >> 16);
    else if (s > 17039u) dispersion = (int32_t)(((int64_t)s - 17039) * 88562 >> 16);

    for (int32_t v = 0; v < polyphony_; ++v) {
        int32_t note_q16 = note_[v] + (fine_ << 9);                    /* fine: 1/128 semitone */
        uint32_t frequency = note_freq_q16(note_q16);
        uint32_t fc;
        if (v == active_) {
            uint64_t f = ((uint64_t)frequency * (st_ratio_q30((((int64_t)cut - 32768) * 96) << 8) >> 10)) >> 20;
            fc = f > 2143188681u ? 2143188681u : (uint32_t)f;
        } else {
            fc = 973915u;                                              /* 10 Hz */
        }
#ifdef FOM_RINGS_DEBUG
        { static int dbgv = 0; if (dbgv++ < 12) fprintf(stderr, "V voice %d active %d strum %d freq %.7f cutoff %.6f note %d\n", (int)v, (int)active_, (int)strum, frequency / 4294967296.0, fc / 4294967296.0, note_[v] >> 16); }
#endif
        int32_t freqs[kNumStrings];
        if (model_ == SYMPATHETIC) {
            ComputeSympatheticNotes(note_q16, s, freqs, num_strings);
            for (int32_t i = 0; i < num_strings; ++i) freqs[i] = (int32_t)note_freq_q24(freqs[i]);
        } else {
            freqs[0] = (int32_t)frequency;
        }
        /* the input: the plucker's burst (the active voice's, on a strum), through the input's DC blocker */
        int32_t input[kRingsBlock];
        if (strum && v == active_) {
            uint64_t c8 = (uint64_t)fc * 8u;
            int32_t level = strum_vel == 127u ? 32768 : (int32_t)strum_vel * 258;
            plucker_[v].Trigger(frequency, c8 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)c8, pos, level);
        }
        plucker_[v].Process(input, n);
        for (int32_t k = 0; k < n; ++k) {
            int32_t x = input[k];
            dc_y_[v] = dc_y_[v] - mulr(dc_y_[v], kDcPartLeak, 30) + ((int64_t)(x - dc_x_[v]) << 16);
            dc_x_[v] = x;
            input[k] = (int32_t)((dc_y_[v] + 32768) >> 16);
        }
        int32_t vo[kRingsBlock], va[kRingsBlock], sym[kRingsBlock];
        for (int32_t k = 0; k < n; ++k) vo[k] = va[k] = sym[k] = 0;
        for (int32_t st = 0; st < num_strings; ++st) {
            int32_t i = v + st * polyphony_;
            RingsString& str = string_[i];
            /* lfo_[i].Next(), 0..1 Q30 */
            /* Next(): y0 = c y0 - y1 in float32, each result rounded as Rings' float rounds it: over thousands of
             * blocks an exact recursion drifts from Rings' and moves the strings' pickups (0.1 sample at C5).
             * c y0 = 2 y0 - (2 - c) y0, so the product fits 64 bits. */
            int64_t t = lfo_y0_[i];                                    /* Q48 */
            int64_t delta = ((int64_t)1 << 30) - lfo_c_[i];            /* 2 - c, Q29 */
            int64_t prod = float_grid_signed(2 * t - ((delta * t) >> 29));
            lfo_y0_[i] = float_grid_signed(prod - lfo_y1_[i]);
            lfo_y1_[i] = t;
            int64_t lfo = float_grid_signed(t + ((int64_t)1 << 47)) >> 18;   /* temp + .5, Q30 */
            uint32_t sb = b, sd = dmp, sp = pos;
            int32_t glide = 1 << 30;
            const int32_t* in = input;
            if (st > 0) {                                              /* the sympathetic strings */
                sb = (uint32_t)(((uint64_t)sb * (131072u - sb)) >> 16);
                sb = (uint32_t)(((uint64_t)sb * (131072u - sb)) >> 16);
                sd = 45875u + (uint32_t)(((uint64_t)dmp * 17695u) >> 16);   /* .7 + d * .27 */
                int64_t pc = (int64_t)pos - 32768;
                int64_t amount = ((32768 - (pc < 0 ? -pc : pc)) * 58982) >> 16;   /* (.5 - |.5 - p|) * .9 */
                sp = (uint32_t)((int64_t)pos + ((((lfo >> 14) * amount)) >> 16));
                glide = (int32_t)st_ratio_q30((((int64_t)sb - 65536) * 36) << 8);   /* SemitonesToRatio((b - 1) 36) */
                in = sym;
            }
            int64_t string_index = (int32_t)(st << 16) / num_strings;  /* Q16 */
            uint32_t damping = sd + (uint32_t)((string_index * (int64_t)(62259 - (int64_t)sd)) >> 16);   /* .95 */
            str.set_params(dispersion, sb, damping, sp);
            str.set_frequency((uint32_t)freqs[st], glide);
            str.Process(in, vo, va, n);
            if (st == 0) {                                             /* the sympathetic input: .2 / n (out - aux) */
                int64_t g = (int32_t)13107 / num_strings;              /* .2 / n, Q16 */
                for (int32_t k = 0; k < n; ++k) sym[k] = (int32_t)((((int64_t)vo[k] - va[k]) * g) >> 16);
            }
        }
        if (polyphony_ == 1) {
            for (int32_t k = 0; k < n; ++k) { out[k] += vo[k]; aux[k] += va[k]; }
        } else {
            int32_t* d = (v & 1) ? aux : out;
            for (int32_t k = 0; k < n; ++k) d[k] += vo[k] - va[k];
        }
    }
    /* Limiter: Rings' model gains, sympathetic 1.0, string 1.4 */
    const int32_t gain = model_ == STRING ? 23488102 : kQ24;
    int32_t peak_out = 0;
    for (int32_t k = 0; k < n; ++k) {
        int32_t l = mulq(out[k], gain, 24), r = mulq(aux[k], gain, 24);
        int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r, dd = r - l, ad = dd < 0 ? -dd : dd;
        int32_t pk = al > ar ? al : ar;
        if (ad > pk) pk = ad;
        int32_t err = pk - peak_;
#ifdef FOM_RINGS_TEST_44K
        peak_ += err > 0 ? mulq(err, 3355443, 26) : mulq(err, 1342, 26);
#else
        peak_ += err > 0 ? mulq(err, 3643983, 26) : mulq(err, 1461, 26);
#endif
        int32_t gn = peak_ <= kQ24 ? kQ24 : (int32_t)(((1u << 31) / ((uint32_t)peak_ >> 9)) << 8);
        int32_t gl = mulq(mulq(l, gn, 24), 13421773, 24), gr = mulq(mulq(r, gn, 24), 13421773, 24);
        int32_t yl = soft_limit(gl) >> 9, yr = soft_limit(gr) >> 9;
        out_[2 * k] = yl;
        out_[2 * k + 1] = yr;
        int32_t a = yl < 0 ? -yl : yl, c = yr < 0 ? -yr : yr;
        if (a > peak_out) peak_out = a;
        if (c > peak_out) peak_out = c;
    }
    if (peak_out > 0) silent_ = 0; else silent_ += (uint32_t)n;
}

void RingsStrings::Render(const RingsPatch& patch, int32_t level, int32_t* out, size_t n)
{
    size_t k = 0;
    while (k < n) {
        if (pos_ >= kRingsBlock) { RenderBlock(patch); pos_ = 0; }
        while (k < n && pos_ < kRingsBlock) {
            out[2 * k] += mulq(out_[2 * pos_], level, 15);
            out[2 * k + 1] += mulq(out_[2 * pos_ + 1], level, 15);
            ++k; ++pos_;
        }
    }
}

}  // namespace fom
