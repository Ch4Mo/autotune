/// High-level Dart API for the VoiceFX engine.
///
/// Every heavy call runs on a background isolate so the UI never stutters.
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'src/bindings.dart';

export 'src/bindings.dart' show VoiceFxBindings;

enum MusicalScale {
  chromatic,
  major,
  minor,
  harmonicMinor,
  majorPentatonic,
  minorPentatonic,
  blues,
}

class MusicalKey {
  const MusicalKey(this.root, this.scale);

  /// 0 = C ... 11 = B.
  final int root;
  final MusicalScale scale;

  @override
  bool operator ==(Object other) => other is MusicalKey && other.root == root && other.scale == scale;

  @override
  int get hashCode => Object.hash(root, scale);
}

class KeyDetection {
  const KeyDetection(this.key, this.confidence);
  final MusicalKey key;

  /// 0..1. Below ~0.3 the melody was too short or ambiguous.
  final double confidence;

  bool get isReliable => key.scale != MusicalScale.chromatic && confidence >= 0.3;
}

/// Ids match the VFX_PRESET_* constants of voicefx.h.
enum VoicePreset {
  natural(0),
  pop(1),
  hardTune(2),
  choir(3),
  robot(4),
  deep(5),
  high(6),
  studio(7);

  const VoicePreset(this.id);
  final int id;
}

enum HarmonyMode { thirdAbove, thirdAndFifth, octaveBelow, choir }

/// All the knobs of a render. Mirrors `vfx_params`.
class VoiceFxParams {
  const VoiceFxParams({
    this.keyRoot = -1,
    this.scale = -1,
    this.correction = 1.0,
    this.retuneMs = 20,
    this.humanize = 0.3,
    this.transpose = 0,
    this.formant = 0,
    this.robot = false,
    this.harmony = 0,
    this.harmonyMode = HarmonyMode.thirdAbove,
    this.doubler = 0,
    this.reverb = 0.15,
    this.reverbSize = 0.5,
    this.delay = 0,
    this.delayMs = 320,
    this.brightness = 0.3,
    this.warmth = 0,
    this.compression = 0.5,
    this.deess = 0.5,
    this.gate = 0.3,
    this.normalize = true,
    this.outputGainDb = 0,
    this.useBacking = true,
    this.vocalBalance = 0,
    this.backingOffsetMs = 0,
  });

  /// -1 = use the detected key.
  final int keyRoot;

  /// MusicalScale index, -1 = use the detected scale.
  final int scale;
  final double correction;
  final double retuneMs;
  final double humanize;
  final double transpose;
  final double formant;
  final bool robot;
  final double harmony;
  final HarmonyMode harmonyMode;
  final double doubler;
  final double reverb;
  final double reverbSize;
  final double delay;
  final double delayMs;
  final double brightness;
  final double warmth;
  final double compression;
  final double deess;
  final double gate;
  final bool normalize;
  final double outputGainDb;

  /// Mix the instrumental (when the session has one) into the render.
  final bool useBacking;

  /// -1 (music louder) .. 1 (voice louder).
  final double vocalBalance;

  /// Positive values delay the voice against the instrumental.
  final double backingOffsetMs;

  /// Settings of a built-in preset (read from the native engine).
  static VoiceFxParams preset(VoicePreset preset) {
    final b = VoiceFxBindings.instance;
    final p = calloc<VfxParamsStruct>();
    try {
      if (b.getPreset(preset.id, p) != 0) throw VoiceFxException(b.errorMessage());
      return VoiceFxParams._fromStruct(p.ref);
    } finally {
      calloc.free(p);
    }
  }

  factory VoiceFxParams._fromStruct(VfxParamsStruct s) => VoiceFxParams(
        keyRoot: s.keyRoot,
        scale: s.scale,
        correction: s.correction,
        retuneMs: s.retuneMs,
        humanize: s.humanize,
        transpose: s.transpose,
        formant: s.formant,
        robot: s.robot != 0,
        harmony: s.harmony,
        harmonyMode: HarmonyMode.values[s.harmonyMode.clamp(0, HarmonyMode.values.length - 1)],
        doubler: s.doubler,
        reverb: s.reverb,
        reverbSize: s.reverbSize,
        delay: s.delay,
        delayMs: s.delayMs,
        brightness: s.brightness,
        warmth: s.warmth,
        compression: s.compression,
        deess: s.deess,
        gate: s.gate,
        normalize: s.normalize != 0,
        outputGainDb: s.outputGainDb,
        useBacking: s.useBacking != 0,
        vocalBalance: s.vocalBalance,
        backingOffsetMs: s.backingOffsetMs,
      );

