// Engine unit tests (desktop). Build: cmake -S src -B build -DVOICEFX_BUILD_TESTS=ON
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "engine/api_internal.hpp"
#include "engine/audio_io.hpp"
#include "engine/denoise.hpp"
#include "engine/dsp.hpp"
#include "engine/effects.hpp"
#include "engine/pitch_tracker.hpp"
#include "engine/psola.hpp"
#include "engine/renderer.hpp"
#include "engine/resample.hpp"
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

// Polyphonic "instrumental": chords with harmonics, bass and noise hits.
vfx::Stereo synthBacking(double sr, double seconds, const std::vector<std::vector<int>>& chords, double chordLen) {
  const size_t n = static_cast<size_t>(sr * seconds);
  vfx::Stereo s(n);
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    const auto& ch = chords[static_cast<size_t>(t / chordLen) % chords.size()];
    double v = 0;
    for (int note : ch)
      for (int h = 1; h <= 4; ++h) v += std::sin(2 * vfx::kPi * vfx::midiToHz(note) * h * t) / (h * 1.5);
    v += 0.8 * std::sin(2 * vfx::kPi * vfx::midiToHz(ch[0] - 12) * t);  // bass
    const double beat = std::fmod(t, 0.5);
    if (beat < 0.03) v += 2.0 * uni(rng) * (1 - beat / 0.03);  // hi-hat-ish noise
    s.l[i] = s.r[i] = static_cast<float>(0.05 * v);
  }
  return s;
}

void testResample() {
  std::printf("resample\n");
  const double inSr = 44100, outSr = 48000, f = 1000;
  std::vector<float> x(44100);
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2 * vfx::kPi * f * i / inSr));
  auto y = vfx::resample(x, inSr, outSr);
  CHECK(std::abs(static_cast<long>(y.size()) - 48000) <= 1, "length %zu", y.size());
  double err = 0, sig = 0;
  for (size_t i = 2000; i + 2000 < y.size(); ++i) {
    const double ref = 0.5 * std::sin(2 * vfx::kPi * f * i / outSr);
    err += (y[i] - ref) * (y[i] - ref);
    sig += ref * ref;
  }
  const double snr = 10 * std::log10(sig / err);
  CHECK(snr > 70.0, "resample SNR %.1f dB", snr);
  // Down-sampling must remove content above the new Nyquist (no aliasing).
  std::vector<float> hf(48000);
  for (size_t i = 0; i < hf.size(); ++i) hf[i] = static_cast<float>(0.5 * std::sin(2 * vfx::kPi * 23000 * i / 48000.0));
  auto d = vfx::resample(hf, 48000, 44100);
  double e = 0;
  for (size_t i = 2000; i + 2000 < d.size(); ++i) e += d[i] * d[i];
  const double db = 10 * std::log10(e / (d.size() - 4000) / 0.125);
  CHECK(db < -60.0, "aliasing %.1f dB", db);
}

