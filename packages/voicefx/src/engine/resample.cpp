#include "resample.hpp"

#include <cmath>
#include <numeric>

#include "dsp.hpp"

namespace vfx {

namespace {

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

std::vector<float> resample(const std::vector<float>& x, double inRate, double outRate) {
  if (x.empty() || std::fabs(inRate - outRate) < 1e-6) return x;
  constexpr int kZeroCrossings = 24;   // half-length of the kernel, in zero crossings
  constexpr int kPhases = 512;         // table resolution per zero crossing
  constexpr double kBeta = 9.0;        // Kaiser window shape

  const double ratio = outRate / inRate;
  const double cutoff = 0.90 * std::min(1.0, ratio);  // relative to the input Nyquist

  // Kernel table h(t) for t in [0, kZeroCrossings], t in units of input samples * cutoff.
  std::vector<float> table(kZeroCrossings * kPhases + 2);
  const double i0b = besselI0(kBeta);
  for (size_t i = 0; i < table.size(); ++i) {
    const double t = static_cast<double>(i) / kPhases;
    const double sinc = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
    const double u = t / kZeroCrossings;
    const double w = u >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0b;
    table[i] = static_cast<float>(sinc * w);
  }

  const size_t outLen = static_cast<size_t>(std::floor(static_cast<double>(x.size()) * ratio));
  std::vector<float> y(outLen);
  const double reach = kZeroCrossings / cutoff;  // kernel half-width in input samples
  const long n = static_cast<long>(x.size());
  auto kernel = [&](double d) -> float {
    const double pos = std::fabs(d) * cutoff * kPhases;
    const size_t idx = static_cast<size_t>(pos);
    if (idx + 1 >= table.size()) return 0.0f;
    const double frac = pos - static_cast<double>(idx);
    return static_cast<float>((table[idx] + frac * (table[idx + 1] - table[idx])) * cutoff);
  };

  // Integer rates with a small common ratio (44.1k <-> 48k is 147/160):
  // polyphase filter with exact precomputed coefficients per output phase.
  const long inR = std::lround(inRate), outR = std::lround(outRate);
  long g = std::gcd(inR, outR);
  const long L = g > 0 ? outR / g : 0, M = g > 0 ? inR / g : 0;
  const long H = static_cast<long>(std::ceil(reach));
  if (std::fabs(inRate - inR) < 1e-9 && std::fabs(outRate - outR) < 1e-9 && L > 0 && L <= 4096) {
    const long taps = 2 * H;
    std::vector<float> coefs(static_cast<size_t>(L * taps));
    for (long ph = 0; ph < L; ++ph)
      for (long j = 0; j < taps; ++j)  // input index = base - H + 1 + j
        coefs[static_cast<size_t>(ph * taps + j)] = kernel(static_cast<double>(ph) / L + static_cast<double>(H - 1 - j));
    for (size_t i = 0; i < outLen; ++i) {
      const long num = static_cast<long>(i) * M;
      const long base = num / L, ph = num % L;
      const float* c = &coefs[static_cast<size_t>(ph * taps)];
      const long k0 = base - H + 1;
      float acc = 0.0f;
      if (k0 >= 0 && k0 + taps <= n) {
        const float* src = &x[static_cast<size_t>(k0)];
        for (long j = 0; j < taps; ++j) acc += src[j] * c[j];
      } else {
        for (long j = 0; j < taps; ++j) {
          const long k = k0 + j;
          if (k >= 0 && k < n) acc += x[static_cast<size_t>(k)] * c[j];
        }
      }
      y[i] = acc;
    }
    return y;
  }

  // Generic ratio: evaluate the kernel on the fly.
  for (size_t i = 0; i < outLen; ++i) {
    const double t = static_cast<double>(i) / ratio;
    const long k0 = std::max(0L, static_cast<long>(std::ceil(t - reach)));
    const long k1 = std::min(n - 1, static_cast<long>(std::floor(t + reach)));
    double acc = 0.0;
    for (long k = k0; k <= k1; ++k) acc += x[static_cast<size_t>(k)] * kernel(t - static_cast<double>(k));
    y[i] = static_cast<float>(acc);
  }
  return y;
}

}  // namespace vfx
