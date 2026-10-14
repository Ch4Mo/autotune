import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

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
  });

  final String id;
  final Directory dir;
  String name;
  final DateTime createdAt;
  double duration;
  VoicePreset preset;
  VoiceFxParams? params;

  File get audioFile => File('${dir.path}/take.wav');
  File get _metaFile => File('${dir.path}/meta.json');

  Map<String, Object?> toJson() => {
        'name': name,
        'createdAt': createdAt.toIso8601String(),
        'duration': duration,
        'preset': preset.name,
        if (params != null) 'params': params!.toJson(),
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
