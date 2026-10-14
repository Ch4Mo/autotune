#include "tuning.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "dsp.hpp"

namespace vfx {

std::array<bool, 12> scaleMask(const Key& key) {
  static const std::vector<int> kMajor{0, 2, 4, 5, 7, 9, 11};
  static const std::vector<int> kMinor{0, 2, 3, 5, 7, 8, 10};
  static const std::vector<int> kHarmMinor{0, 2, 3, 5, 7, 8, 11};
  static const std::vector<int> kMajPent{0, 2, 4, 7, 9};
  static const std::vector<int> kMinPent{0, 3, 5, 7, 10};
  static const std::vector<int> kBlues{0, 3, 5, 6, 7, 10};

  std::array<bool, 12> mask{};
  const std::vector<int>* steps = nullptr;
  switch (key.scale) {
    case ScaleType::Chromatic: mask.fill(true); return mask;
    case ScaleType::Major: steps = &kMajor; break;
    case ScaleType::Minor: steps = &kMinor; break;
    case ScaleType::HarmonicMinor: steps = &kHarmMinor; break;
    case ScaleType::MajorPentatonic: steps = &kMajPent; break;
    case ScaleType::MinorPentatonic: steps = &kMinPent; break;
    case ScaleType::Blues: steps = &kBlues; break;
  }
  mask.fill(false);
  for (int s : *steps) mask[static_cast<size_t>(((key.root + s) % 12 + 12) % 12)] = true;
  return mask;
}

namespace {

int pitchClass(int midi) { return ((midi % 12) + 12) % 12; }

// Nearest allowed MIDI note to a fractional MIDI value.
int nearestAllowed(double midi, const std::array<bool, 12>& mask) {
  const int base = static_cast<int>(std::lround(midi));
  int best = base;
  double bestDist = 1e9;
  for (int d = -6; d <= 6; ++d) {
    const int n = base + d;
    if (!mask[static_cast<size_t>(pitchClass(n))]) continue;
    const double dist = std::fabs(midi - n);
    if (dist < bestDist) { bestDist = dist; best = n; }
  }
  return best;
}

double pearson(const std::array<double, 12>& a, const std::array<double, 12>& b) {
  const double ma = std::accumulate(a.begin(), a.end(), 0.0) / 12.0;
  const double mb = std::accumulate(b.begin(), b.end(), 0.0) / 12.0;
  double num = 0, da = 0, db = 0;
  for (size_t i = 0; i < 12; ++i) {
    num += (a[i] - ma) * (b[i] - mb);
    da += (a[i] - ma) * (a[i] - ma);
    db += (b[i] - mb) * (b[i] - mb);
  }
  return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
}

}  // namespace

KeyEstimate detectKey(const PitchTrack& track) {
  // Krumhansl-Kessler key profiles.
  static const std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                                     2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
  static const std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                                     2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

  std::array<double, 12> hist{};
  double total = 0.0;
  for (size_t i = 0; i < track.frames(); ++i) {
    const float f = track.f0[i];
    if (f <= 0.0f) continue;
    const double m = hzToMidi(f);
    const int n = static_cast<int>(std::lround(m));
    // Frames close to a semitone centre are more reliable than slides.
    const double w = track.confidence[i] * (1.0 - std::min(1.0, std::fabs(m - n) * 2.0));
    hist[static_cast<size_t>(pitchClass(n))] += w;
    total += w;
  }

  KeyEstimate est;
  est.key = Key{0, ScaleType::Chromatic};
  // Need about one second of confident singing to say anything useful.
  const double framesPerSecond = track.sampleRate / track.hop;
  if (total < framesPerSecond * 0.5) return est;

  double best = -2.0, second = -2.0;
  for (int root = 0; root < 12; ++root) {
    for (int minor = 0; minor < 2; ++minor) {
      std::array<double, 12> prof{};
      const auto& src = minor ? kMinorProfile : kMajorProfile;
      for (int i = 0; i < 12; ++i) prof[static_cast<size_t>((i + root) % 12)] = src[static_cast<size_t>(i)];
      const double r = pearson(hist, prof);
      if (r > best) {
        second = best;
        best = r;
        est.key = Key{root, minor ? ScaleType::Minor : ScaleType::Major};
      } else if (r > second) {
        second = r;
      }
    }
  }
  est.confidence = static_cast<float>(std::clamp((best - second) * 5.0 + std::max(0.0, best) * 0.5, 0.0, 1.0));
  return est;
}

Correction correctPitch(const PitchTrack& track, const CorrectionParams& p) {
  const size_t n = track.frames();
  Correction out;
  out.hz.assign(n, 0.0f);
  out.note.assign(n, -1);

  const auto mask = scaleMask(p.key);
  const double hopMs = 1000.0 * track.hop / track.sampleRate;
  const double amount = std::clamp(static_cast<double>(p.amount), 0.0, 1.0);
  // Hysteresis (semitones): how far past the midpoint the voice must go before
  // we jump to the neighbouring note. Prevents warbling on vibrato.
  const double hysteresis = 0.18;

  size_t f = 0;
  while (f < n) {
    if (track.f0[f] <= 0.0f) { ++f; continue; }
    size_t e = f;
    while (e < n && track.f0[e] > 0.0f) ++e;

    // --- Note selection for the voiced segment [f, e) --------------------
    std::vector<double> midi(e - f), smooth(e - f);
    for (size_t i = f; i < e; ++i) midi[i - f] = hzToMidi(track.f0[i]);
    // ~30 ms moving average, used only for the note decision.
    const long R = std::max<long>(1, std::lround(15.0 / hopMs));
    for (long i = 0; i < static_cast<long>(midi.size()); ++i) {
      double s = 0; long c = 0;
      for (long k = std::max(0L, i - R); k <= std::min<long>(midi.size() - 1, i + R); ++k) { s += midi[k]; ++c; }
      smooth[i] = s / c;
    }

    std::vector<int> notes(e - f);
    if (p.robot) {
      // Monotone: the whole phrase on the tonic closest to its median pitch.
      std::vector<double> tmp = midi;
      std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
      const double med = tmp[tmp.size() / 2];
      int best = 0; double bestDist = 1e9;
      for (int oct = -1; oct <= 10; ++oct) {
        const int cand = p.key.root + 12 * oct;
        if (std::fabs(cand - med) < bestDist) { bestDist = std::fabs(cand - med); best = cand; }
      }
      std::fill(notes.begin(), notes.end(), best);
    } else {
      int current = nearestAllowed(smooth[0], mask);
      for (size_t i = 0; i < notes.size(); ++i) {
        const int cand = nearestAllowed(smooth[i], mask);
        if (cand != current &&
            std::fabs(smooth[i] - current) > std::fabs(smooth[i] - cand) + hysteresis) {
          current = cand;
        }
        notes[i] = current;
      }
    }

    // --- Correction curve -------------------------------------------------
    // The correction offset (target - sung) is low-pass filtered with the
    // retune-speed time constant: slow drift is corrected, while quick
    // expressive movements survive when the retune speed is slow.
    double offset = notes[0] - midi[0];
    double noteStartMs = 0.0;
    for (size_t i = 0; i < notes.size(); ++i) {
      if (i > 0 && notes[i] != notes[i - 1]) noteStartMs = static_cast<double>(i) * hopMs;
      const double sinceNote = static_cast<double>(i) * hopMs - noteStartMs;
      double tau = p.retuneMs;
      if (p.humanize > 0.0f) {
        const double sustain = std::clamp((sinceNote - 150.0) / 300.0, 0.0, 1.0);
        tau += p.humanize * 180.0 * sustain;
      }
      const double target = notes[i] - midi[i];
      if (p.robot || tau <= 0.5) {
        offset = target;
      } else {
        const double a = std::exp(-hopMs / tau);
        offset = a * offset + (1.0 - a) * target;
      }
      const double outMidi = midi[i] + amount * offset + p.transpose;
      out.hz[f + i] = static_cast<float>(midiToHz(outMidi));
      out.note[f + i] = notes[i];
    }
    f = e;
  }
  return out;
}

std::vector<float> diatonicShift(const Correction& lead, const Key& key, int degrees) {
  const auto mask = scaleMask(key);
  std::vector<float> out(lead.hz.size(), 0.0f);
  for (size_t i = 0; i < lead.hz.size(); ++i) {
    if (lead.hz[i] <= 0.0f || lead.note[i] < 0) continue;
    const double m = hzToMidi(lead.hz[i]);
    // Snap the reference note into the key (in case the lead uses chromatic notes).
    int note = nearestAllowed(lead.note[i], mask);
    const double deviation = m - lead.note[i];
    int steps = std::abs(degrees);
    const int dir = degrees >= 0 ? 1 : -1;
    while (steps > 0) {
      note += dir;
      if (mask[static_cast<size_t>(pitchClass(note))]) --steps;
    }
    out[i] = static_cast<float>(midiToHz(note + deviation));
  }
  return out;
}

}  // namespace vfx
