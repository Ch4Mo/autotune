import 'dart:async';
import 'dart:io';

import 'package:audio_session/audio_session.dart';
import 'package:flutter/foundation.dart';
import 'package:just_audio/just_audio.dart';
import 'package:path_provider/path_provider.dart';
import 'package:voicefx/voicefx.dart';

import 'takes.dart';

enum StudioPhase { loading, ready, error }

enum ExportFormat {
  mp3High('MP3', 'Haute qualité · 320 kbps', 'mp3'),
  mp3Standard('MP3', 'Léger · 192 kbps', 'mp3'),
  wav24('WAV', 'Studio · 24 bits', 'wav'),
  wav16('WAV', 'Qualité CD · 16 bits', 'wav');

  const ExportFormat(this.label, this.detail, this.extension);
  final String label;
  final String detail;
  final String extension;
}

/// Owns the engine session, the current settings, background re-rendering
/// and the preview player of the effects screen.
class StudioController extends ChangeNotifier {
  StudioController(this.take, this.repo);

  final Take take;
  final TakeRepository repo;
  final AudioPlayer player = AudioPlayer();

  StudioPhase phase = StudioPhase.loading;
  String? error;
  VoiceFxSession? _session;

  late VoicePreset preset;
  late VoiceFxParams params;

  bool rendering = false;
  bool listenOriginal = false;
  Float32List? waveform;
  Float32List? originalWaveform;
  PitchCurves? curves;

  Timer? _debounce;
  bool _queued = false;
  Completer<void>? _idle;
  String? _effectPath;
  String? _originalPath;
  int _flip = 0;
  bool _disposed = false;
  late final Directory _cache;

  String loadingMessage = 'Analyse de votre voix…';
  bool busyBacking = false;

  bool get hasBacking => (_session?.backingDuration ?? 0) > 0;

  /// True when the key comes from the instrumental (same rule as the engine).
  bool get keyFromBacking {
    final b = _session?.backingKey;
    return b != null && b.key.scale != MusicalScale.chromatic && b.confidence >= 0.2;
  }

  KeyDetection? get detectedKey => keyFromBacking ? _session!.backingKey : _session?.detectedKey;

  /// Key actually used for correction (manual choice or detection).
  MusicalKey get effectiveKey {
    final det = detectedKey?.key ?? const MusicalKey(0, MusicalScale.chromatic);
    final root = params.keyRoot >= 0 ? params.keyRoot : det.root;
    var scale = params.scale >= 0 ? MusicalScale.values[params.scale] : det.scale;
    if (params.keyRoot >= 0 && params.scale < 0 && scale == MusicalScale.chromatic) scale = MusicalScale.major;
    return MusicalKey(root, scale);
  }

  bool get autoKey => params.keyRoot < 0 && params.scale < 0;

  Future<void> init() async {
    try {
      final audio = await AudioSession.instance;
      await audio.configure(const AudioSessionConfiguration.music());
      final tmp = await getTemporaryDirectory();
      _cache = Directory('${tmp.path}/preview_${take.id}');
      await _cache.create(recursive: true);

      _session = await VoiceFxSession.open(take.audioFile.path);
      final backing = take.backingFile;
      if (backing != null) {
        loadingMessage = "Analyse de l'instrumentale…";
        _notify();
        await _session!.setBacking(backing.path);
      }
      preset = take.preset;
      params = take.params ?? VoiceFxParams.preset(preset).copyWith(backingOffsetMs: take.recordedOffsetMs);
      originalWaveform = await _session!.waveform(400, rendered: false);

      player.playerStateStream.listen((s) {
        if (s.processingState == ProcessingState.completed) {
          player.pause();
          player.seek(Duration.zero);
        }
      });

      await _renderLoop();
      if (_disposed) return;
      phase = StudioPhase.ready;
    } catch (e) {
      debugPrint('Studio init failed: $e');
      error = e is VoiceFxException ? e.message : e.toString();
      phase = StudioPhase.error;
    }
    _notify();
  }

  void selectPreset(VoicePreset p) {
    preset = p;
    // Keep the user's key and instrumental settings when switching style.
    params = VoiceFxParams.preset(p).copyWith(
      keyRoot: params.keyRoot,
      scale: params.scale,
      useBacking: params.useBacking,
      vocalBalance: params.vocalBalance,
      backingOffsetMs: params.backingOffsetMs,
    );
    _notify();
    _schedule(immediate: true);
  }

  void resetToPreset() => selectPreset(preset);

  void update(VoiceFxParams p) {
    params = p;
    _notify();
    _schedule();
  }

