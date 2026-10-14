// Offline monophonic pitch tracker (YIN, FFT-accelerated) tuned for singing voice.
#pragma once

#include <vector>

namespace vfx {

struct PitchTrack {
  double sampleRate = 48000.0;  // sample rate of the analysed signal
  double hop = 240.0;           // frame hop, in samples of the analysed signal
  double offset = 0.0;          // time of frame 0, in samples
  std::vector<float> f0;        // Hz, 0 = unvoiced
  std::vector<float> confidence;  // 0..1 (1 - YIN aperiodicity)
  std::vector<float> rmsDb;     // frame level in dBFS

  size_t frames() const { return f0.size(); }
  double frameTime(size_t i) const { return offset + hop * static_cast<double>(i); }

  // f0 at an arbitrary sample position (linear interpolation, 0 if unvoiced).
  float f0At(double samplePos) const;
  bool voicedAt(double samplePos) const { return f0At(samplePos) > 0.0f; }
};

struct PitchTrackerConfig {
  double minF0 = 65.0;     // C2
  double maxF0 = 1100.0;   // ~C#6
  double hopSeconds = 0.005;
  double windowSeconds = 0.032;
  double yinThreshold = 0.15;
  double voicingThreshold = 0.30;  // max aperiodicity accepted as voiced
  double silenceDb = -50.0;        // absolute floor
  double relativeSilenceDb = -40.0;  // relative to loudest frame
};

PitchTrack trackPitch(const std::vector<float>& mono, double sampleRate,
                      const PitchTrackerConfig& cfg = PitchTrackerConfig());

}  // namespace vfx
