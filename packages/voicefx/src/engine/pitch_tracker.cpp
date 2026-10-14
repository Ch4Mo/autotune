#include "pitch_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "dsp.hpp"
#include "fft.hpp"

namespace vfx {

float PitchTrack::f0At(double samplePos) const {
  if (f0.empty()) return 0.0f;
  const double p = (samplePos - offset) / hop;
  if (p <= 0.0) return f0.front();
  const size_t last = f0.size() - 1;
  if (p >= static_cast<double>(last)) return f0.back();
  const size_t i = static_cast<size_t>(p);
  const double t = p - static_cast<double>(i);
  const float a = f0[i], b = f0[i + 1];
  if (a > 0.0f && b > 0.0f) {
    // Interpolate in the log domain (perceptually linear).
    return static_cast<float>(std::exp((1.0 - t) * std::log(a) + t * std::log(b)));
  }
  return t < 0.5 ? a : b;
}

namespace {

size_t nextPow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Anti-aliased integer decimation (4th order Butterworth low-pass, then pick).
std::vector<float> decimate(const std::vector<float>& x, double sr, int factor) {
  if (factor <= 1) return x;
  Biquad lp1, lp2, hp;
  const double cutoff = 0.42 * sr / factor;
  lp1.setup(Biquad::Type::LowPass, sr, cutoff, 0.5411961);
  lp2.setup(Biquad::Type::LowPass, sr, cutoff, 1.3065630);
  hp.setup(Biquad::Type::HighPass, sr, 50.0, 0.7071);
  std::vector<float> y;
  y.reserve(x.size() / static_cast<size_t>(factor) + 1);
  for (size_t i = 0; i < x.size(); ++i) {
    const float v = lp2.process(lp1.process(hp.process(x[i])));
    if (i % static_cast<size_t>(factor) == 0) y.push_back(v);
  }
  return y;
}

float median(std::vector<float> v) {
  if (v.empty()) return 0.0f;
  const size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(m), v.end());
  return v[m];
}

}  // namespace

