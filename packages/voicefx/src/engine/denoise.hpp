// Noise reduction for phone recordings (hiss, hum, buzz): spectral denoising.
#pragma once

#include <vector>

namespace vfx {

struct DenoiseReport {
  float humHz = 0.0f;          // 50 / 60 when mains hum was found, else 0
  float noiseFloorDb = -120.0f;  // estimated background noise level (dBFS)
};

// Removes stationary background noise (hiss, fan, electrical buzz) from a
// mono voice recording. strength 0..1 controls how much noise is removed
// (up to ~26 dB); 0 leaves the signal untouched.
//
// Noise is learned from the quietest parts of the take (pauses between
// phrases), then removed with a decision-directed Wiener filter in the STFT
// domain, which avoids the "musical noise" of plain spectral subtraction.
// voiceLevelDb (typical level of the singing, dBFS) keeps the noise learning
// away from soft singing: only parts >= 20 dB below it are taken as noise.
DenoiseReport denoise(std::vector<float>& x, double sampleRate, float strength, float voiceLevelDb = 0.0f);

// Measures the background noise without changing anything: noise floor and
// mains hum. Used to suggest the cleaning to the user.
DenoiseReport analyseNoise(const std::vector<float>& x, double sampleRate, float voiceLevelDb = 0.0f);

}  // namespace vfx
