#include "renderer.hpp"

#include <algorithm>
#include <cmath>

#include "dsp.hpp"

namespace vfx {

namespace {

constexpr double kTargetLufs = -14.0;   // streaming-platform reference loudness
constexpr float kCeilingDb = -1.0f;     // keeps MP3 encoding free of clipping

vfx_params baseParams() {
  vfx_params p{};
  p.key_root = -1;
  p.scale = -1;
  p.correction = 1.0f;
  p.retune_ms = 20.0f;
  p.humanize = 0.3f;
  p.transpose = 0.0f;
  p.formant = 0.0f;
  p.robot = 0;
  p.harmony = 0.0f;
  p.harmony_mode = 0;
  p.doubler = 0.0f;
  p.reverb = 0.15f;
  p.reverb_size = 0.5f;
  p.delay = 0.0f;
  p.delay_ms = 320.0f;
  p.brightness = 0.3f;
  p.warmth = 0.0f;
  p.compression = 0.5f;
  p.deess = 0.5f;
  p.gate = 0.3f;
  p.normalize = 1;
  p.output_gain_db = 0.0f;
  return p;
}

float percentile(std::vector<float> v, double q) {
  if (v.empty()) return -120.0f;
  const size_t i = static_cast<size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<long>(i), v.end());
  return v[i];
}

// Adds a mono voice into a stereo bus with a pan (-1..1), gain and delay.
void mixVoice(Stereo& bus, const std::vector<float>& v, float gain, float pan, size_t delay) {
  const double theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0) * kPi / 4.0;
  const float gl = static_cast<float>(std::cos(theta) * std::sqrt(2.0)) * gain;
  const float gr = static_cast<float>(std::sin(theta) * std::sqrt(2.0)) * gain;
  for (size_t i = 0; i < v.size() && i + delay < bus.size(); ++i) {
    bus.l[i + delay] += gl * v[i];
    bus.r[i + delay] += gr * v[i];
  }
}

Key resolveKey(const vfx_params& p, const KeyEstimate& detected) {
  Key k = detected.key;
  if (p.key_root >= 0) k.root = p.key_root % 12;
  if (p.scale >= 0) {
    k.scale = static_cast<ScaleType>(std::clamp(p.scale, 0, 6));
  } else if (p.key_root >= 0 && k.scale == ScaleType::Chromatic) {
    k.scale = ScaleType::Major;  // root forced but nothing detected: assume major
  }
  return k;
}

void master(Stereo& s, double sr, bool normalize, float extraGainDb, double& lufsOut) {
  float gainDb = extraGainDb;
  if (normalize) {
    const double l = integratedLoudness(s, sr);
    if (l > -100.0) gainDb += static_cast<float>(std::clamp(kTargetLufs - l, -20.0, 36.0));
  }
  const float g = dbToGain(gainDb);
  for (size_t i = 0; i < s.size(); ++i) { s.l[i] *= g; s.r[i] *= g; }
  limit(s, sr, kCeilingDb);
  lufsOut = integratedLoudness(s, sr);
}

}  // namespace

