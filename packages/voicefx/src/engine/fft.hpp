// Minimal iterative radix-2 complex FFT (power-of-two sizes only).
#pragma once

#include <complex>
#include <vector>

namespace vfx {

class FFT {
 public:
  explicit FFT(size_t size);

  size_t size() const { return n_; }

  // In-place forward transform (e^{-i...}). No scaling.
  void forward(std::vector<std::complex<float>>& data) const { transform(data, false); }
  // In-place inverse transform, scaled by 1/N.
  void inverse(std::vector<std::complex<float>>& data) const;

 private:
  void transform(std::vector<std::complex<float>>& data, bool inverse) const;

  size_t n_;
  std::vector<size_t> bitrev_;
  std::vector<std::complex<float>> twiddle_;
};

}  // namespace vfx
