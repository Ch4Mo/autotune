// Offline vocal effects: dynamics, EQ, space (delay / reverb), mastering.
// Everything works on whole buffers, which allows zero-latency look-ahead.
#pragma once

#include <array>
#include <vector>

#include "dsp.hpp"

namespace vfx {

struct Stereo {
  std::vector<float> l, r;
  explicit Stereo(size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
  size_t size() const { return l.size(); }
};

// Downward expander used as a soft noise gate (keeps breaths natural).
void noiseGate(std::vector<float>& x, double sr, float thresholdDb, float rangeDb = 18.0f);

// Linked-stereo split-band de-esser.
void deEss(Stereo& s, double sr, float amount /*0..1*/);

// Vocal tone shaping. warmth/presence/air are in dB (can be negative).
void vocalEq(Stereo& s, double sr, float lowCutHz, float warmthDb, float presenceDb, float airDb);

// Linked-stereo feed-forward compressor with soft knee (make-up gain is left
// to the final loudness normalisation).
void compress(Stereo& s, double sr, float thresholdDb, float ratio, float attackMs, float releaseMs);

// Power-of-two circular delay line.
class DelayLine {
 public:
  explicit DelayLine(size_t maxDelay = 1);
  // Sample written `delay` writes ago (1 <= delay <= maxDelay).
  float read(size_t delay) const { return buf_[(pos_ - delay + 1) & mask_]; }
  void write(float v) {
    pos_ = (pos_ + 1) & mask_;
    buf_[pos_] = v;
  }

 private:
  std::vector<float> buf_;
  size_t mask_ = 0;
  size_t pos_ = 0;
};

// Stereo ping-pong echo, streaming. Outputs only the wet signal.
class PingPongDelay {
 public:
  PingPongDelay(double sr, float timeMs, float feedback, float toneHz);
  void process(float inL, float inR, float& outL, float& outR);

 private:
  size_t d_;
  float fb_;
  DelayLine l_, r_;
  Biquad toneL_, toneR_, hp_;
};

// 8-line feedback-delay-network reverb with input diffusion, streaming.
// Outputs only the wet signal.
class FdnReverb {
 public:
  FdnReverb(double sr, float size /*0..1*/, float decaySeconds, float damping /*0..1*/, float predelayMs);
  void process(float inL, float inR, float& outL, float& outR);

 private:
  static constexpr int kLines = 8;
  struct Allpass {
    DelayLine line;
    size_t delay;
    float g;
    float process(float x) {
      const float d = line.read(delay);
      const float v = x + g * d;
      line.write(v);
      return d - g * v;
    }
  };
  std::array<DelayLine, kLines> lines_;
  std::array<size_t, kLines> len_{};
  std::array<float, kLines> g_{}, lp_{};
  std::vector<Allpass> diffusers_;
  DelayLine pre_;
  size_t preDelay_;
  float damp_;
  Biquad inHp_, inLp_;
};

// Integrated loudness, ITU-R BS.1770-4 (LUFS). Returns -120 for silence.
double integratedLoudness(const Stereo& s, double sr);

// Look-ahead brickwall limiter (offline, so the look-ahead costs no latency).
void limit(Stereo& s, double sr, float ceilingDb, float releaseMs = 60.0f);

}  // namespace vfx
