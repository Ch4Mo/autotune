import 'package:flutter_test/flutter_test.dart';
import 'package:voicefx/voicefx.dart';
import 'package:voxtune/src/strings.dart';

void main() {
  test('key names are French', () {
    expect(Strings.keyName(const MusicalKey(9, MusicalScale.minor)), 'La mineur');
    expect(Strings.keyName(const MusicalKey(0, MusicalScale.major)), 'Do majeur');
    expect(Strings.keyName(const MusicalKey(4, MusicalScale.chromatic)), 'Toutes les notes');
  });

  test('scale masks', () {
    // A minor has the same notes as C major (no sharps / flats).
    final m = scaleMask(const MusicalKey(9, MusicalScale.minor));
    expect([for (var i = 0; i < 12; i++) if (m[i]) i], [0, 2, 4, 5, 7, 9, 11]);
    expect(scaleMask(const MusicalKey(3, MusicalScale.chromatic)).every((v) => v), isTrue);
  });

  test('durations', () {
    expect(Strings.duration(0), '0:00');
    expect(Strings.duration(61.4), '1:01');
  });

  test('every preset has UI metadata', () {
    for (final p in VoicePreset.values) {
      expect(PresetInfo.of(p).name, isNotEmpty);
    }
  });
}
