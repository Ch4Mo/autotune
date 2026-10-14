import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:just_audio/just_audio.dart';
import 'package:record/record.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../strings.dart';
import '../takes.dart';
import '../theme.dart';
import 'studio_screen.dart';

/// Full-screen recorder: one big button, a timer and a live level meter.
class RecordScreen extends StatefulWidget {
  const RecordScreen({super.key, required this.repo});

  final TakeRepository repo;

  @override
  State<RecordScreen> createState() => _RecordScreenState();
}

enum _State { idle, starting, recording, saving }

class _RecordScreenState extends State<RecordScreen> {
  static const maxDuration = Duration(minutes: 6);

  final _recorder = AudioRecorder();
  _State _state = _State.idle;
  Take? _draft;
  final _clock = Stopwatch();
  Timer? _ticker;
  StreamSubscription<Amplitude>? _ampSub;
  double _level = 0; // 0..1
  double _peakHold = 0;
  bool _clipped = false;

  // Instrumental played in the headphones while singing.
  ({File file, String name})? _backing;
  Duration? _backingLength;
  AudioPlayer? _player;
  StreamSubscription<PlayerState>? _playerSub;
  double _offsetMs = 0;

  @override
  void dispose() {
    _ticker?.cancel();
    _ampSub?.cancel();
    _playerSub?.cancel();
    _player?.dispose();
    WakelockPlus.disable();
    if (_state == _State.recording) {
      _recorder.cancel();
      final d = _draft;
      if (d != null) widget.repo.discard(d);
    }
    _recorder.dispose();
    super.dispose();
  }

  RecordConfig _config(AndroidAudioSource source) => RecordConfig(
        encoder: AudioEncoder.wav,
        sampleRate: 48000,
        numChannels: 1,
        // Raw microphone: no automatic gain / noise suppression, they
        // damage singing voice and the engine does its own processing.
        autoGain: false,
        echoCancel: false,
        noiseSuppress: false,
        // Bluetooth headsets: listen through them (A2DP) but keep recording with
        // the phone's microphone; a headset mic (HFP/SCO) is limited to 8-16 kHz.
        androidConfig: AndroidRecordConfig(audioSource: source, manageBluetooth: false),
        iosConfig: const IosRecordConfig(categoryOptions: [
          IosAudioCategoryOption.defaultToSpeaker,
          IosAudioCategoryOption.allowBluetoothA2DP,
        ]),
        audioInterruption: AudioInterruptionMode.pause,
      );

