// Full vocal render: clean-up -> pitch correction -> voices -> channel strip -> space -> mastering.
#pragma once

#include <vector>

#include "../voicefx.h"
#include "effects.hpp"
#include "psola.hpp"
#include "tuning.hpp"

namespace vfx {

struct Session {
  double sampleRate = 48000.0;
  std::vector<float> input;

  bool analyzed = false;
  PitchTrack track;
  PitchMarks marks;
  KeyEstimate key;

  bool rendered = false;
  Stereo output;
  std::vector<float> correctedHz;  // per pitch frame, for display
  double outputLufs = -14.0;

  void analyze();
  void render(const vfx_params& p);
  Stereo loudnessMatchedOriginal() const;
};

vfx_params presetParams(int preset);

}  // namespace vfx