vfx_params presetParams(int preset) {
  vfx_params p = baseParams();
  switch (preset) {
    case VFX_PRESET_NATURAL:  // transparent correction, keeps the singer's expression
      p.correction = 0.85f; p.retune_ms = 70.0f; p.humanize = 0.7f;
      p.reverb = 0.14f; p.reverb_size = 0.4f; p.compression = 0.45f; p.brightness = 0.2f;
      break;
    case VFX_PRESET_POP:  // tight modern pop vocal
      p.correction = 1.0f; p.retune_ms = 18.0f; p.humanize = 0.35f;
      p.doubler = 0.22f; p.reverb = 0.2f; p.reverb_size = 0.55f;
      p.delay = 0.1f; p.delay_ms = 330.0f; p.brightness = 0.55f; p.compression = 0.65f;
      break;
    case VFX_PRESET_HARDTUNE:  // the famous robotic, stepped "auto-tune" effect
      p.correction = 1.0f; p.retune_ms = 0.0f; p.humanize = 0.0f;
      p.reverb = 0.16f; p.reverb_size = 0.5f; p.delay = 0.12f; p.delay_ms = 300.0f;
      p.brightness = 0.45f; p.compression = 0.65f;
      break;
    case VFX_PRESET_CHOIR:  // tuned lead + automatic in-key harmonies
      p.correction = 1.0f; p.retune_ms = 15.0f; p.humanize = 0.3f;
      p.harmony = 0.6f; p.harmony_mode = 3; p.doubler = 0.25f;
      p.reverb = 0.3f; p.reverb_size = 0.8f; p.brightness = 0.35f; p.compression = 0.55f;
      break;
    case VFX_PRESET_ROBOT:  // monotone vocoder-like robot
      p.correction = 1.0f; p.retune_ms = 0.0f; p.humanize = 0.0f; p.robot = 1; p.formant = -1.0f;
      p.delay = 0.15f; p.delay_ms = 250.0f; p.reverb = 0.12f; p.compression = 0.7f; p.brightness = 0.2f;
      break;
    case VFX_PRESET_DEEP:  // lower, bigger voice
      p.correction = 0.7f; p.retune_ms = 40.0f; p.transpose = -5.0f; p.formant = -3.0f;
      p.reverb = 0.15f; p.warmth = 0.4f; p.brightness = 0.0f;
      break;
    case VFX_PRESET_HIGH:  // higher, lighter voice
      p.correction = 0.7f; p.retune_ms = 40.0f; p.transpose = 6.0f; p.formant = 3.0f;
      p.reverb = 0.12f; p.warmth = -0.3f; p.brightness = 0.3f;
      break;
    case VFX_PRESET_STUDIO:  // no pitch correction, just a polished vocal chain
      p.correction = 0.0f; p.humanize = 0.0f;
      p.reverb = 0.16f; p.reverb_size = 0.45f; p.brightness = 0.4f; p.compression = 0.55f;
      break;
    default:
      break;
  }
  return p;
}

void Session::analyze() {
  track = trackPitch(input, sampleRate);
  marks = computePitchMarks(input, track);
  key = detectKey(track);
  analyzed = true;
}