  Future<void> _pickBacking() async {
    try {
      final picked = await pickInstrumental();
      if (picked == null) return;
      final probe = AudioPlayer();
      Duration? length;
      try {
        length = await probe.setFilePath(picked.file.path);
      } finally {
        await probe.dispose();
      }
      if (!mounted) return;
      setState(() {
        _backing = picked;
        _backingLength = length;
      });
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(e is FormatException ? e.message : "Impossible d'ouvrir ce fichier audio.")));
    }
  }

  /// Starts the instrumental and measures when it really started, relative to
  /// the recording, so the voice can be lined up with it afterwards.
  Future<void> _startBacking(Stopwatch sync, Duration recStart) async {
    final player = _player;
    if (player == null) return;
    _playerSub = player.playerStateStream.listen((s) {
      if (s.processingState == ProcessingState.completed) _stop();
    });
    unawaited(player.play());
    final started = Completer<void>();
    late final StreamSubscription<Duration> sub;
    sub = player.positionStream.listen((pos) {
      if (pos > const Duration(milliseconds: 30) && !started.isCompleted) {
        final playStart = sync.elapsed - pos;
        _offsetMs = (recStart - playStart).inMicroseconds / 1000.0;
        started.complete();
        sub.cancel();
      }
    });
  }

  Future<void> _start() async {
    setState(() => _state = _State.starting);
    if (!await _recorder.hasPermission()) {
      if (!mounted) return;
      setState(() => _state = _State.idle);
      _showPermissionHelp();
      return;
    }
    final draft = await widget.repo.createDraft();
    if (_backing != null) {
      _player = AudioPlayer();
      await _player!.setFilePath(_backing!.file.path);
    }
    final sync = Stopwatch()..start();
    try {
      try {
        // Android's CDD requires this source to have AGC and noise reduction
        // disabled and a flat frequency response: the cleanest capture.
        await _recorder.start(_config(AndroidAudioSource.voiceRecognition), path: draft.audioFile.path);
      } catch (_) {
        await _recorder.start(_config(AndroidAudioSource.mic), path: draft.audioFile.path);
      }
    } catch (e) {
      await widget.repo.discard(draft);
      if (!mounted) return;
      setState(() => _state = _State.idle);
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text("Impossible de démarrer l'enregistrement : $e")));
      return;
    }
    final recStart = sync.elapsed;
    await _startBacking(sync, recStart);
    HapticFeedback.mediumImpact();
    await WakelockPlus.enable();
    _draft = draft;
    _clock
      ..reset()
      ..start();
    _ticker = Timer.periodic(const Duration(milliseconds: 200), (_) {
      if (_clock.elapsed >= maxDuration) _stop();
      if (mounted) setState(() {});
    });
    _ampSub = _recorder.onAmplitudeChanged(const Duration(milliseconds: 60)).listen((a) {
      // dBFS -> 0..1 on a -60..0 dB scale.
      final v = ((a.current + 60) / 60).clamp(0.0, 1.0);
      setState(() {
        _level = v > _level ? v : _level * 0.75 + v * 0.25;
        _peakHold = v > _peakHold ? v : _peakHold - 0.01;
        if (a.current > -0.5) _clipped = true;
      });
    });
    if (mounted) setState(() => _state = _State.recording);
  }

  Future<void> _stop() async {
    if (_state != _State.recording) return;
    setState(() => _state = _State.saving);
    _ticker?.cancel();
    await _ampSub?.cancel();
    _clock.stop();
    await WakelockPlus.disable();
    HapticFeedback.mediumImpact();
    await _recorder.stop();
    await _playerSub?.cancel();
    await _player?.stop();
    final draft = _draft!;
    final seconds = _clock.elapsedMilliseconds / 1000.0;
    if (seconds < 1.0 || !await draft.audioFile.exists()) {
      await widget.repo.discard(draft);
      if (!mounted) return;
      setState(() => _state = _State.idle);
      ScaffoldMessenger.of(context)
          .showSnackBar(const SnackBar(content: Text('Prise trop courte, réessayez (au moins 1 seconde).')));
      return;
    }
    draft.duration = seconds;
    final backing = _backing;
    if (backing != null) {
      draft.recordedOffsetMs = _offsetMs;
      await draft.attachBacking(backing.file, backing.name);
    }
    await widget.repo.commit(draft);
    if (!mounted) return;
    await Navigator.of(context).pushReplacement(MaterialPageRoute(
      builder: (_) => StudioScreen(take: draft, repo: widget.repo),
    ));
  }

  void _showPermissionHelp() {
    showDialog<void>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Micro non autorisé'),
        content: const Text(
            "Pour enregistrer votre voix, autorisez l'accès au micro dans les réglages de votre téléphone."),
        actions: [TextButton(onPressed: () => Navigator.pop(c), child: const Text('OK'))],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final recording = _state == _State.recording;
    final elapsed = _clock.elapsed;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Enregistrer'),
        leading: IconButton(
          icon: const Icon(Icons.close_rounded),
          onPressed: _state == _State.saving ? null : () => Navigator.pop(context),
        ),
      ),
      body: SafeArea(
        child: Column(
          children: [
            const Spacer(),
            Text(
              Strings.duration(elapsed.inMilliseconds / 1000.0),
              style: const TextStyle(fontSize: 64, fontWeight: FontWeight.w300, fontFeatures: [FontFeature.tabularFigures()]),
            ),
            const SizedBox(height: 8),
            Text(
              switch (_state) {
                _State.idle => 'Appuyez pour commencer',
                _State.starting => 'Préparation…',
                _State.recording => _clipped ? 'Trop fort ! Éloignez un peu le téléphone' : 'Enregistrement…',
                _State.saving => 'Enregistrement de la prise…',
              },
              style: TextStyle(color: _clipped && recording ? AppColors.record : AppColors.textDim, fontSize: 16),
            ),
            const Spacer(),
            _RecordButton(
              recording: recording,
              level: recording ? _level : 0,
              busy: _state == _State.starting || _state == _State.saving,
              onTap: recording ? _stop : (_state == _State.idle ? _start : null),
            ),
            const SizedBox(height: 32),
            _LevelBar(level: recording ? _level : 0, peak: recording ? _peakHold : 0),
            const Spacer(),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 24),
              child: _BackingCard(
                name: _backing?.name,
                length: _backingLength,
                locked: _state != _State.idle,
                onPick: _pickBacking,
                onRemove: () => setState(() {
                  _backing = null;
                  _backingLength = null;
                }),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(32, 16, 32, 24),
              child: Text(
                _backing != null
                    ? 'Mettez un casque ou des écouteurs : la musique ne doit pas être captée par le micro. '
                        "L'enregistrement s'arrête tout seul à la fin du morceau."
                    : 'Astuce : tenez le téléphone à 15–20 cm de votre bouche, dans une pièce calme.',
                textAlign: TextAlign.center,
                style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _BackingCard extends StatelessWidget {
  const _BackingCard(
      {required this.name, required this.length, required this.locked, required this.onPick, required this.onRemove});

  final String? name;
  final Duration? length;
  final bool locked;
  final VoidCallback onPick;
  final VoidCallback onRemove;

  @override
  Widget build(BuildContext context) {
    if (name == null) {
      return OutlinedButton.icon(
        onPressed: locked ? null : onPick,
        style: OutlinedButton.styleFrom(minimumSize: const Size.fromHeight(52)),
        icon: const Icon(Icons.library_music_rounded),
        label: const Text('Chanter sur une instrumentale (MP3, WAV)'),
      );
    }
    return Material(
      color: AppColors.surface,
      borderRadius: BorderRadius.circular(16),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 6, 10),
        child: Row(children: [
          const Icon(Icons.library_music_rounded, color: AppColors.accent),
          const SizedBox(width: 12),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(name!, maxLines: 1, overflow: TextOverflow.ellipsis, style: const TextStyle(fontWeight: FontWeight.w600)),
              Text(length == null ? 'Instrumentale' : 'Instrumentale · ${Strings.duration(length!.inMilliseconds / 1000)}',
                  style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
            ]),
          ),
          IconButton(
            tooltip: "Retirer l'instrumentale",
            onPressed: locked ? null : onRemove,
            icon: const Icon(Icons.close_rounded),
          ),
        ]),
      ),
    );
  }
}

