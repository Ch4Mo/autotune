import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:voicefx/voicefx.dart';

import '../strings.dart';
import '../studio_controller.dart';
import '../takes.dart';
import '../theme.dart';
import '../widgets/sheets.dart';
import '../widgets/waveform_view.dart';

/// The effects screen: listen, pick a style, tweak a few knobs, export.
class StudioScreen extends StatefulWidget {
  const StudioScreen({super.key, required this.take, required this.repo});

  final Take take;
  final TakeRepository repo;

  @override
  State<StudioScreen> createState() => _StudioScreenState();
}

class _StudioScreenState extends State<StudioScreen> {
  late final StudioController _ctl = StudioController(widget.take, widget.repo)..init();
  bool _pitchView = false;

  @override
  void dispose() {
    _ctl.dispose();
    super.dispose();
  }

  Future<void> _rename() async {
    final c = TextEditingController(text: widget.take.name);
    final name = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Renommer la prise'),
        content: TextField(controller: c, autofocus: true, onSubmitted: (v) => Navigator.pop(ctx, v)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Annuler')),
          FilledButton(onPressed: () => Navigator.pop(ctx, c.text), child: const Text('OK')),
        ],
      ),
    );
    c.dispose();
    if (name != null) {
      await widget.repo.rename(widget.take, name);
      setState(() {});
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _ctl,
      builder: (context, _) {
        return Scaffold(
          appBar: AppBar(
            title: GestureDetector(
              onTap: _rename,
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                Flexible(child: Text(widget.take.name, overflow: TextOverflow.ellipsis)),
                const SizedBox(width: 6),
                const Icon(Icons.edit_rounded, size: 18, color: AppColors.textDim),
              ]),
            ),
            actions: [
              if (_ctl.phase == StudioPhase.ready)
                Padding(
                  padding: const EdgeInsets.only(right: 12),
                  child: FilledButton.icon(
                    onPressed: () => showExportSheet(context, _ctl),
                    icon: const Icon(Icons.ios_share_rounded, size: 18),
                    label: const Text('Exporter'),
                  ),
                ),
            ],
          ),
          body: switch (_ctl.phase) {
            StudioPhase.loading => const _Loading(),
            StudioPhase.error => _Error(message: _ctl.error ?? 'Erreur inconnue'),
            StudioPhase.ready => _buildReady(context),
          },
        );
      },
    );
  }

  Widget _buildReady(BuildContext context) {
    final p = _ctl.params;
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
      children: [
        _display(),
        const SizedBox(height: 12),
        _transport(),
        const SizedBox(height: 24),
        const _SectionTitle('Style'),
        const SizedBox(height: 10),
        SizedBox(
          height: 132,
          child: ListView.separated(
            scrollDirection: Axis.horizontal,
            itemCount: PresetInfo.all.length,
            separatorBuilder: (_, _) => const SizedBox(width: 10),
            itemBuilder: (context, i) {
              final info = PresetInfo.all[i];
              return _PresetCard(
                info: info,
                selected: info.preset == _ctl.preset,
                onTap: () => _ctl.selectPreset(info.preset),
              );
            },
          ),
        ),
        const SizedBox(height: 8),
        Text(PresetInfo.of(_ctl.preset).description, style: const TextStyle(color: AppColors.textDim)),
        const SizedBox(height: 20),
        _KeyTile(ctl: _ctl),
        const SizedBox(height: 8),
        if (!p.robot) ...[
          _LabeledSlider(
            label: 'Correction',
            valueLabel: '${(p.correction * 100).round()} %',
            value: p.correction,
            onChanged: (v) => _ctl.update(p.copyWith(correction: v)),
          ),
          _LabeledSlider(
            label: 'Effet',
            leftHint: 'Naturel',
            rightHint: 'Robotique',
            value: _robotic(p),
            enabled: p.correction > 0,
            onChanged: (v) => _ctl.update(p.copyWith(retuneMs: 200 * (1 - v) * (1 - v), humanize: 0.8 * (1 - v))),
          ),
        ],
        _LabeledSlider(
          label: 'Ambiance',
          leftHint: 'Sec',
          rightHint: 'Grande salle',
          value: p.reverb,
          onChanged: (v) => _ctl.update(p.copyWith(reverb: v)),
        ),
        const SizedBox(height: 8),
        _Advanced(ctl: _ctl),
      ],
    );
  }

  static double _robotic(VoiceFxParams p) => (1 - math.sqrt((p.retuneMs / 200).clamp(0.0, 1.0))).clamp(0.0, 1.0);

  Widget _display() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 10, 14, 14),
        child: Column(children: [
          Row(children: [
            SegmentedButton<bool>(
              style: const ButtonStyle(visualDensity: VisualDensity.compact),
              showSelectedIcon: false,
              segments: const [
                ButtonSegment(value: false, label: Text('Onde')),
                ButtonSegment(value: true, label: Text('Justesse')),
              ],
              selected: {_pitchView},
              onSelectionChanged: (s) => setState(() => _pitchView = s.first),
            ),
            const Spacer(),
            AnimatedOpacity(
              opacity: _ctl.rendering ? 1 : 0,
              duration: const Duration(milliseconds: 200),
              child: const Row(children: [
                SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2)),
                SizedBox(width: 8),
                Text('Mise à jour…', style: TextStyle(color: AppColors.textDim, fontSize: 13)),
              ]),
            ),
          ]),
          const SizedBox(height: 10),
          SizedBox(
            height: 120,
            child: StreamBuilder<Duration>(
              stream: _ctl.player.positionStream,
              builder: (context, snap) {
                final d = _ctl.player.duration?.inMilliseconds ?? 0;
                final pos = snap.data?.inMilliseconds ?? 0;
                final progress = d > 0 ? (pos / d).clamp(0.0, 1.0) : 0.0;
                if (_pitchView) {
                  return PitchView(
                    curves: _ctl.curves,
                    progress: progress,
                    onSeek: _ctl.seekFraction,
                    keyMask: scaleMask(_ctl.effectiveKey),
                  );
                }
                return WaveformView(
                  peaks: _ctl.listenOriginal ? _ctl.originalWaveform : _ctl.waveform,
                  progress: progress,
                  dimmed: _ctl.listenOriginal,
                  onSeek: _ctl.seekFraction,
                );
              },
            ),
          ),
        ]),
      ),
    );
  }

  Widget _transport() {
    return Row(children: [
      StreamBuilder<Duration>(
        stream: _ctl.player.positionStream,
        builder: (context, snap) {
          final pos = (snap.data?.inMilliseconds ?? 0) / 1000.0;
          final total = (_ctl.player.duration?.inMilliseconds ?? 0) / 1000.0;
          return SizedBox(
            width: 84,
            child: Text('${Strings.duration(pos)} / ${Strings.duration(total)}',
                style: const TextStyle(color: AppColors.textDim, fontFeatures: [FontFeature.tabularFigures()])),
          );
        },
      ),
      const Spacer(),
      StreamBuilder<bool>(
        stream: _ctl.player.playingStream,
        builder: (context, snap) {
          final playing = snap.data ?? false;
          return Container(
            decoration: const BoxDecoration(shape: BoxShape.circle, gradient: AppColors.accentGradient),
            child: IconButton(
              iconSize: 40,
              padding: const EdgeInsets.all(12),
              tooltip: playing ? 'Pause' : 'Écouter',
              onPressed: _ctl.togglePlay,
              icon: Icon(playing ? Icons.pause_rounded : Icons.play_arrow_rounded, color: Colors.white),
            ),
          );
        },
      ),
      const Spacer(),
      SizedBox(
        width: 140,
        child: Tooltip(
          message: 'Comparer avec la voix originale',
          child: SegmentedButton<bool>(
            style: const ButtonStyle(visualDensity: VisualDensity.compact, padding: WidgetStatePropertyAll(EdgeInsets.zero)),
            showSelectedIcon: false,
            segments: const [
              ButtonSegment(value: true, label: Text('Avant')),
              ButtonSegment(value: false, label: Text('Après')),
            ],
            selected: {_ctl.listenOriginal},
            onSelectionChanged: (s) => _ctl.setListenOriginal(s.first),
          ),
        ),
      ),
    ]);
  }
}

