// Full vocal render: clean-up -> pitch correction -> voices -> channel strip -> space -> mastering.
#pragma once

#include <string>
#include <vector>

#include "../voicefx.h"
#include "denoise.hpp"
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
  DenoiseReport noise;   // background noise of the raw take
  float voiceLevelDb = -120.0f;

  bool rendered = false;
  Stereo output;
  std::vector<float> correctedHz;  // per pitch frame, for display
  double outputLufs = -14.0;
  vfx_params lastParams{};

  // Instrumental to sing over (resampled to sampleRate).
  bool hasBacking = false;
  Stereo backing;
  double backingLufs = -14.0;
  KeyEstimate backingKey;

  void analyze();
  void setBacking(const std::string& path);
  void clearBacking();
  void render(const vfx_params& p);
  Stereo loudnessMatchedOriginal() const;

 private:
  // Denoised copy of the input, cached per strength (slow to compute).
  std::vector<float> denoised_;
  float denoisedStrength_ = -1.0f;
  const std::vector<float>& cleanInput(float strength);

  bool mixesBacking(const vfx_params& p) const { return hasBacking && p.use_backing != 0; }
  // Places the voice on the instrumental's timeline with a balanced level.
  // Returns the mix and the loudness it should be mastered to.
  Stereo mixWithBacking(const Stereo& voice, const vfx_params& p, double& targetLufs, size_t& contentLen) const;
};

vfx_params presetParams(int preset);

}  // namespace vfx
