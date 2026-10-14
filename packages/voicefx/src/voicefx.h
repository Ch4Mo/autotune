/*
 * VoiceFX - offline vocal processing engine (pitch correction + vocal chain).
 * Plain C API so it can be used from Dart FFI, Swift, Kotlin/JNI or C++.
 *
 * Typical flow:
 *   s = vfx_session_open_wav("take.wav");
 *   vfx_session_analyze(s);                  // pitch tracking + key detection (once)
 *   vfx_get_preset(VFX_PRESET_POP, &p);
 *   vfx_session_render(s, &p);               // fast, repeatable with new params
 *   vfx_session_export_wav(s, "out.wav", 24) / vfx_session_export_mp3(s, "out.mp3", 320);
 *   vfx_session_close(s);
 *
 * A session must not be used from two threads at the same time.
 */
#ifndef VOICEFX_H
#define VOICEFX_H

#include <stdint.h>

#if defined(_WIN32)
#define VFX_API __declspec(dllexport)
#else
#define VFX_API __attribute__((visibility("default"))) __attribute__((used))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vfx_session vfx_session;

/* Keep in sync with lib/src/bindings.dart (field order and types matter). */
typedef struct vfx_params {
  int32_t key_root;     /* 0 = C ... 11 = B, -1 = use detected key */
  int32_t scale;        /* 0 chromatic, 1 major, 2 minor, 3 harmonic minor,
                           4 major pentatonic, 5 minor pentatonic, 6 blues, -1 = detected */
  float correction;     /* 0..1  amount of pitch correction */
  float retune_ms;      /* 0 (hard / robotic) .. 200 (very gentle) */
  float humanize;       /* 0..1  keeps natural vibrato on long notes */
  float transpose;      /* semitones, -12..12 */
  float formant;        /* semitones, -6..6 (voice character, independent of pitch) */
  int32_t robot;        /* 1 = monotone robot voice */
  float harmony;        /* 0..1  level of generated harmonies (0 = off) */
  int32_t harmony_mode; /* 0 third above, 1 third+fifth above, 2 octave below, 3 choir */
  float doubler;        /* 0..1  stereo double-tracking */
  float reverb;         /* 0..1  reverb amount */
  float reverb_size;    /* 0..1  small room .. large hall */
  float delay;          /* 0..1  echo amount */
  float delay_ms;       /* echo time */
  float brightness;     /* -1..1 */
  float warmth;         /* -1..1 */
  float compression;    /* 0..1 */
  float deess;          /* 0..1 */
  float gate;           /* 0..1  background-noise reduction between phrases */
  int32_t normalize;    /* 1 = loudness-normalise to -14 LUFS, true-peak safe */
  float output_gain_db; /* extra gain, applied before the final limiter */
  int32_t use_backing;  /* 1 = mix the instrumental (if one is loaded) into the render */
  float vocal_balance;  /* -1 (music louder) .. 1 (voice louder), 0 = balanced */
  float backing_offset_ms; /* > 0 delays the voice against the instrumental */
} vfx_params;

enum {
  VFX_PRESET_NATURAL = 0,
  VFX_PRESET_POP = 1,
  VFX_PRESET_HARDTUNE = 2,
  VFX_PRESET_CHOIR = 3,
  VFX_PRESET_ROBOT = 4,
  VFX_PRESET_DEEP = 5,
  VFX_PRESET_HIGH = 6,
  VFX_PRESET_STUDIO = 7,
  VFX_PRESET_COUNT = 8
};

/* Fills `out` with the preset's settings. Returns 0 on success. */
VFX_API int32_t vfx_get_preset(int32_t preset, vfx_params* out);

/* Opens a WAV file. Returns NULL on error (see vfx_last_error). */
VFX_API vfx_session* vfx_session_open_wav(const char* path);
VFX_API void vfx_session_close(vfx_session* s);

VFX_API double vfx_session_sample_rate(const vfx_session* s);
VFX_API double vfx_session_duration(const vfx_session* s);

/* Pitch tracking + key detection. Needed once before rendering. */
VFX_API int32_t vfx_session_analyze(vfx_session* s);
/* Detected key (after analyze). confidence 0..1. scale is 0 when unknown. */
VFX_API int32_t vfx_session_detected_key(const vfx_session* s, int32_t* root, int32_t* scale, float* confidence);

/* Loads an instrumental (MP3 or WAV) to sing over. It is resampled to the
 * take's rate, its key is detected and used for pitch correction. */
VFX_API int32_t vfx_session_set_backing(vfx_session* s, const char* path);
VFX_API void vfx_session_clear_backing(vfx_session* s);
VFX_API double vfx_session_backing_duration(const vfx_session* s); /* 0 = none */
/* Key detected in the instrumental. Returns -1 when no instrumental is loaded. */
VFX_API int32_t vfx_session_backing_key(const vfx_session* s, int32_t* root, int32_t* scale, float* confidence);

/* Renders the processed result into the session (replaces the previous one). */
VFX_API int32_t vfx_session_render(vfx_session* s, const vfx_params* p);
VFX_API double vfx_session_rendered_duration(const vfx_session* s);

/* Exports the last render. bits: 16 (dithered), 24, 32 (float). kbps: 128..320. */
VFX_API int32_t vfx_session_export_wav(vfx_session* s, const char* path, int32_t bits);
VFX_API int32_t vfx_session_export_mp3(vfx_session* s, const char* path, int32_t kbps);

/* Writes the unprocessed take (mixed with the instrumental like the render),
 * loudness-matched to the render, for fair A/B listening. */
VFX_API int32_t vfx_session_export_original_wav(vfx_session* s, const char* path);

/* Peak envelope for drawing: `count` values in 0..1. which: 0 = original, 1 = render. */
VFX_API int32_t vfx_session_waveform(const vfx_session* s, int32_t which, float* out, int32_t count);

/* Pitch curves for display, resampled to `count` points, as MIDI note numbers
 * (0 = silence). `detected` and/or `corrected` may be NULL. */
VFX_API int32_t vfx_session_pitch_curves(const vfx_session* s, float* detected, float* corrected, int32_t count);

/* Message describing the last error on the calling thread. */
VFX_API const char* vfx_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICEFX_H */
