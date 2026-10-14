# VoxTune

Application **Android et iOS** pour enregistrer sa voix et lui appliquer des effets de
correction de justesse (type « autotune »), avec export **MP3** ou **WAV**.

L'idée : une app simple, pas un studio. On enregistre, on choisit un style, on écoute, on exporte.
Le moteur audio, lui, est fait pour un rendu de qualité professionnelle.

> « VoxTune » est un nom provisoire. « Auto-Tune » est une marque déposée d'Antares :
> il vaut mieux ne pas l'utiliser dans le nom ni dans la fiche de l'app sur les stores.

## Utilisation

1. **Enregistrer** : un gros bouton, un chrono et un vumètre (il prévient si le son sature).
2. **Choisir un style** : Naturel, Pop, Hard Tune, Chœur, Studio, Robot, Grave, Aiguë.
3. **Ajuster** (optionnel) : *Correction*, *Effet* (naturel ↔ robotique), *Ambiance*.
   La **tonalité** est détectée toute seule ; on peut aussi la choisir à la main.
   « Plus de réglages » donne accès à la hauteur, au timbre, aux harmonies, au doublage,
   à l'écho, à la brillance, à la chaleur, à la compression et à la réduction de bruit.
4. **Comparer** avant/après au même volume, et regarder la courbe « Justesse » (notes chantées
   contre notes corrigées).
5. **Exporter** en MP3 320/192 kbps ou en WAV 24/16 bits, puis partager ou enregistrer le fichier
   avec la feuille de partage du système.

## Architecture

```
app/                      Application Flutter (Dart) : interface Android + iOS
packages/voicefx/         Plugin FFI : moteur audio C++ partagé par les deux plateformes
  src/voicefx.h           API C (utilisée via dart:ffi)
  src/engine/             Moteur DSP (C++17)
  src/third_party/lame/   Encodeur MP3 LAME 3.100 (LGPL)
  ios/                    Podspec + fichiers de renvoi vers src/ (CocoaPods)
  android/                Build Gradle/CMake (NDK)
  lib/                    API Dart (VoiceFxSession, VoiceFxParams…)
  test_native/            Tests C++ et outil en ligne de commande
.github/workflows/ci.yml  Tests du moteur, build APK, build iOS (macOS)
```

Le traitement se fait **hors temps réel**, sur la prise complète : on peut donc regarder la
mélodie entière, sans latence, ce qui donne une meilleure qualité qu'un traitement en direct.
Les calculs tournent dans un isolate en arrière-plan pour que l'interface reste fluide.

### Chaîne de traitement

| Étape | Technique |
|---|---|
| Détection de hauteur | YIN accéléré par FFT, interpolation parabolique, correction des erreurs d'octave, lissage médian |
| Tonalité | Profils de Krumhansl-Kessler sur l'histogramme des notes chantées |
| Correction | Choix de la note avec hystérésis (pas de « tremblement » sur le vibrato), vitesse de correction réglable, *humanize* sur les notes tenues |
| Changement de hauteur | **TD-PSOLA** synchrone à la période : les formants sont préservés, la voix garde son timbre. Le timbre peut aussi être déplacé indépendamment de la hauteur |
| Harmonies | Voix générées dans la tonalité (tierce, accord, octave, chœur à 3 voix) |
| Chaîne vocale | Coupe-bas 24 dB/oct, gate doux, de-esser à bande partagée, EQ (chaleur / présence / air), compresseur à genou doux |
| Espace | Écho ping-pong stéréo, réverbération FDN 8 lignes avec diffusion |
| Mastering | Normalisation à **-14 LUFS** (ITU-R BS.1770-4), limiteur à anticipation à -1 dBFS |
| Export | WAV 16 bits avec dither TPDF, 24 bits, 32 bits float ; MP3 LAME CBR qualité 2 avec tag Xing/LAME |

## Développement

Prérequis : Flutter 3.47+, Android SDK + NDK, et Xcode pour iOS.

```bash
# Moteur : tests unitaires (Linux/macOS)
cmake -S packages/voicefx/src -B build -DVOICEFX_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/voicefx_tests /tmp

# Écouter le moteur sur un fichier WAV : preset 0-7, tonalité optionnelle (0 = Do … 11 = Si)
./build/voicefx_cli ma_voix.wav sortie.mp3 2

# Tests du plugin Dart contre une build desktop du moteur
cd packages/voicefx && VOICEFX_LIB=$PWD/../../build/libvoicefx.so flutter test

# Application
cd app
flutter run                       # sur un téléphone branché
flutter build apk --release       # Android
flutter build ipa                 # iOS (compte Apple Developer requis)
```

## Licences

Le moteur MP3 est **LAME** (LGPL v2). Sur Android, LAME est compilé dans `libvoicefx.so`. Pour
respecter la LGPL avec une distribution sur les stores, il faut fournir le code source de LAME
(il est dans ce dépôt) et permettre de relier l'app à une version modifiée. Si c'est un problème,
on peut remplacer LAME par l'encodeur AAC/M4A natif du système (AAC est libre de droits
d'utilisation sur Android et iOS).
