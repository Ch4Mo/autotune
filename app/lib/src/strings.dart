import 'package:flutter/material.dart';
import 'package:voicefx/voicefx.dart';

/// French UI vocabulary, kept in one place to make translation easy.
class Strings {
  static const appName = 'VoxTune';

  static const noteNames = ['Do', 'Do♯', 'Ré', 'Mi♭', 'Mi', 'Fa', 'Fa♯', 'Sol', 'La♭', 'La', 'Si♭', 'Si'];

  static String scaleName(MusicalScale s) => switch (s) {
        MusicalScale.chromatic => 'chromatique',
        MusicalScale.major => 'majeur',
        MusicalScale.minor => 'mineur',
        MusicalScale.harmonicMinor => 'mineur harmonique',
        MusicalScale.majorPentatonic => 'penta. majeure',
        MusicalScale.minorPentatonic => 'penta. mineure',
        MusicalScale.blues => 'blues',
      };

  static String keyName(MusicalKey k) =>
      k.scale == MusicalScale.chromatic ? 'Toutes les notes' : '${noteNames[k.root]} ${scaleName(k.scale)}';

  static String harmonyName(HarmonyMode m) => switch (m) {
        HarmonyMode.thirdAbove => 'Tierce',
        HarmonyMode.thirdAndFifth => 'Accord',
        HarmonyMode.octaveBelow => 'Octave grave',
        HarmonyMode.choir => 'Chœur',
      };

  static String duration(double seconds) {
    final s = seconds.round();
    return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
  }

  static const _months = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'];

  static String date(DateTime d) {
    final now = DateTime.now();
    final hm = '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';
    if (d.year == now.year && d.month == now.month && d.day == now.day) return "Aujourd'hui · $hm";
    final y = now.subtract(const Duration(days: 1));
    if (d.year == y.year && d.month == y.month && d.day == y.day) return 'Hier · $hm';
    return '${d.day} ${_months[d.month - 1]}${d.year == now.year ? '' : ' ${d.year}'} · $hm';
  }
}

/// How each preset is presented to the user.
class PresetInfo {
  const PresetInfo(this.preset, this.name, this.description, this.icon, this.colors);

  final VoicePreset preset;
  final String name;
  final String description;
  final IconData icon;
  final List<Color> colors;

  static const all = [
    PresetInfo(VoicePreset.natural, 'Naturel', 'Justesse corrigée en douceur, voix intacte',
        Icons.spa_rounded, [Color(0xFF34D399), Color(0xFF059669)]),
    PresetInfo(VoicePreset.pop, 'Pop', 'Voix moderne, précise et brillante',
        Icons.auto_awesome_rounded, [Color(0xFFFF3D7F), Color(0xFFFF8A3D)]),
    PresetInfo(VoicePreset.hardTune, 'Hard Tune', "L'effet robotique culte du rap et de la pop",
        Icons.graphic_eq_rounded, [Color(0xFF8B5CF6), Color(0xFF3B82F6)]),
    PresetInfo(VoicePreset.choir, 'Chœur', 'Harmonies automatiques dans la tonalité',
        Icons.groups_rounded, [Color(0xFFF59E0B), Color(0xFFEF4444)]),
    PresetInfo(VoicePreset.studio, 'Studio', 'Son pro, sans correction de justesse',
        Icons.mic_external_on_rounded, [Color(0xFF64748B), Color(0xFF334155)]),
    PresetInfo(VoicePreset.robot, 'Robot', 'Voix de machine, une seule note',
        Icons.smart_toy_rounded, [Color(0xFF06B6D4), Color(0xFF0E7490)]),
    PresetInfo(VoicePreset.deep, 'Grave', 'Voix plus basse et plus profonde',
        Icons.south_rounded, [Color(0xFF7C3AED), Color(0xFF1E1B4B)]),
    PresetInfo(VoicePreset.high, 'Aiguë', 'Voix plus haute et plus légère',
        Icons.north_rounded, [Color(0xFFF472B6), Color(0xFFA855F7)]),
  ];

  static PresetInfo of(VoicePreset p) => all.firstWhere((i) => i.preset == p);
}

/// Notes of a key (index 0 = C), mirrors scaleMask() in the engine.
List<bool> scaleMask(MusicalKey k) {
  const steps = {
    MusicalScale.major: [0, 2, 4, 5, 7, 9, 11],
    MusicalScale.minor: [0, 2, 3, 5, 7, 8, 10],
    MusicalScale.harmonicMinor: [0, 2, 3, 5, 7, 8, 11],
    MusicalScale.majorPentatonic: [0, 2, 4, 7, 9],
    MusicalScale.minorPentatonic: [0, 3, 5, 7, 10],
    MusicalScale.blues: [0, 3, 5, 6, 7, 10],
  };
  final s = steps[k.scale];
  if (s == null) return List.filled(12, true);
  final mask = List.filled(12, false);
  for (final i in s) {
    mask[(k.root + i) % 12] = true;
  }
  return mask;
}