PitchTrack trackPitch(const std::vector<float>& mono, double sampleRate, const PitchTrackerConfig& cfg) {
  PitchTrack track;
  track.sampleRate = sampleRate;

  const int factor = std::max(1, static_cast<int>(std::floor(sampleRate / 16000.0 + 0.25)));
  const double dsr = sampleRate / factor;
  const std::vector<float> x = decimate(mono, sampleRate, factor);

  const size_t hop = std::max<size_t>(1, static_cast<size_t>(std::lround(cfg.hopSeconds * dsr)));
  const size_t W = static_cast<size_t>(std::lround(cfg.windowSeconds * dsr));
  const size_t tauMax = static_cast<size_t>(std::ceil(dsr / cfg.minF0));
  const size_t tauMin = std::max<size_t>(2, static_cast<size_t>(std::floor(dsr / cfg.maxF0)));
  const size_t span = W + tauMax + 2;
  const size_t N = nextPow2(span);

  track.hop = static_cast<double>(hop * static_cast<size_t>(factor));
  track.offset = static_cast<double>(W / 2) * factor;

  if (x.size() < span) {
    // Too short to analyse: a single unvoiced frame.
    track.f0.assign(1, 0.0f);
    track.confidence.assign(1, 0.0f);
    track.rmsDb.assign(1, -120.0f);
    return track;
  }

  // Prefix sums of squared samples for the YIN energy terms.
  std::vector<double> csum(x.size() + 1, 0.0);
  for (size_t i = 0; i < x.size(); ++i) csum[i + 1] = csum[i] + static_cast<double>(x[i]) * x[i];

  const size_t nFrames = (x.size() - span) / hop + 1;
  track.f0.assign(nFrames, 0.0f);
  track.confidence.assign(nFrames, 0.0f);
  track.rmsDb.assign(nFrames, -120.0f);

  FFT fft(N);
  std::vector<std::complex<float>> A(N), B(N);
  std::vector<float> d(tauMax + 2), dn(tauMax + 2);

  for (size_t f = 0; f < nFrames; ++f) {
    const size_t s = f * hop;
    const double e0 = csum[s + W] - csum[s];
    track.rmsDb[f] = static_cast<float>(10.0 * std::log10(std::max(e0 / W, 1e-12)));

    std::fill(A.begin(), A.end(), std::complex<float>(0.0f, 0.0f));
    std::fill(B.begin(), B.end(), std::complex<float>(0.0f, 0.0f));
    for (size_t j = 0; j < W; ++j) A[j] = x[s + j];
    for (size_t j = 0; j < W + tauMax + 1; ++j) B[j] = x[s + j];
    fft.forward(A);
    fft.forward(B);
    for (size_t k = 0; k < N; ++k) A[k] = std::conj(A[k]) * B[k];
    fft.inverse(A);

    // Difference function and cumulative-mean-normalised difference.
    d[0] = 0.0f;
    dn[0] = 1.0f;
    double running = 0.0;
    for (size_t tau = 1; tau <= tauMax + 1; ++tau) {
      const double et = csum[s + tau + W] - csum[s + tau];
      const double v = std::max(0.0, e0 + et - 2.0 * A[tau].real());
      d[tau] = static_cast<float>(v);
      running += v;
      dn[tau] = running > 0.0 ? static_cast<float>(v * tau / running) : 1.0f;
    }

    // Absolute threshold, then descend to the local minimum.
    size_t best = 0;
    for (size_t tau = tauMin; tau <= tauMax; ++tau) {
      if (dn[tau] < cfg.yinThreshold) {
        while (tau + 1 <= tauMax && dn[tau + 1] < dn[tau]) ++tau;
        best = tau;
        break;
      }
    }
    if (best == 0) {
      best = tauMin;
      for (size_t tau = tauMin; tau <= tauMax; ++tau)
        if (dn[tau] < dn[best]) best = tau;
    }
    const float aperiodicity = dn[best];

    // Parabolic interpolation around the minimum of the normalised difference.
    double refined = static_cast<double>(best);
    if (best > 1 && best < tauMax + 1) {
      const double a = dn[best - 1], b = dn[best], c = dn[best + 1];
      const double den = a - 2.0 * b + c;
      if (std::fabs(den) > 1e-12) refined += 0.5 * (a - c) / den;
    }

    track.confidence[f] = std::clamp(1.0f - aperiodicity, 0.0f, 1.0f);
    if (aperiodicity < cfg.voicingThreshold && refined > 0.0) {
      track.f0[f] = static_cast<float>(dsr / refined);
    }
  }

  // --- Post-processing -------------------------------------------------------
  const float loudest = *std::max_element(track.rmsDb.begin(), track.rmsDb.end());
  const float gate = static_cast<float>(std::max(cfg.silenceDb, loudest + cfg.relativeSilenceDb));
  for (size_t f = 0; f < nFrames; ++f)
    if (track.rmsDb[f] < gate) track.f0[f] = 0.0f;

  // Octave-error correction against a local median (in semitones).
  {
    std::vector<float> fixed = track.f0;
    const long R = 12;  // +-60 ms
    std::vector<float> win;
    for (long f = 0; f < static_cast<long>(nFrames); ++f) {
      if (track.f0[f] <= 0.0f) continue;
      win.clear();
      for (long k = std::max(0L, f - R); k <= std::min<long>(nFrames - 1, f + R); ++k)
        if (track.f0[k] > 0.0f) win.push_back(static_cast<float>(hzToMidi(track.f0[k])));
      if (win.size() < 5) continue;
      const float med = median(win);
      float m = static_cast<float>(hzToMidi(track.f0[f]));
      while (m - med > 8.5f) m -= 12.0f;
      while (med - m > 8.5f) m += 12.0f;
      fixed[f] = static_cast<float>(midiToHz(m));
    }
    track.f0.swap(fixed);
  }

  // Remove very short voiced islands (< 25 ms) and bridge short gaps (< 20 ms).
  {
    const size_t minIsland = std::max<size_t>(2, static_cast<size_t>(0.025 / cfg.hopSeconds));
    const size_t maxGap = std::max<size_t>(1, static_cast<size_t>(0.020 / cfg.hopSeconds));
    size_t f = 0;
    while (f < nFrames) {
      if (track.f0[f] > 0.0f) {
        size_t e = f;
        while (e < nFrames && track.f0[e] > 0.0f) ++e;
        if (e - f < minIsland) std::fill(track.f0.begin() + static_cast<long>(f), track.f0.begin() + static_cast<long>(e), 0.0f);
        f = e;
      } else {
        ++f;
      }
    }
    f = 0;
    while (f < nFrames) {
      if (track.f0[f] <= 0.0f) {
        size_t e = f;
        while (e < nFrames && track.f0[e] <= 0.0f) ++e;
        if (f > 0 && e < nFrames && e - f <= maxGap) {
          const double a = std::log(track.f0[f - 1]), b = std::log(track.f0[e]);
          for (size_t k = f; k < e; ++k) {
            const double t = static_cast<double>(k - f + 1) / static_cast<double>(e - f + 1);
            track.f0[k] = static_cast<float>(std::exp(a + (b - a) * t));
          }
        }
        f = e;
      } else {
        ++f;
      }
    }
  }

  // Light 3-point median smoothing on voiced frames (removes jitter, keeps vibrato).
  {
    std::vector<float> sm = track.f0;
    for (size_t f = 1; f + 1 < nFrames; ++f) {
      if (track.f0[f - 1] > 0 && track.f0[f] > 0 && track.f0[f + 1] > 0)
        sm[f] = median({track.f0[f - 1], track.f0[f], track.f0[f + 1]});
    }
    track.f0.swap(sm);
  }

  return track;
}

}  // namespace vfx
