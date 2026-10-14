// Engine unit tests (desktop). Build: cmake -S src -B build -DVOICEFX_BUILD_TESTS=ON
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/api_internal.hpp"
#include "engine/audio_io.hpp"
#include "engine/dsp.hpp"
#include "engine/effects.hpp"
#include "engine/pitch_tracker.hpp"
#include "engine/psola.hpp"
#include "engine/renderer.hpp"
#include "engine/tuning.hpp"
#include "synth_voice.hpp"
#include "voicefx.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, ...)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);   \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
    }                                                                 \
  } while (0)

constexpr double kSr = 48000.0;

double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }

// Median pitch (Hz) over voiced frames in [t0, t1] seconds.
double medianPitch(const vfx::PitchTrack& tr, double t0, double t1) {
  std::vector<float> v;
  for (size_t i = 0; i < tr.frames(); ++i) {
    const double t = tr.frameTime(i) / tr.sampleRate;
    if (t >= t0 && t <= t1 && tr.f0[i] > 0) v.push_back(tr.f0[i]);
  }
  if (v.empty()) return 0.0;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

// Max |cents| deviation from `hz` over voiced frames in [t0, t1].
double maxDeviation(const vfx::PitchTrack& tr, double t0, double t1, double hz) {
  double m = 0;
  for (size_t i = 0; i < tr.frames(); ++i) {
    const double t = tr.frameTime(i) / tr.sampleRate;
    if (t >= t0 && t <= t1 && tr.f0[i] > 0) m = std::max(m, std::fabs(centsBetween(tr.f0[i], hz)));
  }
  return m;
}

void testPitchTracker() {
  std::printf("pitch tracker\n");
  for (double f : {82.4, 110.0, 196.0, 261.63, 440.0, 659.3, 880.0}) {
    auto x = vfxtest::synthVoice(kSr, 1.0, [f](double) { return f; });
    auto tr = vfx::trackPitch(x, kSr);
    const double med = medianPitch(tr, 0.1, 0.9);
    CHECK(std::fabs(centsBetween(med, f)) < 5.0, "f=%.1f measured=%.2f", f, med);
  }
  // Silence must be unvoiced.
  std::vector<float> silence(48000, 0.0f);
  auto tr = vfx::trackPitch(silence, kSr);
  size_t voiced = 0;
  for (float v : tr.f0) voiced += v > 0;
  CHECK(voiced == 0, "voiced frames in silence: %zu", voiced);
  // Gap in the middle is detected as unvoiced.
  auto x = vfxtest::synthVoice(kSr, 1.5, [](double t) { return (t > 0.6 && t < 0.9) ? 0.0 : 220.0; });
  tr = vfx::trackPitch(x, kSr);
  CHECK(tr.f0At(0.75 * kSr) == 0.0f, "gap should be unvoiced");
  CHECK(tr.f0At(0.3 * kSr) > 0.0f && tr.f0At(1.2 * kSr) > 0.0f, "notes should be voiced");
}

void testKeyDetection() {
  std::printf("key detection\n");
  // A melody in D major: D E F# G A B C# D, with emphasis on D, F#, A.
  const int notes[] = {62, 66, 69, 66, 62, 64, 66, 67, 69, 71, 73, 74, 69, 66, 62, 69, 62};
  const double noteLen = 0.35;
  const int count = sizeof(notes) / sizeof(notes[0]);
  auto x = vfxtest::synthVoice(kSr, noteLen * count, [&](double t) {
    const int i = std::min(count - 1, static_cast<int>(t / noteLen));
    const double inNote = t - i * noteLen;
    return inNote > noteLen - 0.04 ? 0.0 : vfx::midiToHz(notes[i] - 12);
  });
  auto tr = vfx::trackPitch(x, kSr);
  auto key = vfx::detectKey(tr);
  CHECK(key.key.root == 2 && key.key.scale == vfx::ScaleType::Major, "root=%d scale=%d", key.key.root,
        static_cast<int>(key.key.scale));
}

void testScales() {
  std::printf("scales\n");
  auto m = vfx::scaleMask(vfx::Key{9, vfx::ScaleType::Minor});  // A minor = C major notes
  const bool expected[12] = {true, false, true, false, true, true, false, true, false, true, false, true};
  bool same = true;
  for (int i = 0; i < 12; ++i) same &= m[i] == expected[i];
  CHECK(same, "A minor mask");
}

void testPsolaIdentity() {
  std::printf("psola identity\n");
  auto x = vfxtest::synthVoice(kSr, 1.0, [](double t) { return 200.0 + 20.0 * std::sin(2 * vfx::kPi * 5 * t); });
  auto tr = vfx::trackPitch(x, kSr);
  auto marks = vfx::computePitchMarks(x, tr);
  auto y = vfx::psolaResynthesize(x, tr, marks, {}, 0.0f);
  double err = 0, sig = 0;
  for (size_t i = 4800; i + 4800 < x.size(); ++i) {
    err += (y[i] - x[i]) * (y[i] - x[i]);
    sig += x[i] * x[i];
  }
  const double snr = 10 * std::log10(sig / std::max(err, 1e-20));
  CHECK(snr > 25.0, "identity SNR = %.1f dB", snr);
}

void testPsolaShift() {
  std::printf("psola shift\n");
  for (double semis : {-7.0, -2.0, 0.5, 3.0, 7.0}) {
    const double f = 220.0;
    auto x = vfxtest::synthVoice(kSr, 1.0, [f](double) { return f; });
    auto tr = vfx::trackPitch(x, kSr);
    auto marks = vfx::computePitchMarks(x, tr);
    std::vector<float> target(tr.frames());
    for (size_t i = 0; i < tr.frames(); ++i) target[i] = tr.f0[i] > 0 ? static_cast<float>(f * std::pow(2.0, semis / 12.0)) : 0.0f;
    auto y = vfx::psolaResynthesize(x, tr, marks, target, 0.0f);
    auto tr2 = vfx::trackPitch(y, kSr);
    const double med = medianPitch(tr2, 0.15, 0.85);
    CHECK(std::fabs(centsBetween(med, f * std::pow(2.0, semis / 12.0))) < 8.0, "shift %.1f -> %.2f Hz", semis, med);
    // Level stays roughly constant.
    double ex = 0, ey = 0;
    for (size_t i = 4800; i + 4800 < x.size(); ++i) { ex += x[i] * x[i]; ey += y[i] * y[i]; }
    const double dl = 10 * std::log10(ey / ex);
    CHECK(std::fabs(dl) < 3.0, "level change %.1f dB at %.1f st", dl, semis);
  }
}

void testHardTune() {
  std::printf("hard tune\n");
  // Sung 35 cents sharp of A3 with vibrato; correction must land on 220 Hz.
  const double f = 220.0 * std::pow(2.0, 35.0 / 1200.0);
  auto x = vfxtest::synthVoice(kSr, 1.5, [f](double t) { return f * std::pow(2.0, 0.3 / 12.0 * std::sin(2 * vfx::kPi * 5.5 * t)); });
  auto tr = vfx::trackPitch(x, kSr);
  auto marks = vfx::computePitchMarks(x, tr);
  vfx::CorrectionParams cp;
  cp.key = vfx::Key{9, vfx::ScaleType::Minor};
  cp.retuneMs = 0.0f;
  cp.amount = 1.0f;
  auto corr = vfx::correctPitch(tr, cp);
  auto y = vfx::psolaResynthesize(x, tr, marks, corr.hz, 0.0f);
  auto tr2 = vfx::trackPitch(y, kSr);
  const double dev = maxDeviation(tr2, 0.2, 1.3, 220.0);
  CHECK(dev < 12.0, "max deviation after hard tune = %.1f cents", dev);

  // Gentle correction keeps (part of) the vibrato but centres the note.
  cp.retuneMs = 120.0f;
  cp.humanize = 0.8f;
  corr = vfx::correctPitch(tr, cp);
  y = vfx::psolaResynthesize(x, tr, marks, corr.hz, 0.0f);
  tr2 = vfx::trackPitch(y, kSr);
  const double med = medianPitch(tr2, 0.4, 1.3);
  const double vib = maxDeviation(tr2, 0.4, 1.3, med);
  CHECK(std::fabs(centsBetween(med, 220.0)) < 12.0, "gentle centre %.1f cents", centsBetween(med, 220.0));
  CHECK(vib > 10.0, "vibrato should survive gentle correction (%.1f cents)", vib);
}

void testHarmony() {
  std::printf("harmony\n");
  auto x = vfxtest::synthVoice(kSr, 1.0, [](double) { return vfx::midiToHz(60); });  // C4
  auto tr = vfx::trackPitch(x, kSr);
  vfx::CorrectionParams cp;
  cp.key = vfx::Key{0, vfx::ScaleType::Major};
  auto corr = vfx::correctPitch(tr, cp);
  auto third = vfx::diatonicShift(corr, cp.key, 2);
  auto fifth = vfx::diatonicShift(corr, cp.key, 4);
  const size_t mid = tr.frames() / 2;
  CHECK(std::fabs(vfx::hzToMidi(third[mid]) - 64.0) < 0.1, "third = %.2f", vfx::hzToMidi(third[mid]));
  CHECK(std::fabs(vfx::hzToMidi(fifth[mid]) - 67.0) < 0.1, "fifth = %.2f", vfx::hzToMidi(fifth[mid]));
}

void testLimiterAndLoudness() {
  std::printf("limiter / loudness\n");
  vfx::Stereo s(48000);
  for (size_t i = 0; i < s.size(); ++i) {
    s.l[i] = s.r[i] = static_cast<float>(0.5 * std::sin(2 * vfx::kPi * 1000 * i / kSr));
  }
  // BS.1770: a 0 dBFS 1 kHz sine on one channel reads -3.01 LUFS, so -6 dBFS
  // on two channels reads -3.01 - 6.02 + 3.01 = -6.02 LUFS.
  const double l = vfx::integratedLoudness(s, kSr);
  CHECK(std::fabs(l - (-6.02)) < 0.2, "loudness = %.2f", l);
  for (size_t i = 0; i < s.size(); ++i) { s.l[i] *= 4; s.r[i] *= 4; }
  vfx::limit(s, kSr, -1.0f);
  float pk = 0;
  for (size_t i = 0; i < s.size(); ++i) pk = std::max(pk, std::fabs(s.l[i]));
  CHECK(pk <= vfx::dbToGain(-1.0f) + 1e-6f, "peak = %.4f", pk);
}

bool fileStartsWith(const std::string& path, const char* magic, size_t n) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[8] = {0};
  const size_t r = std::fread(buf, 1, n, f);
  std::fclose(f);
  return r == n && std::memcmp(buf, magic, n) == 0;
}

