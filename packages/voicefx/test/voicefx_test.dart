// Host-side tests of the FFI layer. They need a desktop build of the engine:
//   cmake -S src -B build && cmake --build build
//   VOICEFX_LIB=$PWD/build/libvoicefx.so flutter test
import 'dart:ffi';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:voicefx/src/bindings.dart';
import 'package:voicefx/voicefx.dart';

/// Writes a mono 16-bit WAV of a buzzy tone that glides around A3 (220 Hz).
File writeTestWav(String path, {double seconds = 2.0, int sr = 48000}) {
  final n = (seconds * sr).round();
  final data = ByteData(44 + n * 2);
  void str(int o, String s) {
    for (var i = 0; i < s.length; i++) {
      data.setUint8(o + i, s.codeUnitAt(i));
    }
  }

  str(0, 'RIFF');
  data.setUint32(4, 36 + n * 2, Endian.little);
  str(8, 'WAVEfmt ');
  data.setUint32(16, 16, Endian.little);
  data.setUint16(20, 1, Endian.little);
  data.setUint16(22, 1, Endian.little);
  data.setUint32(24, sr, Endian.little);
  data.setUint32(28, sr * 2, Endian.little);
  data.setUint16(32, 2, Endian.little);
  data.setUint16(34, 16, Endian.little);
  str(36, 'data');
  data.setUint32(40, n * 2, Endian.little);
  var phase = 0.0;
  for (var i = 0; i < n; i++) {
    final t = i / sr;
    final f = 220.0 * pow(2, (0.4 * sin(2 * pi * 0.7 * t)) / 12);
    phase += f / sr;
    var v = 0.0;
    for (var h = 1; h <= 12; h++) {
      v += sin(2 * pi * h * phase) / h;
    }
    final env = min(1.0, min(t, seconds - t) * 20);
    data.setInt16(44 + i * 2, (v * 0.25 * env * 32767).round().clamp(-32768, 32767), Endian.little);
  }
  return File(path)..writeAsBytesSync(data.buffer.asUint8List());
}

void main() {
  final lib = Platform.environment['VOICEFX_LIB'];
  final skip = (lib == null || lib.isEmpty) ? 'set VOICEFX_LIB to a desktop build of libvoicefx' : null;

  test('params struct matches the C layout', () {
    expect(sizeOf<VfxParamsStruct>(), 25 * 4);
  });

  test('presets round-trip through the native engine', () {
    for (final p in VoicePreset.values) {
      final params = VoiceFxParams.preset(p);
      expect(params.normalize, isTrue);
      expect(VoiceFxParams.fromJson(params.toJson()), params);
    }
    expect(VoiceFxParams.preset(VoicePreset.hardTune).retuneMs, 0);
    expect(VoiceFxParams.preset(VoicePreset.robot).robot, isTrue);
    expect(VoiceFxParams.preset(VoicePreset.studio).correction, 0);
  }, skip: skip);

  test('open, render, export', () async {
    final dir = Directory.systemTemp.createTempSync('vfx');
    final wav = writeTestWav('${dir.path}/take.wav');
    final s = await VoiceFxSession.open(wav.path);
    expect(s.duration, closeTo(2.0, 0.01));
    expect(s.sampleRate, 48000);

    final d = await s.render(VoiceFxParams.preset(VoicePreset.pop));
    expect(d, greaterThan(2.0));
    await s.exportWav('${dir.path}/out.wav', bits: 24);
    await s.exportMp3('${dir.path}/out.mp3', kbps: 192);
    await s.exportOriginal('${dir.path}/orig.wav');
    expect(File('${dir.path}/out.wav').lengthSync(), greaterThan(48000 * 2 * 3 * 2));
    expect(File('${dir.path}/out.mp3').lengthSync(), greaterThan(30000));

    final wf = await s.waveform(64);
    expect(wf.length, 64);
    expect(wf.reduce(max), closeTo(1.0, 1e-6));
    final curves = await s.pitchCurves(50);
    // The take hovers around A3 (MIDI 57); hard tuning keeps it on scale notes.
    final voiced = curves.detected.where((m) => m > 0).toList();
    expect(voiced, isNotEmpty);
    expect(voiced.reduce((a, b) => a + b) / voiced.length, closeTo(57, 1.0));

    // Concurrent calls are serialised safely.
    await Future.wait([
      s.render(VoiceFxParams.preset(VoicePreset.hardTune)),
      s.render(VoiceFxParams.preset(VoicePreset.choir)),
      s.waveform(10),
    ]);
    // Instrumental: the take itself works as a (WAV) backing track.
    final key = await s.setBacking(wav.path);
    expect(key.key.root, inInclusiveRange(0, 11));
    expect(s.backingDuration, closeTo(2.0, 0.01));
    final mixed = await s.render(VoiceFxParams.preset(VoicePreset.natural).copyWith(backingOffsetMs: 500));
    expect(mixed, greaterThan(2.4));
    await s.clearBacking();
    expect(s.backingKey, isNull);
    await expectLater(s.setBacking('/nope.mp3'), throwsA(isA<VoiceFxException>()));

    await s.dispose();
    dir.deleteSync(recursive: true);
  }, skip: skip);

  test('errors surface as VoiceFxException', () async {
    await expectLater(VoiceFxSession.open('/does/not/exist.wav'), throwsA(isA<VoiceFxException>()));
  }, skip: skip);
}
