import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:file_picker/file_picker.dart';
import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';
import 'package:voicefx/voicefx.dart';

/// One recording ("prise") and the effect settings the user chose for it.
class Take {
  Take({
    required this.id,
    required this.dir,
    required this.name,
    required this.createdAt,
    required this.duration,
    this.preset = VoicePreset.pop,
    this.params,
    this.backingName,
    this.recordedOffsetMs = 0,
  });

  final String id;
  final Directory dir;
  String name;
  final DateTime createdAt;
  double duration;
  VoicePreset preset;
  VoiceFxParams? params;

  /// Original file name of the instrumental, null when there is none.
  String? backingName;

  /// Voice/instrumental offset measured while recording (ms).
  double recordedOffsetMs;

  File get audioFile => File('${dir.path}/take.wav');

  /// The instrumental copied into the take folder (backing.mp3 / backing.wav).
  File? get backingFile {
    if (backingName == null) return null;
    for (final ext in backingExtensions) {
      final f = File('${dir.path}/backing.$ext');
      if (f.existsSync()) return f;
    }
    return null;
  }

  static const backingExtensions = ['mp3', 'wav'];

  /// Copies [source] into the take folder as its instrumental.
  Future<void> attachBacking(File source, String name) async {
    await detachBacking();
    final ext = name.toLowerCase().endsWith('.wav') ? 'wav' : 'mp3';
    await source.copy('${dir.path}/backing.$ext');
    backingName = name;
    await save();
  }

  Future<void> detachBacking() async {
    for (final ext in backingExtensions) {
      final f = File('${dir.path}/backing.$ext');
      if (await f.exists()) await f.delete();
    }
    backingName = null;
    await save();
  }
  File get _metaFile => File('${dir.path}/meta.json');

  Map<String, Object?> toJson() => {
        'name': name,
        'createdAt': createdAt.toIso8601String(),
        'duration': duration,
        'preset': preset.name,
        if (params != null) 'params': params!.toJson(),
        if (backingName != null) 'backingName': backingName,
        'recordedOffsetMs': recordedOffsetMs,
      };

  static Take? load(Directory dir) {
    try {
      final j = jsonDecode(File('${dir.path}/meta.json').readAsStringSync()) as Map<String, dynamic>;
      return Take(
        id: dir.uri.pathSegments.where((s) => s.isNotEmpty).last,
        dir: dir,
        name: j['name'] as String? ?? 'Prise',
        createdAt: DateTime.tryParse(j['createdAt'] as String? ?? '') ?? DateTime.now(),
        duration: (j['duration'] as num?)?.toDouble() ?? 0,
        preset: VoicePreset.values.firstWhere((p) => p.name == j['preset'], orElse: () => VoicePreset.pop),
        params: j['params'] is Map ? VoiceFxParams.fromJson((j['params'] as Map).cast<String, dynamic>()) : null,
        backingName: j['backingName'] as String?,
        recordedOffsetMs: (j['recordedOffsetMs'] as num?)?.toDouble() ?? 0,
      );
    } catch (e) {
      debugPrint('Skipping unreadable take in ${dir.path}: $e');
      return null;
    }
  }

  Future<void> save() async {
    final tmp = File('${_metaFile.path}.tmp');
    await tmp.writeAsString(jsonEncode(toJson()), flush: true);
    await tmp.rename(_metaFile.path);
  }
}

/// Stores takes as folders in the app's private documents directory.
class TakeRepository extends ChangeNotifier {
  TakeRepository._(this._root);

  static TakeRepository? _instance;

  static Future<TakeRepository> instance() async {
    if (_instance != null) return _instance!;
    final docs = await getApplicationDocumentsDirectory();
    final root = Directory('${docs.path}/takes');
    await root.create(recursive: true);
    final repo = TakeRepository._(root);
    await repo.refresh();
    return _instance = repo;
  }

  final Directory _root;
  List<Take> _takes = [];

  List<Take> get takes => List.unmodifiable(_takes);

  Future<void> refresh() async {
    final list = <Take>[];
    await for (final e in _root.list()) {
      if (e is Directory) {
        final t = Take.load(e);
        if (t != null && t.audioFile.existsSync()) list.add(t);
      }
    }
    list.sort((a, b) => b.createdAt.compareTo(a.createdAt));
    _takes = list;
    notifyListeners();
  }

  /// A fresh folder for a recording in progress (not listed until [commit]).
  Future<Take> createDraft() async {
    final id = DateTime.now().millisecondsSinceEpoch.toString();
    final dir = Directory('${_root.path}/$id');
    await dir.create(recursive: true);
    return Take(id: id, dir: dir, name: _nextName(), createdAt: DateTime.now(), duration: 0);
  }

  Future<void> commit(Take take) async {
    await take.save();
    _takes.insert(0, take);
    notifyListeners();
  }

  Future<void> discard(Take take) async {
    if (await take.dir.exists()) await take.dir.delete(recursive: true);
    _takes.removeWhere((t) => t.id == take.id);
    notifyListeners();
  }

  Future<void> rename(Take take, String name) async {
    take.name = name.trim().isEmpty ? take.name : name.trim();
    await take.save();
    notifyListeners();
  }

  Future<void> update(Take take) async {
    await take.save();
    notifyListeners();
  }

  String _nextName() {
    var max = 0;
    final re = RegExp(r'^Prise (\d+)$');
    for (final t in _takes) {
      final m = re.firstMatch(t.name);
      if (m != null) max = math.max(max, int.parse(m.group(1)!));
    }
    return 'Prise ${max + 1}';
  }
}

/// Lets the user choose an instrumental and copies it to a local file
/// (pickers may return content URIs that are not plain paths).
/// Returns null when the user cancels.
Future<({File file, String name})?> pickInstrumental() async {
  final picked = await FilePicker.pickFile(
    type: FileType.custom,
    allowedExtensions: Take.backingExtensions,
    dialogTitle: 'Choisir une instrumentale',
  );
  if (picked == null) return null;
  final ext = picked.extension?.toLowerCase();
  if (ext == null || !Take.backingExtensions.contains(ext)) {
    throw const FormatException('Format non pris en charge : choisissez un fichier MP3 ou WAV.');
  }
  final tmp = await getTemporaryDirectory();
  final dest = File('${tmp.path}/picked_${DateTime.now().millisecondsSinceEpoch}.$ext');
  final sink = dest.openWrite();
  await sink.addStream(picked.readAsByteStream());
  await sink.close();
  return (file: dest, name: picked.name);
}
