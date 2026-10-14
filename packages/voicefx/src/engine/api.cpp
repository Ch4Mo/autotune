// C API wrapper around vfx::Session. No exception ever crosses this boundary.
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>

#include "../voicefx.h"
#include "audio_io.hpp"
#include "dsp.hpp"
#include "api_internal.hpp"

namespace {

thread_local std::string g_lastError;

template <typename F>
int32_t guarded(F&& f) {
  try {
    f();
    g_lastError.clear();
    return 0;
  } catch (const std::exception& e) {
    g_lastError = e.what();
  } catch (...) {
    g_lastError = "unknown error";
  }
  return -1;
}

int32_t fail(const char* msg) {
  g_lastError = msg;
  return -1;
}

}  // namespace

extern "C" {

int32_t vfx_get_preset(int32_t preset, vfx_params* out) {
  if (!out) return fail("null params");
  if (preset < 0 || preset >= VFX_PRESET_COUNT) return fail("unknown preset");
  *out = vfx::presetParams(preset);
  return 0;
}

vfx_session* vfx_session_open_wav(const char* path) {
  if (!path) { fail("null path"); return nullptr; }
  vfx_session* s = nullptr;
  const int32_t rc = guarded([&] {
    vfx::MonoAudio a = vfx::readWav(path);
    if (a.samples.empty()) throw std::runtime_error("the recording is empty");
    if (a.sampleRate < 8000 || a.sampleRate > 192000) throw std::runtime_error("unsupported sample rate");
    s = new vfx_session();
    s->impl.sampleRate = a.sampleRate;
    s->impl.input = std::move(a.samples);
  });
  return rc == 0 ? s : nullptr;
}

void vfx_session_close(vfx_session* s) { delete s; }

double vfx_session_sample_rate(const vfx_session* s) { return s ? s->impl.sampleRate : 0.0; }

double vfx_session_duration(const vfx_session* s) {
  return s ? static_cast<double>(s->impl.input.size()) / s->impl.sampleRate : 0.0;
}

int32_t vfx_session_analyze(vfx_session* s) {
  if (!s) return fail("null session");
  return guarded([&] { s->impl.analyze(); });
}

int32_t vfx_session_detected_key(const vfx_session* s, int32_t* root, int32_t* scale, float* confidence) {
  if (!s) return fail("null session");
  if (!s->impl.analyzed) return fail("not analysed yet");
  if (root) *root = s->impl.key.key.root;
  if (scale) *scale = static_cast<int32_t>(s->impl.key.key.scale);
  if (confidence) *confidence = s->impl.key.confidence;
  return 0;
}

int32_t vfx_session_noise_info(const vfx_session* s, float* snr_db, float* hum_hz) {
  if (!s) return fail("null session");
  if (!s->impl.analyzed) return fail("not analysed yet");
  if (snr_db) *snr_db = std::max(0.0f, s->impl.voiceLevelDb - s->impl.noise.noiseFloorDb);
  if (hum_hz) *hum_hz = s->impl.noise.humHz;
  return 0;
}

int32_t vfx_session_set_backing(vfx_session* s, const char* path) {
  if (!s || !path) return fail("null argument");
  return guarded([&] { s->impl.setBacking(path); });
}

void vfx_session_clear_backing(vfx_session* s) {
  if (s) s->impl.clearBacking();
}

double vfx_session_backing_duration(const vfx_session* s) {
  if (!s || !s->impl.hasBacking) return 0.0;
  return static_cast<double>(s->impl.backing.size()) / s->impl.sampleRate;
}

int32_t vfx_session_backing_key(const vfx_session* s, int32_t* root, int32_t* scale, float* confidence) {
  if (!s) return fail("null session");
  if (!s->impl.hasBacking) return fail("no instrumental loaded");
  if (root) *root = s->impl.backingKey.key.root;
  if (scale) *scale = static_cast<int32_t>(s->impl.backingKey.key.scale);
  if (confidence) *confidence = s->impl.backingKey.confidence;
  return 0;
}

int32_t vfx_session_render(vfx_session* s, const vfx_params* p) {
  if (!s || !p) return fail("null argument");
  return guarded([&] { s->impl.render(*p); });
}

double vfx_session_rendered_duration(const vfx_session* s) {
  if (!s || !s->impl.rendered) return 0.0;
  return static_cast<double>(s->impl.output.size()) / s->impl.sampleRate;
}

int32_t vfx_session_export_wav(vfx_session* s, const char* path, int32_t bits) {
  if (!s || !path) return fail("null argument");
  if (!s->impl.rendered) return fail("nothing rendered yet");
  return guarded([&] { vfx::writeWav(path, s->impl.output, s->impl.sampleRate, bits); });
}

int32_t vfx_session_export_mp3(vfx_session* s, const char* path, int32_t kbps) {
  if (!s || !path) return fail("null argument");
  if (!s->impl.rendered) return fail("nothing rendered yet");
  return guarded([&] { vfx::writeMp3(path, s->impl.output, s->impl.sampleRate, std::clamp(kbps, 96, 320)); });
}

int32_t vfx_session_export_original_wav(vfx_session* s, const char* path) {
  if (!s || !path) return fail("null argument");
  return guarded([&] { vfx::writeWav(path, s->impl.loudnessMatchedOriginal(), s->impl.sampleRate, 16); });
}

int32_t vfx_session_waveform(const vfx_session* s, int32_t which, float* out, int32_t count) {
  if (!s || !out || count <= 0) return fail("bad argument");
  const auto& impl = s->impl;
  if (which == 1 && !impl.rendered) return fail("nothing rendered yet");
  const size_t n = which == 1 ? impl.output.size() : impl.input.size();
  float peakAll = 1e-9f;
  for (int32_t b = 0; b < count; ++b) {
    const size_t a = n * static_cast<size_t>(b) / static_cast<size_t>(count);
    const size_t e = std::max(a + 1, n * static_cast<size_t>(b + 1) / static_cast<size_t>(count));
    float pk = 0.0f;
    for (size_t i = a; i < e && i < n; ++i) {
      const float v = which == 1 ? std::max(std::fabs(impl.output.l[i]), std::fabs(impl.output.r[i]))
                                 : std::fabs(impl.input[i]);
      pk = std::max(pk, v);
    }
    out[b] = pk;
    peakAll = std::max(peakAll, pk);
  }
  for (int32_t b = 0; b < count; ++b) out[b] /= peakAll;
  return 0;
}

int32_t vfx_session_pitch_curves(const vfx_session* s, float* detected, float* corrected, int32_t count) {
  if (!s || count <= 0) return fail("bad argument");
  const auto& impl = s->impl;
  if (!impl.analyzed) return fail("not analysed yet");
  const size_t frames = impl.track.frames();
  for (int32_t b = 0; b < count; ++b) {
    const size_t a = frames * static_cast<size_t>(b) / static_cast<size_t>(count);
    const size_t e = std::max(a + 1, frames * static_cast<size_t>(b + 1) / static_cast<size_t>(count));
    auto avg = [&](const std::vector<float>& c) {
      double acc = 0; int cnt = 0;
      for (size_t i = a; i < e && i < c.size(); ++i)
        if (c[i] > 0.0f) { acc += vfx::hzToMidi(c[i]); ++cnt; }
      return cnt > 0 && cnt * 2 >= static_cast<int>(e - a) ? static_cast<float>(acc / cnt) : 0.0f;
    };
    if (detected) detected[b] = avg(impl.track.f0);
    if (corrected) corrected[b] = impl.rendered ? avg(impl.correctedHz) : 0.0f;
  }
  return 0;
}

const char* vfx_last_error(void) { return g_lastError.c_str(); }

}  // extern "C"