  /// Adds or replaces the instrumental. Returns an error message, or null.
  Future<String?> chooseBacking() async {
    try {
      final picked = await pickInstrumental();
      if (picked == null) return null;
      busyBacking = true;
      _notify();
      await _session!.setBacking(picked.file.path);
      await take.attachBacking(picked.file, picked.name);
      // Added after recording: there is no measured offset.
      params = params.copyWith(useBacking: true, backingOffsetMs: 0);
      _originalPath = null;
      _schedule(immediate: true);
      return null;
    } on FormatException catch (e) {
      return e.message;
    } on VoiceFxException {
      return "Impossible de lire ce fichier. Choisissez un MP3 ou un WAV.";
    } catch (e) {
      return "Impossible d'ajouter l'instrumentale : $e";
    } finally {
      busyBacking = false;
      _notify();
    }
  }

  Future<void> removeBacking() async {
    await _session!.clearBacking();
    await take.detachBacking();
    _originalPath = null;
    _notify();
    _schedule(immediate: true);
  }

  void setKey({required int root, required int scale}) => update(params.copyWith(keyRoot: root, scale: scale));

  Future<void> togglePlay() async {
    if (player.playing) {
      await player.pause();
    } else {
      unawaited(player.play());
    }
  }

  Future<void> seekFraction(double f) async {
    final d = player.duration;
    if (d == null) return;
    await player.seek(d * f.clamp(0.0, 1.0));
  }

  Future<void> setListenOriginal(bool original) async {
    if (original == listenOriginal) return;
    listenOriginal = original;
    _notify();
    final path = original ? _originalPath : _effectPath;
    if (path != null) await _load(path);
  }

  void _schedule({bool immediate = false}) {
    _debounce?.cancel();
    _debounce = Timer(Duration(milliseconds: immediate ? 0 : 350), _renderLoop);
  }

  /// Renders until the settings stop changing; only one render runs at a time.
  Future<void> _renderLoop() async {
    final s = _session;
    if (s == null || _disposed) return;
    if (rendering) {
      _queued = true;
      return;
    }
    rendering = true;
    _idle = Completer<void>();
    _notify();
    try {
      do {
        _queued = false;
        final p = params;
        await s.render(p);
        if (_disposed) return;
        _flip ^= 1;
        final path = '${_cache.path}/effect_$_flip.wav';
        await s.exportWav(path, bits: 16);
        // With an instrumental the "before" file depends on balance and sync.
        if (_originalPath == null || hasBacking) {
          final orig = '${_cache.path}/original_$_flip.wav';
          await s.exportOriginal(orig);
          _originalPath = orig;
          if (listenOriginal) await _load(orig);
        }
        waveform = await s.waveform(400);
        curves = await s.pitchCurves(400);
        _effectPath = path;
        if (!listenOriginal) await _load(path);
        take
          ..preset = preset
          ..params = p;
        unawaited(repo.update(take));
      } while (_queued && !_disposed);
    } catch (e) {
      debugPrint('Render failed: $e');
      error = e is VoiceFxException ? e.message : e.toString();
    } finally {
      rendering = false;
      _idle?.complete();
      _idle = null;
      _notify();
    }
  }

  Future<void> _load(String path) async {
    final pos = player.position;
    final wasPlaying = player.playing;
    await player.setFilePath(path, initialPosition: pos);
    if (wasPlaying) unawaited(player.play());
  }

  /// Writes the final file (with the current settings) and returns it.
  Future<File> export(ExportFormat format, String name) async {
    _debounce?.cancel();
    if (rendering) {
      await _idle?.future;
    }
    // Make sure the last settings are rendered.
    if (take.params != params) await _renderLoop();
    final s = _session!;
    final dir = Directory('${_cache.path}/export');
    if (await dir.exists()) await dir.delete(recursive: true);
    await dir.create(recursive: true);
    final safe = name.trim().replaceAll(RegExp(r'[\\/:*?"<>|]'), '_');
    final file = File('${dir.path}/${safe.isEmpty ? 'VoxTune' : safe}.${format.extension}');
    switch (format) {
      case ExportFormat.mp3High:
        await s.exportMp3(file.path, kbps: 320);
      case ExportFormat.mp3Standard:
        await s.exportMp3(file.path, kbps: 192);
      case ExportFormat.wav24:
        await s.exportWav(file.path, bits: 24);
      case ExportFormat.wav16:
        await s.exportWav(file.path, bits: 16);
    }
    return file;
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    _debounce?.cancel();
    player.dispose();
    final s = _session;
    if (s != null) unawaited(s.dispose());
    super.dispose();
  }
}
