// WAV reading/writing and MP3 encoding (LAME).
#pragma once

#include <string>
#include <vector>

#include "effects.hpp"

namespace vfx {

struct MonoAudio {
  double sampleRate = 48000.0;
  std::vector<float> samples;
};

struct StereoAudio {
  double sampleRate = 48000.0;
  Stereo samples;
};

// Reads PCM 8/16/24/32-bit or float32/64 WAV (incl. WAVE_FORMAT_EXTENSIBLE).
// Multi-channel files are down-mixed to mono. Throws std::runtime_error.
MonoAudio readWav(const std::string& path);

// Reads an MP3 or WAV file as stereo (mono files are duplicated, extra
// channels dropped). The format is detected from the content, not the name.
StereoAudio readAudioFile(const std::string& path);

// Writes a stereo WAV. bits = 16 (TPDF dithered), 24, or 32 (float).
void writeWav(const std::string& path, const Stereo& s, double sampleRate, int bits);
void writeWavMono(const std::string& path, const std::vector<float>& x, double sampleRate, int bits);

// Encodes a stereo MP3 (CBR, joint stereo, high-quality psychoacoustics).
void writeMp3(const std::string& path, const Stereo& s, double sampleRate, int kbps);

}  // namespace vfx
