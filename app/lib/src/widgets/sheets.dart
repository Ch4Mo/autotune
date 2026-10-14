import 'dart:io';

import 'package:flutter/material.dart';
import 'package:share_plus/share_plus.dart';
import 'package:voicefx/voicefx.dart';

import '../strings.dart';
import '../studio_controller.dart';
import '../theme.dart';

/// Lets the user keep the detected key or force one.
Future<void> showKeyPicker(BuildContext context, StudioController ctl) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    builder: (context) => ListenableBuilder(
      listenable: ctl,
      builder: (context, _) {
        final det = ctl.detectedKey;
        final current = ctl.effectiveKey;
        final auto = ctl.autoKey;
        const scales = [
          MusicalScale.major,
          MusicalScale.minor,
          MusicalScale.chromatic,
          MusicalScale.majorPentatonic,
          MusicalScale.minorPentatonic,
          MusicalScale.blues,
          MusicalScale.harmonicMinor,
        ];
        return SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(20, 0, 20, 20),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('Tonalité', style: TextStyle(fontSize: 20, fontWeight: FontWeight.w700)),
                const SizedBox(height: 4),
                const Text('Les notes vers lesquelles votre voix est corrigée.',
                    style: TextStyle(color: AppColors.textDim)),
                const SizedBox(height: 16),
                _OptionTile(
                  selected: auto,
                  icon: Icons.auto_fix_high_rounded,
                  title: 'Automatique',
                  subtitle: det == null
                      ? 'Détection en cours…'
                      : det.isReliable
                          ? 'Détectée : ${Strings.keyName(det.key)}'
                          : 'Mélodie trop courte : toutes les notes',
                  onTap: () => ctl.setKey(root: -1, scale: -1),
                ),
                const SizedBox(height: 16),
                const Text('Note', style: TextStyle(fontWeight: FontWeight.w600)),
                const SizedBox(height: 8),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (var i = 0; i < 12; i++)
                      ChoiceChip(
                        label: SizedBox(width: 38, child: Text(Strings.noteNames[i], textAlign: TextAlign.center)),
                        selected: !auto && current.root == i,
                        onSelected: (_) => ctl.setKey(
                            root: i, scale: (current.scale == MusicalScale.chromatic ? MusicalScale.major : current.scale).index),
                      ),
                  ],
                ),
                const SizedBox(height: 16),
                const Text('Gamme', style: TextStyle(fontWeight: FontWeight.w600)),
                const SizedBox(height: 8),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: [
                    for (final s in scales)
                      ChoiceChip(
                        label: Text(s == MusicalScale.chromatic ? 'Toutes les notes' : Strings.scaleName(s)),
                        selected: !auto && current.scale == s,
                        onSelected: (_) => ctl.setKey(root: current.root, scale: s.index),
                      ),
                  ],
                ),
                const SizedBox(height: 12),
                const Text(
                  "Astuce : si vous chantez sur une musique, choisissez sa tonalité pour un résultat parfait.",
                  style: TextStyle(color: AppColors.textDim, fontSize: 13),
                ),
              ],
            ),
          ),
        );
      },
    ),
  );
}

class _OptionTile extends StatelessWidget {
  const _OptionTile(
      {required this.selected, required this.icon, required this.title, required this.subtitle, required this.onTap});

  final bool selected;
  final IconData icon;
  final String title;
  final String subtitle;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: selected ? AppColors.accent.withValues(alpha: 0.15) : AppColors.surfaceHigh,
      borderRadius: BorderRadius.circular(14),
      child: InkWell(
        borderRadius: BorderRadius.circular(14),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Row(children: [
            Icon(icon, color: selected ? AppColors.accent : AppColors.textDim),
            const SizedBox(width: 12),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
                Text(subtitle, style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
              ]),
            ),
            if (selected) const Icon(Icons.check_circle_rounded, color: AppColors.accent),
          ]),
        ),
      ),
    );
  }
}

/// Format choice + share sheet.
Future<void> showExportSheet(BuildContext context, StudioController ctl) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    builder: (context) => Padding(
      padding: EdgeInsets.only(bottom: MediaQuery.of(context).viewInsets.bottom),
      child: _ExportSheet(ctl: ctl),
    ),
  );
}

class _ExportSheet extends StatefulWidget {
  const _ExportSheet({required this.ctl});

  final StudioController ctl;

  @override
  State<_ExportSheet> createState() => _ExportSheetState();
}

class _ExportSheetState extends State<_ExportSheet> {
  ExportFormat _format = ExportFormat.mp3High;
  late final TextEditingController _name = TextEditingController(text: widget.ctl.take.name);
  bool _busy = false;

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  Future<void> _export() async {
    setState(() => _busy = true);
    final messenger = ScaffoldMessenger.of(context);
    final navigator = Navigator.of(context);
    final box = context.findRenderObject() as RenderBox?;
    try {
      final File file = await widget.ctl.export(_format, _name.text);
      final mime = _format.extension == 'mp3' ? 'audio/mpeg' : 'audio/wav';
      await SharePlus.instance.share(ShareParams(
        files: [XFile(file.path, mimeType: mime)],
        subject: _name.text,
        sharePositionOrigin: box == null ? null : box.localToGlobal(Offset.zero) & box.size,
      ));
      navigator.pop();
    } catch (e) {
      messenger.showSnackBar(SnackBar(content: Text("L'export a échoué : $e")));
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 0, 20, 20),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const Text('Exporter', style: TextStyle(fontSize: 20, fontWeight: FontWeight.w700)),
            const SizedBox(height: 16),
            TextField(
              controller: _name,
              textCapitalization: TextCapitalization.sentences,
              decoration: InputDecoration(
                labelText: 'Nom du fichier',
                filled: true,
                fillColor: AppColors.surfaceHigh,
                border: OutlineInputBorder(borderRadius: BorderRadius.circular(14), borderSide: BorderSide.none),
              ),
            ),
            const SizedBox(height: 16),
            for (final f in ExportFormat.values)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: _OptionTile(
                  selected: _format == f,
                  icon: f.extension == 'mp3' ? Icons.music_note_rounded : Icons.graphic_eq_rounded,
                  title: f.label,
                  subtitle: f.detail,
                  onTap: () => setState(() => _format = f),
                ),
              ),
            const SizedBox(height: 8),
            FilledButton.icon(
              onPressed: _busy ? null : _export,
              style: FilledButton.styleFrom(minimumSize: const Size.fromHeight(54)),
              icon: _busy
                  ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2.5))
                  : const Icon(Icons.ios_share_rounded),
              label: Text(_busy ? 'Préparation…' : 'Exporter et partager'),
            ),
          ],
        ),
      ),
    );
  }
}
