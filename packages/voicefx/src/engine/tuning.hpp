// Musical scales, key detection and pitch-correction curve generation.
#pragma once

#include <array>
#include <vector>

#include "pitch_tracker.hpp"

namespace vfx {

enum class ScaleType : int {
  Chromatic = 0,
  Major = 1,
  Minor = 2,           // natural minor
  HarmonicMinor = 3,
  MajorPentatonic = 4,
  MinorPentatonic = 5,
  Blues = 6,
};

struct Key {
  int root = 0;  // 0 = C ... 11 = B
  ScaleType scale = ScaleType::Chromatic;
};

// 12-entry mask of allowed pitch classes for a key.
std::array<bool, 12> scaleMask(const Key& key);

struct KeyEstimate {
  Key key;
  float confidence = 0.0f;  // 0..1, correlation margin
};

// Krumhansl-Schmuckler key estimation on the sung melody.
KeyEstimate detectKey(const PitchTrack& track);

struct CorrectionParams {
  Key key;
  float amount = 1.0f;        // 0 = no correction, 1 = full
  float retuneMs = 20.0f;     // 0 = instantaneous (hard-tune), ~200 = very gentle
  float humanize = 0.0f;      // 0..1: longer notes get softer correction
  float transpose = 0.0f;     // semitones, applied after correction
  bool robot = false;         // monotone: each phrase sung on one single note
};

struct Correction {
  std::vector<float> hz;   // corrected target pitch per frame (0 = unvoiced)
  std::vector<int> note;   // chosen MIDI note per frame (-1 = unvoiced)
};

Correction correctPitch(const PitchTrack& track, const CorrectionParams& p);

// Shift a corrected melody by a number of scale degrees, staying in key, while
// keeping the expressive deviation (vibrato, slides) of the lead voice.
// Used for automatic harmonies. Returns Hz per frame (0 = unvoiced).
std::vector<float> diatonicShift(const Correction& lead, const Key& key, int degrees);

}  // namespace vfx
