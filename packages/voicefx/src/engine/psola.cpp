#include "psola.hpp"

#include <algorithm>
#include <cmath>

#include "dsp.hpp"

namespace vfx {

namespace {

// Interpolates a per-frame curve (0 = undefined) at a sample position.
float curveAt(const std::vector<float>& curve, const PitchTrack& track, double samplePos) {
  if (curve.empty()) return 0.0f;
  const double p = (samplePos - track.offset) / track.hop;
  if (p <= 0.0) return curve.front();
  const size_t last = curve.size() - 1;
  if (p >= static_cast<double>(last)) return curve.back();
  const size_t i = static_cast<size_t>(p);
  const double t = p - static_cast<double>(i);
  const float a = curve[i], b = curve[i + 1];
  if (a > 0.0f && b > 0.0f) return static_cast<float>(std::exp((1.0 - t) * std::log(a) + t * std::log(b)));
  return t < 0.5 ? a : b;
}

}  // namespace

PitchMarks computePitchMarks(const std::vector<float>& x, const PitchTrack& track) {
  PitchMarks marks;
  const double sr = track.sampleRate;
  const long len = static_cast<long>(x.size());
  if (len == 0) return marks;

  // Low-passed copy: makes the per-period main peak stable.
  std::vector<float> lp(x.size());
  {
    Biquad a, b;
    a.setup(Biquad::Type::LowPass, sr, 1000.0, 0.7071);
    b.setup(Biquad::Type::LowPass, sr, 1000.0, 0.7071);
    for (size_t i = 0; i < x.size(); ++i) lp[i] = b.process(a.process(x[i]));
    // Compensate the (small) group delay by zero-phase filtering backwards too.
    a.reset(); b.reset();
    for (size_t i = x.size(); i-- > 0;) lp[i] = b.process(a.process(lp[i]));
  }

  const double unvoicedPeriod = 0.005 * sr;
  double pos = 0.0;
  bool prevVoiced = false;
  float polarity = 1.0f;

  while (pos < static_cast<double>(len)) {
    const float f0 = track.f0At(pos);
    if (f0 <= 0.0f) {
      marks.pos.push_back(pos);
      marks.voiced.push_back(0);
      pos += unvoicedPeriod;
      prevVoiced = false;
      continue;
    }
    const double T = sr / f0;
    long lo, hi;
    double predicted;
    if (!prevVoiced) {
      // Start of a voiced region: choose polarity from the next ~3 periods.
      const long end = std::min<long>(len, static_cast<long>(pos + 3 * T));
      float mx = 0.0f, mn = 0.0f;
      for (long k = static_cast<long>(pos); k < end; ++k) { mx = std::max(mx, lp[k]); mn = std::min(mn, lp[k]); }
      polarity = (mx >= -mn) ? 1.0f : -1.0f;
      lo = static_cast<long>(pos);
      hi = static_cast<long>(pos + T);
      predicted = pos + 0.5 * T;
    } else {
      predicted = marks.pos.back() + T;
      lo = static_cast<long>(predicted - 0.3 * T);
      hi = static_cast<long>(predicted + 0.3 * T);
    }
    lo = std::clamp(lo, 0L, len - 1);
    hi = std::clamp(hi, lo, len - 1);

    long best = static_cast<long>(std::clamp(predicted, 0.0, static_cast<double>(len - 1)));
    float bestScore = -1e30f;
    const double halfSpan = std::max(1.0, 0.3 * T);
    for (long k = lo; k <= hi; ++k) {
      const double dev = (static_cast<double>(k) - predicted) / halfSpan;
      const float score = polarity * lp[k] - static_cast<float>(0.15 * dev * dev) * std::fabs(lp[k]);
      if (score > bestScore) { bestScore = score; best = k; }
    }
    double next = static_cast<double>(best);
    if (prevVoiced && next <= marks.pos.back() + 0.5 * T) next = marks.pos.back() + T;
    if (!marks.pos.empty() && next <= marks.pos.back()) next = marks.pos.back() + 1.0;
    marks.pos.push_back(next);
    marks.voiced.push_back(1);
    prevVoiced = true;
    pos = next + 0.5 * T;
  }
  return marks;
}

std::vector<float> psolaResynthesize(const std::vector<float>& x, const PitchTrack& track,
                                     const PitchMarks& marks, const std::vector<float>& targetHz,
                                     float formantSemitones) {
  const long len = static_cast<long>(x.size());
  std::vector<float> out(x.size(), 0.0f);
  if (len == 0 || marks.pos.empty()) return out;
  std::vector<float> wsum(x.size(), 0.0f);

  const double sr = track.sampleRate;
  const double ff = std::pow(2.0, formantSemitones / 12.0);
  const size_t M = marks.pos.size();

  auto periodAt = [&](size_t k) -> double {
    if (k + 1 < M) return marks.pos[k + 1] - marks.pos[k];
    if (k > 0) return marks.pos[k] - marks.pos[k - 1];
    return 0.005 * sr;
  };

  double ts = marks.pos[0];
  size_t k = 0;
  while (ts < static_cast<double>(len)) {
    while (k + 1 < M && std::fabs(marks.pos[k + 1] - ts) <= std::fabs(marks.pos[k] - ts)) ++k;
    const double Ta = std::max(16.0, periodAt(k));
    const bool voiced = marks.voiced[k] != 0;
    const float target = voiced ? curveAt(targetHz, track, ts) : 0.0f;

    double place = ts;
    double Ts = Ta;
    if (voiced && target > 0.0f) {
      Ts = std::clamp(sr / target, Ta / 2.5, Ta * 2.5);
    } else {
      // Unvoiced (or no target): copy the grain in place -> exact reconstruction.
      place = marks.pos[k];
    }

    const double half = (Ts > Ta) ? 0.5 * (Ta + Ts) : Ta;
    const long o0 = std::max(0L, static_cast<long>(std::ceil(place - half)));
    const long o1 = std::min(len - 1, static_cast<long>(std::floor(place + half)));
    const double src0 = marks.pos[k];
    for (long o = o0; o <= o1; ++o) {
      const double rel = static_cast<double>(o) - place;
      const float w = static_cast<float>(0.5 * (1.0 + std::cos(kPi * rel / half)));
      const float v = cubicAt(x, src0 + rel * ff);
      out[static_cast<size_t>(o)] += w * v;
      wsum[static_cast<size_t>(o)] += w;
    }
    ts = place + Ts;
  }

  for (size_t i = 0; i < out.size(); ++i) out[i] /= std::max(wsum[i], 0.25f);
  return out;
}

}  // namespace vfx
