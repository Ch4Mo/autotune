// TD-PSOLA pitch shifter. Formants are preserved by construction (the
// spectral envelope of each pitch period is kept), which is what makes pitch
// correction sound natural on voice. An optional formant shift resamples each
// grain, independently from the pitch.
#pragma once

#include <vector>

#include "pitch_tracker.hpp"

namespace vfx {

struct PitchMarks {
  std::vector<double> pos;    // analysis epochs, in samples (ascending)
  std::vector<char> voiced;   // 1 if the epoch belongs to a voiced region
};

// Places one analysis epoch per pitch period (aligned on the main peak of each
// period) in voiced regions, and a regular grid in unvoiced regions.
PitchMarks computePitchMarks(const std::vector<float>& x, const PitchTrack& track);

// Resynthesises x so that its pitch follows targetHz (one value per pitch-track
// frame, 0 = keep original pitch). formantSemitones moves the vocal-tract
// resonances (negative = darker/bigger, positive = brighter/smaller).
std::vector<float> psolaResynthesize(const std::vector<float>& x, const PitchTrack& track,
                                     const PitchMarks& marks, const std::vector<float>& targetHz,
                                     float formantSemitones = 0.0f);

}  // namespace vfx