void testBacking(const std::string& tmpDir) {
  std::printf("instrumental\n");
  // E minor progression: Em - C - G - D.
  const std::vector<std::vector<int>> chords{{64, 67, 71}, {60, 64, 67}, {67, 71, 74}, {62, 66, 69}};
  auto back = synthBacking(44100, 8.0, chords, 1.0);
  const std::string mp3 = tmpDir + "/vfx_backing.mp3";
  vfx::writeMp3(mp3, back, 44100, 192);
  auto dec = vfx::readAudioFile(mp3);
  CHECK(dec.sampleRate == 44100, "mp3 rate %.0f", dec.sampleRate);
  CHECK(std::fabs(static_cast<double>(dec.samples.size()) - back.size()) < 44100 * 0.1, "mp3 length %zu", dec.samples.size());

  std::vector<float> mono(dec.samples.l);
  auto k = vfx::detectKeyFromAudio(mono, 44100);
  // G major and E minor share the same notes; either answer is musically right.
  const bool ok = (k.key.root == 4 && k.key.scale == vfx::ScaleType::Minor) ||
                  (k.key.root == 7 && k.key.scale == vfx::ScaleType::Major);
  CHECK(ok, "instrumental key root=%d scale=%d conf=%.2f", k.key.root, static_cast<int>(k.key.scale), k.confidence);

  // Voice at 48 kHz over a 44.1 kHz MP3: resampled, mixed, offset applied.
  const std::string voice = tmpDir + "/vfx_voice48.wav";
  vfx::writeWavMono(voice, vfxtest::synthVoice(48000.0, 4.0, [](double t) { return t < 3.5 ? vfx::midiToHz(64.3) : 0.0; }),
                    48000.0, 16);
  vfx_session* s = vfx_session_open_wav(voice.c_str());
  CHECK(s && vfx_session_analyze(s) == 0, "open voice");
  if (!s) return;
  CHECK(vfx_session_set_backing(s, mp3.c_str()) == 0, "set backing: %s", vfx_last_error());
  CHECK(std::fabs(vfx_session_backing_duration(s) - 8.0) < 0.1, "backing duration %.2f", vfx_session_backing_duration(s));
  int32_t root = -1, scale = -1;
  float conf = 0;
  CHECK(vfx_session_backing_key(s, &root, &scale, &conf) == 0, "backing key");

  vfx_params p;
  vfx_get_preset(VFX_PRESET_POP, &p);
  CHECK(vfx_session_render(s, &p) == 0, "render with backing: %s", vfx_last_error());
  const double withBacking = vfx_session_rendered_duration(s);
  CHECK(std::fabs(withBacking - 8.0) < 0.2, "mix lasts as long as the instrumental (%.2f)", withBacking);
  const auto& out = s->impl.output;
  float pk = 0;
  for (size_t i = 0; i < out.size(); ++i) pk = std::max(pk, std::max(std::fabs(out.l[i]), std::fabs(out.r[i])));
  CHECK(pk <= vfx::dbToGain(-1.0f) + 1e-4f, "mix peak %.3f", pk);

  // The voice offset moves the voice on the timeline: with a +1 s offset the
  // first second only contains the instrumental -> same samples as voice muted.
  p.backing_offset_ms = 1000.0f;
  p.reverb = 0.0f; p.delay = 0.0f; p.doubler = 0.0f; p.normalize = 0;
  vfx_session_render(s, &p);
  vfx::Stereo a = s->impl.output;
  p.vocal_balance = -1.0f;  // voice quieter: the first second must not change
  vfx_session_render(s, &p);
  const auto& b = s->impl.output;
  double diffFirst = 0, diffLater = 0;
  for (size_t i = 0; i < 44000; ++i) diffFirst += std::fabs(a.l[i] - b.l[i]);
  for (size_t i = 60000; i < 140000; ++i) diffLater += std::fabs(a.l[i] - b.l[i]);
  CHECK(diffFirst < 1e-3 && diffLater > 1.0, "offset placement first=%.4f later=%.2f", diffFirst, diffLater);

  // Original A/B file includes the instrumental too.
  const std::string orig = tmpDir + "/vfx_orig_backing.wav";
  CHECK(vfx_session_export_original_wav(s, orig.c_str()) == 0, "original with backing");
  CHECK(vfx::readWav(orig).samples.size() >= static_cast<size_t>(7.9 * 48000), "original has the instrumental length");

  // Mixing can be switched off; clearing removes it.
  p.use_backing = 0;
  vfx_session_render(s, &p);
  CHECK(vfx_session_rendered_duration(s) < 5.0, "voice only render %.2f", vfx_session_rendered_duration(s));
  vfx_session_clear_backing(s);
  CHECK(vfx_session_backing_duration(s) == 0.0, "cleared");
  CHECK(vfx_session_set_backing(s, "/nonexistent.mp3") != 0, "missing instrumental must fail");
  vfx_session_close(s);
}