  void _writeTo(VfxParamsStruct s) {
    s
      ..keyRoot = keyRoot
      ..scale = scale
      ..correction = correction
      ..retuneMs = retuneMs
      ..humanize = humanize
      ..transpose = transpose
      ..formant = formant
      ..robot = robot ? 1 : 0
      ..harmony = harmony
      ..harmonyMode = harmonyMode.index
      ..doubler = doubler
      ..reverb = reverb
      ..reverbSize = reverbSize
      ..delay = delay
      ..delayMs = delayMs
      ..brightness = brightness
      ..warmth = warmth
      ..compression = compression
      ..deess = deess
      ..gate = gate
      ..normalize = normalize ? 1 : 0
      ..outputGainDb = outputGainDb
      ..useBacking = useBacking ? 1 : 0
      ..vocalBalance = vocalBalance
      ..backingOffsetMs = backingOffsetMs;
  }

  VoiceFxParams copyWith({
    int? keyRoot,
    int? scale,
    double? correction,
    double? retuneMs,
    double? humanize,
    double? transpose,
    double? formant,
    bool? robot,
    double? harmony,
    HarmonyMode? harmonyMode,
    double? doubler,
    double? reverb,
    double? reverbSize,
    double? delay,
    double? delayMs,
    double? brightness,
    double? warmth,
    double? compression,
    double? deess,
    double? gate,
    bool? normalize,
    double? outputGainDb,
    bool? useBacking,
    double? vocalBalance,
    double? backingOffsetMs,
  }) =>
      VoiceFxParams(
        keyRoot: keyRoot ?? this.keyRoot,
        scale: scale ?? this.scale,
        correction: correction ?? this.correction,
        retuneMs: retuneMs ?? this.retuneMs,
        humanize: humanize ?? this.humanize,
        transpose: transpose ?? this.transpose,
        formant: formant ?? this.formant,
        robot: robot ?? this.robot,
        harmony: harmony ?? this.harmony,
        harmonyMode: harmonyMode ?? this.harmonyMode,
        doubler: doubler ?? this.doubler,
        reverb: reverb ?? this.reverb,
        reverbSize: reverbSize ?? this.reverbSize,
        delay: delay ?? this.delay,
        delayMs: delayMs ?? this.delayMs,
        brightness: brightness ?? this.brightness,
        warmth: warmth ?? this.warmth,
        compression: compression ?? this.compression,
        deess: deess ?? this.deess,
        gate: gate ?? this.gate,
        normalize: normalize ?? this.normalize,
        outputGainDb: outputGainDb ?? this.outputGainDb,
        useBacking: useBacking ?? this.useBacking,
        vocalBalance: vocalBalance ?? this.vocalBalance,
        backingOffsetMs: backingOffsetMs ?? this.backingOffsetMs,
      );

  Map<String, Object> toJson() => {
        'keyRoot': keyRoot,
        'scale': scale,
        'correction': correction,
        'retuneMs': retuneMs,
        'humanize': humanize,
        'transpose': transpose,
        'formant': formant,
        'robot': robot,
        'harmony': harmony,
        'harmonyMode': harmonyMode.index,
        'doubler': doubler,
        'reverb': reverb,
        'reverbSize': reverbSize,
        'delay': delay,
        'delayMs': delayMs,
        'brightness': brightness,
        'warmth': warmth,
        'compression': compression,
        'deess': deess,
        'gate': gate,
        'normalize': normalize,
        'outputGainDb': outputGainDb,
        'useBacking': useBacking,
        'vocalBalance': vocalBalance,
        'backingOffsetMs': backingOffsetMs,
      };

