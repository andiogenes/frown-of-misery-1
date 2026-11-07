#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare a fixed-point render with its float reference.

Both WAVs are brought to one sample rate (the lower of the two; band-limited FFT resampling), mixed to mono and
aligned (the lag of the best cross-correlation within +-MAXLAG ms). Then three measures:

  gain   the RMS ratio reference / test: the level difference (lsd and env are measured after evening it out)
  snr    the reference's energy against what is left after subtracting the test at its best gain, dB. Meaningful
         when the port should match sample for sample (a resonator, a filter): a straight port reaches 60-90 dB.
  lsd    log-spectral distance of the average spectra in third-octave bands, dB: the tone colour. For a granular
         or randomised process (Clouds), where no two renders match sample for sample, this and env are the test.
  env    the mean absolute difference of the two RMS envelopes in 10 ms frames, dB (frames above -60 dBFS): decay
         times and dynamics.

  compare.py REF.wav TEST.wav [--min-snr DB] [--max-lsd DB] [--max-env DB] [--expect-differ] [--maxlag MS]
  compare.py --selftest

Exit status 1 when a limit given is not met. Needs numpy."""
import argparse
import sys
import wave

import numpy as np


def read(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: not 16-bit PCM")
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0
        ch, rate = w.getnchannels(), w.getframerate()
    return x.reshape(-1, ch).mean(axis=1), rate


def resample(x, src, dst):
    """Band-limited resampling of the whole signal (FFT, zero-padded / truncated spectrum)."""
    if src == dst or len(x) == 0:
        return x
    n = len(x)
    m = int(round(n * dst / src))
    X = np.fft.rfft(x)
    Y = np.zeros(m // 2 + 1, dtype=complex)
    k = min(len(X), len(Y))
    Y[:k] = X[:k]
    return np.fft.irfft(Y, m) * (m / n)


def align(ref, test, maxlag):
    """test moved by the lag of the best cross-correlation (|lag| <= maxlag samples; + = test is late), both cut
    to one length."""
    n = 1 << int(np.ceil(np.log2(len(ref) + len(test))))
    c = np.fft.irfft(np.fft.rfft(test, n) * np.conj(np.fft.rfft(ref, n)), n)
    lags = np.concatenate([np.arange(0, maxlag + 1), np.arange(-maxlag, 0)])
    lag = int(lags[np.argmax(np.concatenate([c[:maxlag + 1], c[-maxlag:]]))])
    if lag > 0:
        test = test[lag:]
    elif lag < 0:
        test = np.concatenate([np.zeros(-lag), test])
    m = min(len(ref), len(test))
    return ref[:m], test[:m], lag


def snr(ref, test):
    g = np.dot(ref, test) / (np.dot(test, test) or 1.0)
    err = ref - g * test
    return 10 * np.log10((np.dot(ref, ref) + 1e-20) / (np.dot(err, err) + 1e-20)), g


def band_spectrum(x, rate):
    """Average power in third-octave bands from 31.5 Hz to 16 kHz (Welch, 4096-point Hann frames), dB."""
    nfft = 4096
    if len(x) < nfft:
        x = np.concatenate([x, np.zeros(nfft - len(x))])
    win = np.hanning(nfft)
    frames = [x[i:i + nfft] * win for i in range(0, len(x) - nfft + 1, nfft // 2)]
    p = np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], axis=0)
    f = np.fft.rfftfreq(nfft, 1.0 / rate)
    centres = 1000.0 * 2.0 ** (np.arange(-15, 13) / 3.0)
    out = []
    for c in centres[centres < rate / 2 * 0.9]:
        sel = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
        out.append(10 * np.log10(p[sel].mean() + 1e-20) if sel.any() else -200.0)
    return np.array(out)


def lsd(ref, test, rate):
    a, b = band_spectrum(ref, rate), band_spectrum(test, rate)
    keep = a > a.max() - 60                     # the bands that matter: within 60 dB of the loudest
    return float(np.sqrt(np.mean((a[keep] - b[keep]) ** 2)))


def envelope(x, rate):
    hop = rate // 100
    n = len(x) // hop
    return 10 * np.log10(np.mean(x[:n * hop].reshape(n, hop) ** 2, axis=1) + 1e-20)


def env_error(ref, test, rate):
    a, b = envelope(ref, rate), envelope(test, rate)
    keep = a > -60
    return float(np.mean(np.abs(a[keep] - b[keep]))) if keep.any() else 0.0


def measure(ref, rr, test, tr, maxlag_ms=50.0):
    rate = min(rr, tr)
    ref, test = resample(ref, rr, rate), resample(test, tr, rate)
    ref, test, lag = align(ref, test, int(maxlag_ms * rate / 1000))
    s, _ = snr(ref, test)
    # the colour and the envelope at the same loudness: a port may be quieter or louder on purpose, the level is
    # "gain" (the RMS ratio; the least-squares gain of snr is ~0 for signals that do not correlate)
    g = float(np.sqrt(np.dot(ref, ref) / (np.dot(test, test) or 1e-20)))
    return {"rate": rate, "lag": lag, "gain": g, "snr": s, "lsd": lsd(ref, g * test, rate),
            "env": env_error(ref, g * test, rate)}


def selftest():
    rate = 48000
    t = np.arange(rate * 2) / rate
    x = np.sin(2 * np.pi * 440 * t) * np.exp(-t)
    same = measure(x, rate, x, rate)
    assert same["snr"] > 100 and same["lsd"] < 0.01 and same["env"] < 0.01, same
    y = resample(x, rate, 44100)                # the same tone at the FM-1's rate, delayed 5 ms, 6 dB down
    y = np.concatenate([np.zeros(220), y * 0.5])
    m = measure(x, rate, y, 44100)
    assert m["snr"] > 40 and abs(m["gain"] - 2.0) < 0.05 and abs(m["lag"] - 220) <= 1, m
    z = np.sin(2 * np.pi * 466.16 * t) * np.exp(-t)     # a semitone off: the waveform differs, the colour barely
    d = measure(x, rate, z, rate)
    assert d["snr"] < 3 and d["env"] < 1, d
    w = np.sin(2 * np.pi * 440 * t) * np.exp(-3 * t)    # a decay three times faster: the envelope says so
    e = measure(x, rate, w, rate)
    assert e["env"] > 10, e
    print("compare.py selftest passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", nargs="?")
    ap.add_argument("test", nargs="?")
    ap.add_argument("--min-snr", type=float)
    ap.add_argument("--max-lsd", type=float)
    ap.add_argument("--max-env", type=float)
    ap.add_argument("--expect-differ", action="store_true", help="fail if the two are (nearly) the same")
    ap.add_argument("--maxlag", type=float, default=50.0, help="the alignment search, +- ms (default 50)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return
    if not a.ref or not a.test:
        ap.error("REF.wav and TEST.wav")
    x, xr = read(a.ref)
    y, yr = read(a.test)
    m = measure(x, xr, y, yr, a.maxlag)
    print(f"{a.test} vs {a.ref}: at {m['rate']} Hz, lag {m['lag']}, gain {m['gain']:.3f}; "
          f"snr {m['snr']:.1f} dB, lsd {m['lsd']:.2f} dB, env {m['env']:.2f} dB")
    bad = []
    if a.min_snr is not None and m["snr"] < a.min_snr:
        bad.append(f"snr {m['snr']:.1f} < {a.min_snr}")
    if a.max_lsd is not None and m["lsd"] > a.max_lsd:
        bad.append(f"lsd {m['lsd']:.2f} > {a.max_lsd}")
    if a.max_env is not None and m["env"] > a.max_env:
        bad.append(f"env {m['env']:.2f} > {a.max_env}")
    if a.expect_differ and m["snr"] > 20 and m["lsd"] < 1:
        bad.append("expected to differ, but they match")
    if bad:
        print("FAILED: " + "; ".join(bad))
        sys.exit(1)


if __name__ == "__main__":
    main()
