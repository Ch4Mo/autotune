// Synthetic singing voice for tests: glottal pulse train + vowel formants + breath noise.
#pragma once

#include <cmath>
#include <functional>
#include <random>
#include <vector>

#include "engine/dsp.hpp"

namespace vfxtest {

// pitchHz(t) returns the f0 at time t (seconds); 0 = silence.
inline std::vector<float> synthVoice(double sr, double seconds, const std::function<double(double)>& pitchHz,
                                     double noise = 0.002) {
  const size_t n = static_cast<size_t>(sr * seconds);
  std::vector<float> src(n, 0.0f);
  double phase = 0.0;
  std::mt19937 rng(7);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  float amp = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double f = pitchHz(t);
    const float target = f > 0 ? 1.0f : 0.0f;
    amp += (target - amp) * 0.002f;  // ~10 ms attack/release
    if (f > 0) phase += f / sr;
    if (phase >= 1.0) phase -= 1.0;
    // Rosenberg-like glottal flow derivative: smooth opening, sharp closure.
    const double op = 0.6, cl = 0.15;
    double g = 0.0;
    if (phase < op) g = 0.5 * (1.0 - std::cos(vfx::kPi * phase / op));
    else if (phase < op + cl) g = std::cos(vfx::kPi * (phase - op) / (2.0 * cl));
    src[i] = amp * static_cast<float>(g);
  }
  // Differentiate (lip radiation) and add aspiration noise.
  std::vector<float> x(n, 0.0f);
  float prev = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    x[i] = src[i] - prev + static_cast<float>(noise) * gauss(rng);
    prev = src[i];
  }
  // Vowel /a/ formants.
  std::vector<float> y(n, 0.0f);
  const double fr[3] = {730, 1090, 2440}, bw[3] = {90, 110, 160}, gain[3] = {1.0, 0.6, 0.25};
  for (int k = 0; k < 3; ++k) {
    vfx::Biquad b;
    b.setup(vfx::Biquad::Type::BandPass, sr, fr[k], fr[k] / bw[k]);
    for (size_t i = 0; i < n; ++i) y[i] += static_cast<float>(gain[k]) * b.process(x[i]);
  }
  float pk = 1e-9f;
  for (float v : y) pk = std::max(pk, std::fabs(v));
  for (auto& v : y) v *= 0.3f / pk;
  return y;
}

}  // namespace vfxtest