class _Loading extends StatelessWidget {
  const _Loading();

  @override
  Widget build(BuildContext context) {
    return const Center(
      child: Column(mainAxisSize: MainAxisSize.min, children: [
        CircularProgressIndicator(),
        SizedBox(height: 20),
        Text('Analyse de votre voix…', style: TextStyle(fontSize: 16)),
        SizedBox(height: 6),
        Text('Détection des notes et de la tonalité', style: TextStyle(color: AppColors.textDim)),
      ]),
    );
  }
}

class _Error extends StatelessWidget {
  const _Error({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const Icon(Icons.error_outline_rounded, size: 48, color: AppColors.record),
          const SizedBox(height: 16),
          const Text('Impossible de lire cette prise', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          Text(message, textAlign: TextAlign.center, style: const TextStyle(color: AppColors.textDim)),
        ]),
      ),
    );
  }
}

class _SectionTitle extends StatelessWidget {
  const _SectionTitle(this.text);

  final String text;

  @override
  Widget build(BuildContext context) =>
      Text(text, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w700));
}

class _PresetCard extends StatelessWidget {
  const _PresetCard({required this.info, required this.selected, required this.onTap});

  final PresetInfo info;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      selected: selected,
      button: true,
      label: 'Style ${info.name}',
      child: GestureDetector(
        onTap: onTap,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 180),
          width: 104,
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(18),
            gradient: LinearGradient(
              colors: info.colors.map((c) => selected ? c : c.withValues(alpha: 0.35)).toList(),
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
            ),
            border: Border.all(color: selected ? Colors.white : Colors.transparent, width: 2),
          ),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Icon(info.icon, color: Colors.white, size: 30),
            const Spacer(),
            Text(info.name,
                style: const TextStyle(color: Colors.white, fontWeight: FontWeight.w700, fontSize: 15)),
          ]),
        ),
      ),
    );
  }
}

