import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
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

  @override
  void dispose() {
    _ticker?.cancel();
    _ampSub?.cancel();
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
        androidConfig: AndroidRecordConfig(audioSource: source),
        audioInterruption: AudioInterruptionMode.pause,
      );

  Future<void> _start() async {
    setState(() => _state = _State.starting);
    if (!await _recorder.hasPermission()) {
      if (!mounted) return;
      setState(() => _state = _State.idle);
      _showPermissionHelp();
      return;
    }
    final draft = await widget.repo.createDraft();
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
            const Padding(
              padding: EdgeInsets.fromLTRB(32, 0, 32, 24),
              child: Text(
                'Astuce : tenez le téléphone à 15–20 cm de votre bouche, dans une pièce calme. '
                'Pour chanter sur une musique, écoutez-la au casque.',
                textAlign: TextAlign.center,
                style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
              ),
            ),
          ],
        ),
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
