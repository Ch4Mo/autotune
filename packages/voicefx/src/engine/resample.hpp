// High-quality offline sample-rate conversion (Kaiser-windowed sinc).
#pragma once

#include <vector>

namespace vfx {

// Converts x from inRate to outRate. Pass-band flat to 0.45 * min(rates),
// stop-band attenuation > 60 dB above the new Nyquist. Returns x unchanged when the rates match.
std::vector<float> resample(const std::vector<float>& x, double inRate, double outRate);

}  // namespace vfx