class _KeyTile extends StatelessWidget {
  const _KeyTile({required this.ctl});

  final StudioController ctl;

  @override
  Widget build(BuildContext context) {
    final key = ctl.effectiveKey;
    return Material(
      color: AppColors.surface,
      borderRadius: BorderRadius.circular(16),
      child: InkWell(
        borderRadius: BorderRadius.circular(16),
        onTap: () => showKeyPicker(context, ctl),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
          child: Row(children: [
            const Icon(Icons.piano_rounded, color: AppColors.accent),
            const SizedBox(width: 12),
            const Text('Tonalité', style: TextStyle(fontWeight: FontWeight.w600)),
            const Spacer(),
            Text('${Strings.keyName(key)}${ctl.autoKey ? ' · auto' : ''}',
                style: const TextStyle(color: AppColors.textDim)),
            const Icon(Icons.chevron_right_rounded, color: AppColors.textDim),
          ]),
        ),
      ),
    );
  }
}

class _LabeledSlider extends StatelessWidget {
  const _LabeledSlider({
    required this.label,
    required this.value,
    required this.onChanged,
    this.valueLabel,
    this.leftHint,
    this.rightHint,
    this.min = 0,
    this.max = 1,
    this.divisions,
    this.enabled = true,
  });

  final String label;
  final String? valueLabel;
  final String? leftHint;
  final String? rightHint;
  final double value;
  final double min;
  final double max;
  final int? divisions;
  final bool enabled;
  final ValueChanged<double> onChanged;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 12),
      child: Opacity(
        opacity: enabled ? 1 : 0.4,
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Text(label, style: const TextStyle(fontWeight: FontWeight.w600)),
            const Spacer(),
            if (valueLabel != null) Text(valueLabel!, style: const TextStyle(color: AppColors.textDim)),
          ]),
          Slider(
            value: value.clamp(min, max),
            min: min,
            max: max,
            divisions: divisions,
            onChanged: enabled ? onChanged : null,
          ),
          if (leftHint != null || rightHint != null)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20),
              child: Row(children: [
                Text(leftHint ?? '', style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                const Spacer(),
                Text(rightHint ?? '', style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
              ]),
            ),
        ]),
      ),
    );
  }
}