void Session::render(const vfx_params& p) {
  if (!analyzed) analyze();
  const double sr = sampleRate;
  const size_t n = input.size();

  // --- 1. Clean-up: rumble filter, auto level, soft noise gate -------------
  std::vector<float> pre = input;
  {
    Biquad h1, h2;
    h1.setup(Biquad::Type::HighPass, sr, 70.0, 0.5411961);
    h2.setup(Biquad::Type::HighPass, sr, 70.0, 1.3065630);
    for (auto& v : pre) v = h2.process(h1.process(v));
  }
  std::vector<float> voicedDb, allDb = track.rmsDb;
  for (size_t i = 0; i < track.frames(); ++i)
    if (track.f0[i] > 0.0f) voicedDb.push_back(track.rmsDb[i]);
  const float voiceLevel = voicedDb.size() > 20 ? percentile(voicedDb, 0.7) : percentile(allDb, 0.9);
  const float autoGainDb = std::clamp(-18.0f - voiceLevel, -12.0f, 30.0f);
  {
    const float g = dbToGain(autoGainDb);
    for (auto& v : pre) v *= g;
  }
  if (p.gate > 0.0f) {
    const float noise = percentile(allDb, 0.1) + autoGainDb;
    const float voice = voiceLevel + autoGainDb;
    const float thr = std::min(noise + 6.0f + 8.0f * p.gate, voice - 18.0f);
    noiseGate(pre, sr, thr, 6.0f + 24.0f * p.gate);
  }

  // --- 2. Pitch correction --------------------------------------------------
  const Key key_ = resolveKey(p, key);
  CorrectionParams cp;
  cp.key = key_;
  cp.amount = std::clamp(p.correction, 0.0f, 1.0f);
  cp.retuneMs = std::clamp(p.retune_ms, 0.0f, 400.0f);
  cp.humanize = std::clamp(p.humanize, 0.0f, 1.0f);
  cp.transpose = std::clamp(p.transpose, -12.0f, 12.0f);
  cp.robot = p.robot != 0;
  const Correction corr = correctPitch(track, cp);
  const float formant = std::clamp(p.formant, -6.0f, 6.0f);

  const bool pitchChange = cp.amount > 0.0f || cp.robot || std::fabs(cp.transpose) > 1e-3f;
  std::vector<float> lead;
  if (pitchChange || std::fabs(formant) > 1e-3f) {
    lead = psolaResynthesize(pre, track, marks, pitchChange ? corr.hz : std::vector<float>{}, formant);
  } else {
    lead = pre;
  }
  correctedHz = pitchChange ? corr.hz : track.f0;
  const std::vector<float>& leadPitch = correctedHz;

  // --- 3. Voices: lead, harmonies, doubles -----------------------------------
  float tailSec = 0.15f;
  const float reverbDecay = 0.8f + 2.6f * std::clamp(p.reverb_size, 0.0f, 1.0f);
  if (p.reverb > 0.0f) tailSec += 0.4f + reverbDecay;
  if (p.delay > 0.0f) tailSec += 1.5f;
  tailSec = std::min(tailSec, 6.0f);
  Stereo dry(n + static_cast<size_t>(tailSec * sr));
  mixVoice(dry, lead, 1.0f, 0.0f, 0);
  std::vector<float>().swap(lead);

  if (p.harmony > 0.0f) {
    Key hk = key_;
    if (hk.scale == ScaleType::Chromatic)
      hk = key.key.scale != ScaleType::Chromatic ? key.key : Key{key_.root, ScaleType::Major};
    struct Voice { int degrees; float semis; float pan; float formant; size_t delayMs; };
    std::vector<Voice> voices;
    switch (p.harmony_mode) {
      case 0: voices = {{2, 0, 0.35f, 0.0f, 12}}; break;
      case 1: voices = {{2, 0, -0.5f, 0.0f, 12}, {4, 0, 0.5f, 0.0f, 18}}; break;
      case 2: voices = {{0, -12.0f, 0.0f, 0.0f, 10}}; break;
      default: voices = {{-3, 0, -0.6f, -0.4f, 15}, {2, 0, 0.6f, 0.3f, 11}, {4, 0, 0.15f, 0.2f, 20}}; break;
    }
    // Harmonies follow the corrected melody, so build a correction for them
    // even when the lead itself is left untouched.
    Correction ref = corr;
    if (!pitchChange) {
      CorrectionParams hp = cp;
      hp.amount = 1.0f; hp.retuneMs = 30.0f;
      ref = correctPitch(track, hp);
    }
    const float level = std::clamp(p.harmony, 0.0f, 1.0f) * 0.62f / std::sqrt(static_cast<float>(voices.size()));
    for (const auto& v : voices) {
      std::vector<float> target = diatonicShift(ref, hk, v.degrees);
      if (v.semis != 0.0f) for (auto& t : target) t *= std::pow(2.0f, v.semis / 12.0f);
      const auto voice = psolaResynthesize(pre, track, marks, target, formant + v.formant);
      mixVoice(dry, voice, level, v.pan, static_cast<size_t>(v.delayMs * 0.001 * sr));
    }
  }

  if (p.doubler > 0.0f) {
    const float level = std::clamp(p.doubler, 0.0f, 1.0f) * 0.5f;
    const float cents[2] = {-9.0f, 8.0f};
    const float pans[2] = {-0.85f, 0.85f};
    const size_t delays[2] = {17, 29};
    for (int i = 0; i < 2; ++i) {
      std::vector<float> target = leadPitch;
      const float r = std::pow(2.0f, cents[i] / 1200.0f);
      for (auto& t : target) t *= r;
      const auto voice = psolaResynthesize(pre, track, marks, target, formant);
      mixVoice(dry, voice, level, pans[i], static_cast<size_t>(delays[i] * 0.001 * sr));
    }
  }

  // --- 4. Channel strip --------------------------------------------------------
  deEss(dry, sr, std::clamp(p.deess, 0.0f, 1.0f));
  const float bright = std::clamp(p.brightness, -1.0f, 1.0f);
  vocalEq(dry, sr, 80.0f, 3.0f * std::clamp(p.warmth, -1.0f, 1.0f), 1.0f + 2.5f * bright, 1.5f + 3.5f * bright);
  const float comp = std::clamp(p.compression, 0.0f, 1.0f);
  if (comp > 0.0f) compress(dry, sr, -12.0f - 16.0f * comp, 1.5f + 3.0f * comp, 4.0f, 80.0f);

  // --- 5. Space: echo + reverb sends (streaming, in place) ---------------------
  if (p.delay > 0.0f || p.reverb > 0.0f) {
    const float size = std::clamp(p.reverb_size, 0.0f, 1.0f);
    const float echoGain = 0.55f * std::clamp(p.delay, 0.0f, 1.0f);
    const float verbGain = 1.1f * std::clamp(p.reverb, 0.0f, 1.0f);
    PingPongDelay echo(sr, std::clamp(p.delay_ms, 40.0f, 1200.0f), 0.25f + 0.35f * p.delay, 5000.0f);
    FdnReverb verb(sr, size, reverbDecay, 0.45f, 15.0f + 25.0f * size);
    for (size_t i = 0; i < dry.size(); ++i) {
      const float l = dry.l[i], r = dry.r[i];
      float el = 0.0f, er = 0.0f, wl = 0.0f, wr = 0.0f;
      if (echoGain > 0.0f) echo.process(l, r, el, er);
      // The echo also feeds the reverb so repeats sit in the same space.
      if (verbGain > 0.0f) verb.process(l + 0.5f * el, r + 0.5f * er, wl, wr);
      dry.l[i] = l + echoGain * el + verbGain * wl;
      dry.r[i] = r + echoGain * er + verbGain * wr;
    }
  }
  Stereo& mix = dry;

  // --- 6. Mastering --------------------------------------------------------------
  master(mix, sr, p.normalize != 0, p.output_gain_db, outputLufs);

  // Trim the silent part of the tail and fade the very end.
  size_t end = mix.size();
  const float floor = dbToGain(-75.0f);
  while (end > n && std::fabs(mix.l[end - 1]) < floor && std::fabs(mix.r[end - 1]) < floor) --end;
  end = std::min(mix.size(), end + static_cast<size_t>(0.05 * sr));
  mix.l.resize(end);
  mix.r.resize(end);
  const size_t fade = std::min(end, static_cast<size_t>(0.02 * sr));
  for (size_t i = 0; i < fade; ++i) {
    const float g = static_cast<float>(i) / static_cast<float>(fade);
    mix.l[end - 1 - i] *= g;
    mix.r[end - 1 - i] *= g;
  }

  output = std::move(dry);
  rendered = true;
}

Stereo Session::loudnessMatchedOriginal() const {
  Stereo s(input.size());
  s.l = input;
  s.r = input;
  const double target = rendered ? outputLufs : kTargetLufs;
  const double l = integratedLoudness(s, sampleRate);
  if (l > -100.0) {
    const float g = dbToGain(static_cast<float>(std::clamp(target - l, -20.0, 36.0)));
    for (size_t i = 0; i < s.size(); ++i) { s.l[i] *= g; s.r[i] *= g; }
  }
  limit(s, sampleRate, kCeilingDb);
  return s;
}

}  // namespace vfx