void testIoAndPresets(const std::string& tmpDir) {
  std::printf("io + presets\n");
  const int notes[] = {57, 59, 60, 62, 64, 62, 60, 59, 57};
  auto x = vfxtest::synthVoice(44100.0, 4.5, [&](double t) {
    const int i = std::min(8, static_cast<int>(t / 0.5));
    // Slightly out of tune singer (+-30 cents drift).
    return t - i * 0.5 > 0.45 ? 0.0 : vfx::midiToHz(notes[i] + 0.3 * std::sin(t * 1.7));
  });
  const std::string in = tmpDir + "/vfx_in.wav";
  vfx::writeWavMono(in, x, 44100.0, 16);
  auto back = vfx::readWav(in);
  CHECK(back.samples.size() == x.size() && back.sampleRate == 44100.0, "wav roundtrip");

  vfx_session* s = vfx_session_open_wav(in.c_str());
  CHECK(s != nullptr, "open: %s", vfx_last_error());
  if (!s) return;
  CHECK(vfx_session_analyze(s) == 0, "analyze");
  int32_t root = -1, scale = -1;
  float conf = 0;
  vfx_session_detected_key(s, &root, &scale, &conf);
  std::printf("  detected key root=%d scale=%d conf=%.2f\n", root, scale, conf);

  for (int preset = 0; preset < VFX_PRESET_COUNT; ++preset) {
    vfx_params p;
    vfx_get_preset(preset, &p);
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = vfx_session_render(s, &p);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(rc == 0, "render preset %d: %s", preset, vfx_last_error());
    const auto& out = s->impl.output;
    bool finite = true;
    float pk = 0;
    for (size_t i = 0; i < out.size(); ++i) {
      finite &= std::isfinite(out.l[i]) && std::isfinite(out.r[i]);
      pk = std::max(pk, std::max(std::fabs(out.l[i]), std::fabs(out.r[i])));
    }
    const double lufs = vfx::integratedLoudness(out, 44100.0);
    std::printf("  preset %d: %.0f ms for 4.5 s, peak %.2f dBFS, %.1f LUFS\n", preset, ms, vfx::gainToDb(pk), lufs);
    CHECK(finite, "preset %d produced NaN/Inf", preset);
    CHECK(pk <= vfx::dbToGain(-1.0f) + 1e-4f, "preset %d peak %.3f", preset, pk);
    CHECK(std::fabs(lufs + 14.0) < 1.5, "preset %d loudness %.1f", preset, lufs);
  }

  const std::string wav = tmpDir + "/vfx_out.wav", mp3 = tmpDir + "/vfx_out.mp3", orig = tmpDir + "/vfx_orig.wav";
  CHECK(vfx_session_export_wav(s, wav.c_str(), 24) == 0, "export wav: %s", vfx_last_error());
  CHECK(vfx_session_export_mp3(s, mp3.c_str(), 320) == 0, "export mp3: %s", vfx_last_error());
  CHECK(vfx_session_export_original_wav(s, orig.c_str()) == 0, "export original");
  CHECK(fileStartsWith(wav, "RIFF", 4), "wav header");
  {
    FILE* f = std::fopen(mp3.c_str(), "rb");
    unsigned char h[2] = {0, 0};
    long size = 0;
    if (f) { if (std::fread(h, 1, 2, f) != 2) h[0] = 0; std::fseek(f, 0, SEEK_END); size = std::ftell(f); std::fclose(f); }
    CHECK(h[0] == 0xFF && (h[1] & 0xE0) == 0xE0, "mp3 frame sync");
    // 320 kbps * ~4.6 s ~= 184 kB
    CHECK(size > 150000 && size < 260000, "mp3 size %ld", size);
  }
  std::vector<float> wf(100), det(100), cor(100);
  CHECK(vfx_session_waveform(s, 1, wf.data(), 100) == 0, "waveform");
  CHECK(vfx_session_pitch_curves(s, det.data(), cor.data(), 100) == 0, "pitch curves");
  vfx_session_close(s);

  CHECK(vfx_session_open_wav("/nonexistent.wav") == nullptr, "missing file must fail");
  CHECK(std::strlen(vfx_last_error()) > 0, "error message set");
}

void testPerformance() {
  std::printf("performance\n");
  auto x = vfxtest::synthVoice(kSr, 60.0, [](double t) { return 180.0 + 60.0 * std::sin(t); });
  vfx::Session s;
  s.sampleRate = kSr;
  s.input = x;
  auto t0 = std::chrono::steady_clock::now();
  s.analyze();
  auto t1 = std::chrono::steady_clock::now();
  s.render(vfx::presetParams(VFX_PRESET_POP));
  auto t2 = std::chrono::steady_clock::now();
  std::printf("  60 s of audio: analyse %.0f ms, render (pop) %.0f ms\n",
              std::chrono::duration<double, std::milli>(t1 - t0).count(),
              std::chrono::duration<double, std::milli>(t2 - t1).count());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string tmp = argc > 1 ? argv[1] : "/tmp";
  testPitchTracker();
  testKeyDetection();
  testScales();
  testPsolaIdentity();
  testPsolaShift();
  testHardTune();
  testHarmony();
  testLimiterAndLoudness();
  testIoAndPresets(tmp);
  testPerformance();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