class _RecordButton extends StatelessWidget {
  const _RecordButton({required this.recording, required this.level, required this.busy, required this.onTap});

  final bool recording;
  final double level;
  final bool busy;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final halo = 150 + 70 * level;
    return Semantics(
      button: true,
      label: recording ? "Arrêter l'enregistrement" : "Démarrer l'enregistrement",
      child: GestureDetector(
        onTap: onTap,
        child: SizedBox(
          width: 230,
          height: 230,
          child: Stack(
            alignment: Alignment.center,
            children: [
              AnimatedContainer(
                duration: const Duration(milliseconds: 80),
                width: recording ? halo : 150,
                height: recording ? halo : 150,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: AppColors.record.withValues(alpha: recording ? 0.18 : 0.08),
                ),
              ),
              Container(
                width: 120,
                height: 120,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: AppColors.record,
                  boxShadow: [BoxShadow(color: AppColors.record.withValues(alpha: 0.5), blurRadius: 30)],
                ),
                child: busy
                    ? const Padding(padding: EdgeInsets.all(40), child: CircularProgressIndicator(color: Colors.white))
                    : AnimatedSwitcher(
                        duration: const Duration(milliseconds: 200),
                        child: recording
                            ? Container(
                                key: const ValueKey('stop'),
                                width: 40,
                                height: 40,
                                decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(8)),
                              )
                            : const Icon(Icons.mic_rounded, key: ValueKey('mic'), size: 56, color: Colors.white),
                      ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _LevelBar extends StatelessWidget {
  const _LevelBar({required this.level, required this.peak});

  final double level;
  final double peak;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 48),
      child: SizedBox(
        height: 8,
        child: LayoutBuilder(builder: (context, c) {
          return Stack(children: [
            Container(decoration: BoxDecoration(color: AppColors.surfaceHigh, borderRadius: BorderRadius.circular(4))),
            AnimatedContainer(
              duration: const Duration(milliseconds: 60),
              width: c.maxWidth * level,
              decoration: BoxDecoration(
                gradient: const LinearGradient(colors: [Color(0xFF34D399), Color(0xFFFACC15), AppColors.record]),
                borderRadius: BorderRadius.circular(4),
              ),
            ),
            Positioned(
              left: (c.maxWidth * peak - 2).clamp(0.0, c.maxWidth - 2),
              child: Container(width: 2, height: 8, color: Colors.white70),
            ),
          ]);
        }),
      ),
    );
  }
}