  factory VoiceFxParams.fromJson(Map<String, dynamic> j) {
    const d = VoiceFxParams();
    double f(String k, double def) => (j[k] as num?)?.toDouble() ?? def;
    return VoiceFxParams(
      keyRoot: (j['keyRoot'] as num?)?.toInt() ?? d.keyRoot,
      scale: (j['scale'] as num?)?.toInt() ?? d.scale,
      correction: f('correction', d.correction),
      retuneMs: f('retuneMs', d.retuneMs),
      humanize: f('humanize', d.humanize),
      transpose: f('transpose', d.transpose),
      formant: f('formant', d.formant),
      robot: j['robot'] as bool? ?? d.robot,
      harmony: f('harmony', d.harmony),
      harmonyMode:
          HarmonyMode.values[((j['harmonyMode'] as num?)?.toInt() ?? 0).clamp(0, HarmonyMode.values.length - 1)],
      doubler: f('doubler', d.doubler),
      reverb: f('reverb', d.reverb),
      reverbSize: f('reverbSize', d.reverbSize),
      delay: f('delay', d.delay),
      delayMs: f('delayMs', d.delayMs),
      brightness: f('brightness', d.brightness),
      warmth: f('warmth', d.warmth),
      compression: f('compression', d.compression),
      deess: f('deess', d.deess),
      gate: f('gate', d.gate),
      normalize: j['normalize'] as bool? ?? d.normalize,
      outputGainDb: f('outputGainDb', d.outputGainDb),
      useBacking: j['useBacking'] as bool? ?? d.useBacking,
      vocalBalance: f('vocalBalance', d.vocalBalance),
      backingOffsetMs: f('backingOffsetMs', d.backingOffsetMs),
    );
  }

  @override
  bool operator ==(Object other) =>
      other is VoiceFxParams && _mapEquals(toJson(), other.toJson());

  @override
  int get hashCode => Object.hashAll(toJson().values);

  static bool _mapEquals(Map<String, Object> a, Map<String, Object> b) {
    if (a.length != b.length) return false;
    for (final e in a.entries) {
      if (b[e.key] != e.value) return false;
    }
    return true;
  }
}

class VoiceFxException implements Exception {
  VoiceFxException(this.message);
  final String message;
  @override
  String toString() => 'VoiceFxException: $message';
}

class PitchCurves {
  const PitchCurves(this.detected, this.corrected);

  /// MIDI note numbers per display bucket (0 = silence).
  final Float32List detected;
  final Float32List corrected;
}

/// One recording loaded in the engine. Call [dispose] when done.
class VoiceFxSession {
  VoiceFxSession._(this._address, this.duration, this.sampleRate, this.detectedKey);

  final int _address;

  /// Duration of the original take, in seconds.
  final double duration;
  final double sampleRate;
  final KeyDetection detectedKey;

  bool _disposed = false;
  Future<void> _queue = Future.value();

  /// Key of the loaded instrumental, null when there is none.
  KeyDetection? backingKey;

  /// Duration of the loaded instrumental (s), 0 when there is none.
  double backingDuration = 0;

  /// Loads a WAV take and analyses it (pitch tracking + key detection).
  static Future<VoiceFxSession> open(String wavPath) async {
    final r = await Isolate.run(() {
      final b = VoiceFxBindings.instance;
      final path = wavPath.toNativeUtf8();
      try {
        final s = b.openWav(path);
        if (s == nullptr) throw VoiceFxException(b.errorMessage());
        if (b.analyze(s) != 0) {
          final msg = b.errorMessage();
          b.close(s);
          throw VoiceFxException(msg);
        }
        final root = calloc<Int32>(), scale = calloc<Int32>();
        final conf = calloc<Float>();
        b.detectedKey(s, root, scale, conf);
        final out = (s.address, b.duration(s), b.sampleRate(s), root.value, scale.value, conf.value);
        calloc
          ..free(root)
          ..free(scale)
          ..free(conf);
        return out;
      } finally {
        calloc.free(path);
      }
    });
    final key = MusicalKey(r.$4, MusicalScale.values[r.$5.clamp(0, MusicalScale.values.length - 1)]);
    return VoiceFxSession._(r.$1, r.$2, r.$3, KeyDetection(key, r.$6));
  }

