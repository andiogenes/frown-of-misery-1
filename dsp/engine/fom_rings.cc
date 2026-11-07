/* SPDX-License-Identifier: MIT */
#include "fom_rings.h"

#include "fom_rings_tables.h"

namespace fom {

namespace {

/* ---- integer helpers (no float, no 64-bit division: the chip has neither) */

const int32_t INT32_MIN_ = (int32_t)0x80000000u;

/* n / d for a quotient that fits 32 bits: shift and subtract, constant shifts only (no __udivdi3, no __lshrdi3) */
uint32_t udiv64(uint64_t n, uint32_t d)
{
    uint64_t r = 0;
    uint32_t q = 0;
    int i;
    if (!d) return 0xFFFFFFFFu;
    for (i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> 63) & 1u);
        n <<= 1;
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

}  // namespace fom