class _Advanced extends StatelessWidget {
  const _Advanced({required this.ctl});

  final StudioController ctl;

  String _signed(double v, String unit) => '${v > 0 ? '+' : ''}${v.toStringAsFixed(v.truncateToDouble() == v ? 0 : 1)} $unit';

  @override
  Widget build(BuildContext context) {
    final p = ctl.params;
    return Theme(
      data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
      child: ExpansionTile(
        tilePadding: EdgeInsets.zero,
        childrenPadding: EdgeInsets.zero,
        title: const Text('Plus de réglages', style: TextStyle(fontWeight: FontWeight.w700)),
        children: [
          _LabeledSlider(
            label: 'Hauteur',
            valueLabel: _signed(p.transpose, 'demi-tons'),
            value: p.transpose,
            min: -12,
            max: 12,
            divisions: 24,
            onChanged: (v) => ctl.update(p.copyWith(transpose: v)),
          ),
          _LabeledSlider(
            label: 'Timbre',
            leftHint: 'Plus grave',
            rightHint: 'Plus clair',
            value: p.formant,
            min: -6,
            max: 6,
            divisions: 24,
            onChanged: (v) => ctl.update(p.copyWith(formant: v)),
          ),
          _LabeledSlider(
            label: 'Harmonies',
            valueLabel: p.harmony == 0 ? 'Désactivées' : '${(p.harmony * 100).round()} %',
            value: p.harmony,
            onChanged: (v) => ctl.update(p.copyWith(harmony: v)),
          ),
          if (p.harmony > 0)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Wrap(spacing: 8, children: [
                for (final m in HarmonyMode.values)
                  ChoiceChip(
                    label: Text(Strings.harmonyName(m)),
                    selected: p.harmonyMode == m,
                    onSelected: (_) => ctl.update(p.copyWith(harmonyMode: m)),
                  ),
              ]),
            ),
          _LabeledSlider(
            label: 'Doublage stéréo',
            value: p.doubler,
            onChanged: (v) => ctl.update(p.copyWith(doubler: v)),
          ),
          _LabeledSlider(
            label: 'Écho',
            value: p.delay,
            onChanged: (v) => ctl.update(p.copyWith(delay: v)),
          ),
          _LabeledSlider(
            label: 'Brillance',
            leftHint: 'Doux',
            rightHint: 'Brillant',
            value: p.brightness,
            min: -1,
            max: 1,
            onChanged: (v) => ctl.update(p.copyWith(brightness: v)),
          ),
          _LabeledSlider(
            label: 'Chaleur',
            value: p.warmth,
            min: -1,
            max: 1,
            onChanged: (v) => ctl.update(p.copyWith(warmth: v)),
          ),
          _LabeledSlider(
            label: 'Compression',
            leftHint: 'Dynamique',
            rightHint: 'Compacte',
            value: p.compression,
            onChanged: (v) => ctl.update(p.copyWith(compression: v)),
          ),
          _LabeledSlider(
            label: 'Réduction du bruit',
            value: p.gate,
            onChanged: (v) => ctl.update(p.copyWith(gate: v)),
          ),
          SwitchListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Voix robot (une seule note)', style: TextStyle(fontWeight: FontWeight.w600)),
            value: p.robot,
            onChanged: (v) => ctl.update(p.copyWith(robot: v, retuneMs: v ? 0 : p.retuneMs)),
          ),
          const SizedBox(height: 8),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton.icon(
              onPressed: ctl.resetToPreset,
              icon: const Icon(Icons.restart_alt_rounded),
              label: Text('Réinitialiser « ${PresetInfo.of(ctl.preset).name} »'),
            ),
          ),
        ],
      ),
    );
  }
}