void testDenoise() {
  std::printf("denoise\n");
  const double sr = 48000;
  // Phrases with pauses, like a real take.
  auto clean = vfxtest::synthVoice(sr, 6.0, [](double t) {
    const double ph = std::fmod(t, 1.5);
    return ph < 1.1 ? 200.0 + 30.0 * std::sin(t * 2.0) : 0.0;
  }, 0.0);
  // Old-phone noise: hiss (-45 dBFS) + 50 Hz mains buzz with harmonics.
  std::vector<float> noise(clean.size());
  std::mt19937 rng(11);
  std::normal_distribution<float> g(0.0f, 1.0f);
  for (size_t i = 0; i < noise.size(); ++i) {
    const double t = i / sr;
    double hum = 0;
    for (int h = 1; h <= 5; ++h) hum += std::sin(2 * vfx::kPi * 50.0 * h * t) / h;
    noise[i] = 0.0056f * g(rng) + static_cast<float>(0.004 * hum);
  }
  std::vector<float> noisy(clean.size());
  for (size_t i = 0; i < noisy.size(); ++i) noisy[i] = clean[i] + noise[i];

  auto info = vfx::analyseNoise(noisy, sr);
  CHECK(info.humHz == 50.0f, "hum detected %.0f", info.humHz);
  CHECK(vfx::analyseNoise(clean, sr).humHz == 0.0f, "no hum in clean take");

  auto unchanged = noisy;
  vfx::denoise(unchanged, sr, 0.0f);
  CHECK(unchanged == noisy, "strength 0 must not touch the audio");

  auto out = noisy;
  const auto t0 = std::chrono::steady_clock::now();
  vfx::denoise(out, sr, 0.8f);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  // Residual noise in the pauses (1.2..1.45 s of each 1.5 s cycle).
  auto pauseDb = [&](const std::vector<float>& v) {
    double e = 0; size_t c = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      const double ph = std::fmod(i / sr, 1.5);
      if (ph > 1.2 && ph < 1.45) { e += v[i] * v[i]; ++c; }
    }
    return 10 * std::log10(e / c + 1e-20);
  };
  // Error against the clean voice while singing (how much noise is left + damage).
  auto voiceErrDb = [&](const std::vector<float>& v) {
    double e = 0, s = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      const double ph = std::fmod(i / sr, 1.5);
      if (ph > 0.1 && ph < 1.0) { e += (v[i] - clean[i]) * (v[i] - clean[i]); s += clean[i] * clean[i]; }
    }
    return 10 * std::log10(s / e);
  };
  const double before = pauseDb(noisy), after = pauseDb(out);
  std::printf("  pauses %.1f -> %.1f dBFS, voice SNR %.1f -> %.1f dB, %.0f ms for 6 s\n", before, after,
              voiceErrDb(noisy), voiceErrDb(out), ms);
  CHECK(before - after > 15.0, "noise in pauses reduced by %.1f dB", before - after);
  CHECK(voiceErrDb(out) > voiceErrDb(noisy) + 3.0, "voice SNR improved");
  double ev = 0, eo = 0;
  for (size_t i = 0; i < out.size(); ++i) { ev += clean[i] * clean[i]; eo += out[i] * out[i]; }
  CHECK(std::fabs(10 * std::log10(eo / ev)) < 1.5, "voice level kept (%.2f dB)", 10 * std::log10(eo / ev));

  // Through the API: noise info + render with cleaning.
  vfx::Session s;
  s.sampleRate = sr;
  s.input = noisy;
  s.analyze();
  const float snr = s.voiceLevelDb - s.noise.noiseFloorDb;
  CHECK(snr < 40.0f && s.noise.humHz == 50.0f, "noisy take diagnosed (snr %.1f)", snr);
  auto p = vfx::presetParams(VFX_PRESET_STUDIO);
  p.denoise = 0.7f;
  s.render(p);
  CHECK(s.output.size() > 0, "render with denoise");
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
  testResample();
  testBacking(tmp);
  testDenoise();
  testPerformance();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
