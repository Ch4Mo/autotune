#include "fft.hpp"

#include <cmath>
#include <stdexcept>

#include "dsp.hpp"

namespace vfx {

FFT::FFT(size_t size) : n_(size) {
  if (size < 2 || (size & (size - 1)) != 0) throw std::invalid_argument("FFT size must be a power of two");
  size_t bits = 0;
  while ((size_t{1} << bits) < n_) ++bits;
  bitrev_.resize(n_);
  for (size_t i = 0; i < n_; ++i) {
    size_t r = 0;
    for (size_t b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
    bitrev_[i] = r;
  }
  twiddle_.resize(n_ / 2);
  for (size_t i = 0; i < n_ / 2; ++i) {
    const double a = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_);
    twiddle_[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
  }
}

void FFT::transform(std::vector<std::complex<float>>& d, bool inverse) const {
  for (size_t i = 0; i < n_; ++i) {
    const size_t j = bitrev_[i];
    if (j > i) std::swap(d[i], d[j]);
  }
  for (size_t len = 2; len <= n_; len <<= 1) {
    const size_t half = len >> 1;
    const size_t step = n_ / len;
    for (size_t i = 0; i < n_; i += len) {
      for (size_t k = 0; k < half; ++k) {
        std::complex<float> w = twiddle_[k * step];
        if (inverse) w = std::conj(w);
        const std::complex<float> t = w * d[i + k + half];
        d[i + k + half] = d[i + k] - t;
        d[i + k] += t;
      }
    }
  }
}

void FFT::inverse(std::vector<std::complex<float>>& data) const {
  transform(data, true);
  const float s = 1.0f / static_cast<float>(n_);
  for (auto& v : data) v *= s;
}

}  // namespace vfx
