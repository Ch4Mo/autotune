#include "denoise.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "dsp.hpp"
#include "fft.hpp"

namespace vfx {

namespace {

size_t nextPow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

struct Stft {
  size_t N, hop, bins;
  std::vector<float> window;  // sqrt of a periodic Hann: analysis = synthesis window
  FFT fft;

  explicit Stft(double sr)
      : N(nextPow2(static_cast<size_t>(0.04 * sr))), hop(N / 4), bins(N / 2 + 1), window(N), fft(N) {
    for (size_t i = 0; i < N; ++i) window[i] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * i / N)));
  }

  size_t frames(size_t len) const { return (len + N) / hop + 1; }

  // Frame f covers samples [f*hop - N, f*hop) of the signal (zero outside).
  void analyse(const std::vector<float>& x, size_t f, std::vector<std::complex<float>>& buf) const {
    const long start = static_cast<long>(f * hop) - static_cast<long>(N);
    for (size_t i = 0; i < N; ++i) {
      const long k = start + static_cast<long>(i);
      const float v = (k >= 0 && k < static_cast<long>(x.size())) ? x[static_cast<size_t>(k)] : 0.0f;
      buf[i] = v * window[i];
    }
    fft.forward(buf);
  }
};

// Mean power spectrum of the quietest (but not digitally silent) frames.
std::vector<double> noiseSpectrum(const std::vector<float>& x, const Stft& st, double voiceLevelDb, double& floorDb) {
  const size_t F = st.frames(x.size());
  std::vector<double> energy(F, 0.0);
  for (size_t f = 0; f < F; ++f) {
    const long start = static_cast<long>(f * st.hop) - static_cast<long>(st.N);
    double e = 0;
    for (size_t i = 0; i < st.N; ++i) {
      const long k = start + static_cast<long>(i);
      if (k >= 0 && k < static_cast<long>(x.size())) e += static_cast<double>(x[static_cast<size_t>(k)]) * x[static_cast<size_t>(k)];
    }
    energy[f] = e;
  }
  std::vector<double> live;
  for (double e : energy) if (e > 1e-9 * st.N) live.push_back(e);
  std::vector<double> psd(st.bins, 0.0);
  floorDb = -120.0f;
  if (live.empty()) return psd;
  std::sort(live.begin(), live.end());
  // Quietest 15 % of the take, but never closer than 20 dB to the voice
  // (a take sung without pauses has no "silence" to learn from). Keep at
  // least a handful of frames: the quietest moments, e.g. breaths.
  const double voiceCap = st.N * std::pow(10.0, (voiceLevelDb - 20.0) / 10.0);
  double thr = std::min(live[std::min(live.size() - 1, live.size() * 15 / 100)], voiceCap);
  const bool reliable = std::count_if(live.begin(), live.end(), [&](double e) { return e <= voiceCap; }) >= 5;
  thr = std::max(thr, live[std::min(live.size() - 1, size_t{4})]);

  std::vector<std::complex<float>> buf(st.N);
  size_t used = 0;
  double sumE = 0;
  for (size_t f = 0; f < F; ++f) {
    if (energy[f] <= 1e-9 * st.N || energy[f] > thr) continue;
    st.analyse(x, f, buf);
    for (size_t k = 0; k < st.bins; ++k) psd[k] += std::norm(buf[k]);
    sumE += energy[f];
    ++used;
  }
  if (used == 0) return psd;
  double meanE = sumE / static_cast<double>(used);
  double scale = 1.0 / static_cast<double>(used);
  if (!reliable && meanE > voiceCap) {
    // No real pause: these frames hold soft singing. Cap the profile so the
    // voice stays well above the "noise" and is not eaten by the filter.
    scale *= voiceCap / meanE;
    meanE = voiceCap;
  }
  for (auto& v : psd) v *= scale;
  // Unknown noise level when there is no pause to measure it in.
  floorDb = reliable ? 10.0 * std::log10(std::max(meanE / static_cast<double>(st.N), 1e-12)) : -120.0;
  return psd;
}

