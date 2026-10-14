// Small DSP building blocks shared by the whole engine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace vfx {

constexpr double kPi = 3.14159265358979323846;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

inline double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

// Catmull-Rom style cubic interpolation of x at fractional position pos.
inline float cubicAt(const std::vector<float>& x, double pos) {
  const long n = static_cast<long>(x.size());
  const long i = static_cast<long>(std::floor(pos));
  const float t = static_cast<float>(pos - static_cast<double>(i));
  auto s = [&](long k) -> float { return (k >= 0 && k < n) ? x[static_cast<size_t>(k)] : 0.0f; };
  const float y0 = s(i - 1), y1 = s(i), y2 = s(i + 1), y3 = s(i + 2);
  const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
  const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  const float c = -0.5f * y0 + 0.5f * y2;
  return ((a * t + b) * t + c) * t + y1;
}

// RBJ "Audio EQ Cookbook" biquad, transposed direct form II.
class Biquad {
 public:
  enum class Type { LowPass, HighPass, BandPass, Peak, LowShelf, HighShelf };

  void setup(Type type, double sampleRate, double freq, double q, double gainDb = 0.0) {
    freq = std::min(freq, sampleRate * 0.45);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / sampleRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
      case Type::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      case Type::HighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      case Type::BandPass:  // constant 0 dB peak gain
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      case Type::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
      case Type::LowShelf: {
        const double sq = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
        break;
      }
      case Type::HighShelf: {
        const double sq = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
        break;
      }
    }
    setCoefficients(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
  }

  void setCoefficients(double b0, double b1, double b2, double a1, double a2) {
    b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
  }

  void reset() { z1_ = z2_ = 0.0; }

  inline float process(float x) {
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
  }

  void processBuffer(std::vector<float>& buf) {
    for (auto& s : buf) s = process(s);
  }

 private:
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  double z1_ = 0, z2_ = 0;
};

// Envelope follower with separate attack / release time constants.
class EnvelopeFollower {
 public:
  void setup(double sampleRate, double attackMs, double releaseMs) {
    att_ = coef(sampleRate, attackMs);
    rel_ = coef(sampleRate, releaseMs);
  }
  inline float process(float x) {
    const float c = x > env_ ? att_ : rel_;
    env_ = c * env_ + (1.0f - c) * x;
    return env_;
  }
  float value() const { return env_; }
  void reset(float v = 0.0f) { env_ = v; }

  static float coef(double sampleRate, double ms) {
    if (ms <= 0.0) return 0.0f;
    return static_cast<float>(std::exp(-1.0 / (sampleRate * ms * 0.001)));
  }

 private:
  float att_ = 0, rel_ = 0, env_ = 0;
};

}  // namespace vfx
