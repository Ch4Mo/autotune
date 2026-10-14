// Low-level FFI bindings for src/voicefx.h (hand-written, keep in sync).
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

final class VfxParamsStruct extends Struct {
  @Int32()
  external int keyRoot;
  @Int32()
  external int scale;
  @Float()
  external double correction;
  @Float()
  external double retuneMs;
  @Float()
  external double humanize;
  @Float()
  external double transpose;
  @Float()
  external double formant;
  @Int32()
  external int robot;
  @Float()
  external double harmony;
  @Int32()
  external int harmonyMode;
  @Float()
  external double doubler;
  @Float()
  external double reverb;
  @Float()
  external double reverbSize;
  @Float()
  external double delay;
  @Float()
  external double delayMs;
  @Float()
  external double brightness;
  @Float()
  external double warmth;
  @Float()
  external double compression;
  @Float()
  external double deess;
  @Float()
  external double gate;
  @Int32()
  external int normalize;
  @Float()
  external double outputGainDb;
}

final class VfxSessionOpaque extends Opaque {}

typedef SessionPtr = Pointer<VfxSessionOpaque>;

class VoiceFxBindings {
  VoiceFxBindings(DynamicLibrary lib)
      : getPreset = lib.lookupFunction<Int32 Function(Int32, Pointer<VfxParamsStruct>),
            int Function(int, Pointer<VfxParamsStruct>)>('vfx_get_preset'),
        openWav = lib.lookupFunction<SessionPtr Function(Pointer<Utf8>), SessionPtr Function(Pointer<Utf8>)>(
            'vfx_session_open_wav'),
        close = lib.lookupFunction<Void Function(SessionPtr), void Function(SessionPtr)>('vfx_session_close'),
        sampleRate =
            lib.lookupFunction<Double Function(SessionPtr), double Function(SessionPtr)>('vfx_session_sample_rate'),
        duration =
            lib.lookupFunction<Double Function(SessionPtr), double Function(SessionPtr)>('vfx_session_duration'),
        analyze = lib.lookupFunction<Int32 Function(SessionPtr), int Function(SessionPtr)>('vfx_session_analyze'),
        detectedKey = lib.lookupFunction<
            Int32 Function(SessionPtr, Pointer<Int32>, Pointer<Int32>, Pointer<Float>),
            int Function(SessionPtr, Pointer<Int32>, Pointer<Int32>, Pointer<Float>)>('vfx_session_detected_key'),
        render = lib.lookupFunction<Int32 Function(SessionPtr, Pointer<VfxParamsStruct>),
            int Function(SessionPtr, Pointer<VfxParamsStruct>)>('vfx_session_render'),
        renderedDuration = lib.lookupFunction<Double Function(SessionPtr), double Function(SessionPtr)>(
            'vfx_session_rendered_duration'),
        exportWav = lib.lookupFunction<Int32 Function(SessionPtr, Pointer<Utf8>, Int32),
            int Function(SessionPtr, Pointer<Utf8>, int)>('vfx_session_export_wav'),
        exportMp3 = lib.lookupFunction<Int32 Function(SessionPtr, Pointer<Utf8>, Int32),
            int Function(SessionPtr, Pointer<Utf8>, int)>('vfx_session_export_mp3'),
        exportOriginalWav = lib.lookupFunction<Int32 Function(SessionPtr, Pointer<Utf8>),
            int Function(SessionPtr, Pointer<Utf8>)>('vfx_session_export_original_wav'),
        waveform = lib.lookupFunction<Int32 Function(SessionPtr, Int32, Pointer<Float>, Int32),
            int Function(SessionPtr, int, Pointer<Float>, int)>('vfx_session_waveform'),
        pitchCurves = lib.lookupFunction<Int32 Function(SessionPtr, Pointer<Float>, Pointer<Float>, Int32),
            int Function(SessionPtr, Pointer<Float>, Pointer<Float>, int)>('vfx_session_pitch_curves'),
        lastError = lib.lookupFunction<Pointer<Utf8> Function(), Pointer<Utf8> Function()>('vfx_last_error');

  final int Function(int, Pointer<VfxParamsStruct>) getPreset;
  final SessionPtr Function(Pointer<Utf8>) openWav;
  final void Function(SessionPtr) close;
  final double Function(SessionPtr) sampleRate;
  final double Function(SessionPtr) duration;
  final int Function(SessionPtr) analyze;
  final int Function(SessionPtr, Pointer<Int32>, Pointer<Int32>, Pointer<Float>) detectedKey;
  final int Function(SessionPtr, Pointer<VfxParamsStruct>) render;
  final double Function(SessionPtr) renderedDuration;
  final int Function(SessionPtr, Pointer<Utf8>, int) exportWav;
  final int Function(SessionPtr, Pointer<Utf8>, int) exportMp3;
  final int Function(SessionPtr, Pointer<Utf8>) exportOriginalWav;
  final int Function(SessionPtr, int, Pointer<Float>, int) waveform;
  final int Function(SessionPtr, Pointer<Float>, Pointer<Float>, int) pitchCurves;
  final Pointer<Utf8> Function() lastError;

  String errorMessage() => lastError().toDartString();

  static DynamicLibrary openLibrary() {
    final override = Platform.environment['VOICEFX_LIB'];
    if (override != null && override.isNotEmpty) return DynamicLibrary.open(override);
    if (Platform.isIOS || Platform.isMacOS) {
      try {
        return DynamicLibrary.open('voicefx.framework/voicefx');
      } on ArgumentError {
        // Statically linked pods: symbols live in the main executable.
        return DynamicLibrary.process();
      }
    }
    if (Platform.isAndroid || Platform.isLinux) return DynamicLibrary.open('libvoicefx.so');
    if (Platform.isWindows) return DynamicLibrary.open('voicefx.dll');
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }

  static VoiceFxBindings? _instance;

  /// Bindings are created lazily, once per isolate.
  static VoiceFxBindings get instance => _instance ??= VoiceFxBindings(openLibrary());
}