  /// Serialises work on this session (the native side is not re-entrant).
  Future<T> _run<T>(T Function(VoiceFxBindings b, SessionPtr s) job) {
    if (_disposed) return Future.error(StateError('session disposed'));
    final address = _address;
    final result = _queue.then((_) => Isolate.run(() => job(VoiceFxBindings.instance, Pointer.fromAddress(address))));
    _queue = result.then((_) {}, onError: (_) {});
    return result;
  }

  /// Loads an instrumental (MP3 or WAV) to sing over and detects its key.
  Future<KeyDetection> setBacking(String path) async {
    final r = await _run((b, s) {
      final p = path.toNativeUtf8();
      final root = calloc<Int32>(), scale = calloc<Int32>();
      final conf = calloc<Float>();
      try {
        if (b.setBacking(s, p) != 0) throw VoiceFxException(b.errorMessage());
        b.backingKey(s, root, scale, conf);
        return (b.backingDuration(s), root.value, scale.value, conf.value);
      } finally {
        calloc
          ..free(p)
          ..free(root)
          ..free(scale)
          ..free(conf);
      }
    });
    backingDuration = r.$1;
    backingKey = KeyDetection(MusicalKey(r.$2, MusicalScale.values[r.$3.clamp(0, MusicalScale.values.length - 1)]), r.$4);
    return backingKey!;
  }

  Future<void> clearBacking() async {
    await _run((b, s) => b.clearBacking(s));
    backingKey = null;
    backingDuration = 0;
  }

  /// Renders the take with [params]. Returns the rendered duration (s).
  Future<double> render(VoiceFxParams params) => _run((b, s) {
        final p = calloc<VfxParamsStruct>();
        try {
          params._writeTo(p.ref);
          if (b.render(s, p) != 0) throw VoiceFxException(b.errorMessage());
          return b.renderedDuration(s);
        } finally {
          calloc.free(p);
        }
      });

  /// bits: 16 (dithered), 24 or 32 (float).
  Future<void> exportWav(String path, {int bits = 24}) => _run((b, s) {
        final p = path.toNativeUtf8();
        try {
          if (b.exportWav(s, p, bits) != 0) throw VoiceFxException(b.errorMessage());
        } finally {
          calloc.free(p);
        }
      });

  Future<void> exportMp3(String path, {int kbps = 320}) => _run((b, s) {
        final p = path.toNativeUtf8();
        try {
          if (b.exportMp3(s, p, kbps) != 0) throw VoiceFxException(b.errorMessage());
        } finally {
          calloc.free(p);
        }
      });

  /// The untouched take, at the same loudness as the render (for A/B).
  Future<void> exportOriginal(String path) => _run((b, s) {
        final p = path.toNativeUtf8();
        try {
          if (b.exportOriginalWav(s, p) != 0) throw VoiceFxException(b.errorMessage());
        } finally {
          calloc.free(p);
        }
      });

  /// Peak envelope (0..1). [rendered] = false for the original take.
  Future<Float32List> waveform(int count, {bool rendered = true}) => _run((b, s) {
        final buf = calloc<Float>(count);
        try {
          if (b.waveform(s, rendered ? 1 : 0, buf, count) != 0) throw VoiceFxException(b.errorMessage());
          return Float32List.fromList(buf.asTypedList(count));
        } finally {
          calloc.free(buf);
        }
      });

  Future<PitchCurves> pitchCurves(int count) => _run((b, s) {
        final det = calloc<Float>(count), cor = calloc<Float>(count);
        try {
          if (b.pitchCurves(s, det, cor, count) != 0) throw VoiceFxException(b.errorMessage());
          return PitchCurves(Float32List.fromList(det.asTypedList(count)), Float32List.fromList(cor.asTypedList(count)));
        } finally {
          calloc
            ..free(det)
            ..free(cor);
        }
      });

  /// Frees the native session once pending work is done.
  Future<void> dispose() async {
    if (_disposed) return;
    final pending = _queue;
    _disposed = true;
    await pending;
    VoiceFxBindings.instance.close(Pointer.fromAddress(_address));
  }
}