// Finds mains hum (50 or 60 Hz family) as narrow peaks in the spectrum of
// the quiet parts, analysed with a long FFT (~3 Hz resolution).
float detectHum(const std::vector<float>& x, double sr) {
  const size_t N = nextPow2(static_cast<size_t>(0.33 * sr)), hop = N / 2;
  if (x.size() < N) return 0.0f;
  std::vector<std::pair<double, size_t>> blocks;
  for (size_t s = 0; s + N <= x.size(); s += hop) {
    double e = 0;
    for (size_t i = s; i < s + N; ++i) e += static_cast<double>(x[i]) * x[i];
    if (e > 1e-9 * N) blocks.emplace_back(e, s);
  }
  if (blocks.empty()) return 0.0f;
  std::sort(blocks.begin(), blocks.end());
  const size_t use = std::max<size_t>(1, std::min<size_t>(blocks.size() * 3 / 10, 20));

  FFT fft(N);
  std::vector<std::complex<float>> buf(N);
  std::vector<double> psd(N / 2 + 1, 0.0);
  for (size_t b = 0; b < use; ++b) {
    const size_t s = blocks[b].second;
    for (size_t i = 0; i < N; ++i) buf[i] = x[s + i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / N));
    fft.forward(buf);
    for (size_t k = 0; k < psd.size(); ++k) psd[k] += std::norm(buf[k]);
  }
  const double binHz = sr / N;
  // A sung note can line up with some multiples of 50/60 Hz, but its
  // harmonics are all multiples of its own f0 (> 80 Hz): it can never light
  // up the fundamental, nor two harmonics with coprime ranks (2 and 3).
  auto prominence = [&](double base) {
    double score = 0;
    bool present[9] = {false};
    for (int h = 1; h <= 8; ++h) {
      const size_t k = static_cast<size_t>(std::lround(base * h / binHz));
      if (k < 8 || k + 8 >= psd.size()) continue;
      const double peak = std::max({psd[k - 1], psd[k], psd[k + 1]});
      double side = 0;
      for (int d = 4; d <= 7; ++d) side += psd[k - d] + psd[k + d];
      side /= 8.0;
      const double db = 10.0 * std::log10((peak + 1e-20) / (side + 1e-20));
      if (db > 10.0) { score += db; present[h] = true; }
    }
    const bool hum = present[1] ? (present[2] || present[3]) : (present[2] && present[3]);
    return hum ? score : 0.0;
  };
  const double s50 = prominence(50.0), s60 = prominence(60.0);
  if (s50 <= 0 && s60 <= 0) return 0.0f;
  return s50 >= s60 ? 50.0f : 60.0f;
}

}  // namespace

DenoiseReport analyseNoise(const std::vector<float>& x, double sr, float voiceLevelDb) {
  DenoiseReport report;
  if (x.empty()) return report;
  const Stft st(sr);
  double floorDb = -120.0;
  const std::vector<double> noise = noiseSpectrum(x, st, voiceLevelDb, floorDb);
  report.noiseFloorDb = static_cast<float>(floorDb);
  report.humHz = detectHum(x, sr);
  return report;
}

DenoiseReport denoise(std::vector<float>& x, double sr, float strength, float voiceLevelDb) {
  DenoiseReport report;
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (x.empty() || strength <= 0.0f) return report;

  const Stft st(sr);
  double floorDb = -120.0;
  const std::vector<double> noise = noiseSpectrum(x, st, voiceLevelDb, floorDb);
  report.noiseFloorDb = static_cast<float>(floorDb);

  // Mains hum is stationary, so it is part of the learned noise profile and
  // removed by the spectral stage. (Dedicated notch filters were tried: they
  // also cut voice harmonics passing over 50 Hz multiples, a net loss.)
  report.humHz = detectHum(x, sr);

  // Smooth the noise profile slightly across frequency.
  std::vector<double> nsm(noise.size());
  for (size_t k = 0; k < noise.size(); ++k) {
    double s = 0; int c = 0;
    for (size_t j = (k > 2 ? k - 2 : 0); j <= std::min(noise.size() - 1, k + 2); ++j) { s += noise[j]; ++c; }
    nsm[k] = s / c;
  }

  const double over = 1.0 + 1.5 * strength;                         // noise over-estimation
  const float gainFloor = dbToGain(-(6.0f + 20.0f * strength));     // max reduction
  const double alpha = 0.98;                                        // decision-directed smoothing
  const size_t F = st.frames(x.size());
  std::vector<float> out(x.size() + st.N, 0.0f);
  std::vector<std::complex<float>> buf(st.N);
  std::vector<double> prevClean(st.bins, 0.0);  // |S|^2 estimate of previous frame
  std::vector<float> gain(st.bins), gsm(st.bins);

  for (size_t f = 0; f < F; ++f) {
    st.analyse(x, f, buf);
    for (size_t k = 0; k < st.bins; ++k) {
      const double n = std::max(nsm[k] * over, 1e-20);
      const double p = std::norm(buf[k]);
      const double post = p / n;  // a-posteriori SNR
      const double prio = alpha * prevClean[k] / n + (1.0 - alpha) * std::max(post - 1.0, 0.0);
      gain[k] = static_cast<float>(prio / (1.0 + prio));  // Wiener gain
    }
    // Frequency smoothing of the gain curve reduces isolated tonal artefacts.
    for (size_t k = 0; k < st.bins; ++k) {
      const float a = gain[k > 0 ? k - 1 : k], b = gain[k], c = gain[k + 1 < st.bins ? k + 1 : k];
      gsm[k] = std::max(gainFloor, 0.25f * a + 0.5f * b + 0.25f * c);
    }
    for (size_t k = 0; k < st.bins; ++k) {
      prevClean[k] = static_cast<double>(gsm[k]) * gsm[k] * std::norm(buf[k]);
      buf[k] *= gsm[k];
      if (k > 0 && k < st.N / 2) buf[st.N - k] = std::conj(buf[k]);
    }
    st.fft.inverse(buf);
    const long start = static_cast<long>(f * st.hop) - static_cast<long>(st.N);
    for (size_t i = 0; i < st.N; ++i) {
      const long k = start + static_cast<long>(i);
      if (k >= 0 && k < static_cast<long>(x.size())) out[static_cast<size_t>(k)] += buf[i].real() * st.window[i] * 0.5f;
    }
  }
  std::copy(out.begin(), out.begin() + static_cast<long>(x.size()), x.begin());
  return report;
}

}  // namespace vfx
