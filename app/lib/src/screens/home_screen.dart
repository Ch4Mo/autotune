import 'package:flutter/material.dart';

import '../strings.dart';
import '../takes.dart';
import '../theme.dart';
import 'record_screen.dart';
import 'studio_screen.dart';

/// List of takes + the big record button.
class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key, required this.repo});

  final TakeRepository repo;

  void _record(BuildContext context) =>
      Navigator.of(context).push(MaterialPageRoute(builder: (_) => RecordScreen(repo: repo)));

  void _open(BuildContext context, Take t) =>
      Navigator.of(context).push(MaterialPageRoute(builder: (_) => StudioScreen(take: t, repo: repo)));

  Future<void> _menu(BuildContext context, Take t) async {
    final action = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          ListTile(
            leading: const Icon(Icons.edit_rounded),
            title: const Text('Renommer'),
            onTap: () => Navigator.pop(c, 'rename'),
          ),
          ListTile(
            leading: const Icon(Icons.delete_outline_rounded, color: AppColors.record),
            title: const Text('Supprimer', style: TextStyle(color: AppColors.record)),
            onTap: () => Navigator.pop(c, 'delete'),
          ),
        ]),
      ),
    );
    if (!context.mounted) return;
    if (action == 'rename') {
      final ctl = TextEditingController(text: t.name);
      final name = await showDialog<String>(
        context: context,
        builder: (c) => AlertDialog(
          title: const Text('Renommer la prise'),
          content: TextField(controller: ctl, autofocus: true, onSubmitted: (v) => Navigator.pop(c, v)),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c), child: const Text('Annuler')),
            FilledButton(onPressed: () => Navigator.pop(c, ctl.text), child: const Text('OK')),
          ],
        ),
      );
      ctl.dispose();
      if (name != null) await repo.rename(t, name);
    } else if (action == 'delete') {
      final ok = await showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: Text('Supprimer « ${t.name} » ?'),
          content: const Text('La prise sera définitivement effacée.'),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Annuler')),
            FilledButton(
              style: FilledButton.styleFrom(backgroundColor: AppColors.record),
              onPressed: () => Navigator.pop(c, true),
              child: const Text('Supprimer'),
            ),
          ],
        ),
      );
      if (ok == true) await repo.discard(t);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: ShaderMask(
          shaderCallback: (r) => AppColors.accentGradient.createShader(r),
          child: const Text(Strings.appName,
              style: TextStyle(fontSize: 28, fontWeight: FontWeight.w800, color: Colors.white)),
        ),
      ),
      body: ListenableBuilder(
        listenable: repo,
        builder: (context, _) {
          final takes = repo.takes;
          if (takes.isEmpty) return const _EmptyState();
          return ListView.separated(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 140),
            itemCount: takes.length,
            separatorBuilder: (_, _) => const SizedBox(height: 10),
            itemBuilder: (context, i) {
              final t = takes[i];
              final info = PresetInfo.of(t.preset);
              return Card(
                clipBehavior: Clip.antiAlias,
                child: InkWell(
                  onTap: () => _open(context, t),
                  onLongPress: () => _menu(context, t),
                  child: Padding(
                    padding: const EdgeInsets.all(14),
                    child: Row(children: [
                      Container(
                        width: 48,
                        height: 48,
                        decoration: BoxDecoration(
                          borderRadius: BorderRadius.circular(14),
                          gradient: LinearGradient(colors: info.colors),
                        ),
                        child: Icon(info.icon, color: Colors.white),
                      ),
                      const SizedBox(width: 14),
                      Expanded(
                        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                          Text(t.name,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                          const SizedBox(height: 2),
                          Text('${Strings.date(t.createdAt)} · ${Strings.duration(t.duration)} · ${info.name}',
                              style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
                        ]),
                      ),
                      IconButton(
                        tooltip: 'Options',
                        icon: const Icon(Icons.more_vert_rounded, color: AppColors.textDim),
                        onPressed: () => _menu(context, t),
                      ),
                    ]),
                  ),
                ),
              );
            },
          );
        },
      ),
      floatingActionButtonLocation: FloatingActionButtonLocation.centerFloat,
      floatingActionButton: SizedBox(
        width: 84,
        height: 84,
        child: FloatingActionButton(
          heroTag: 'record',
          shape: const CircleBorder(),
          backgroundColor: AppColors.record,
          tooltip: 'Nouvelle prise',
          onPressed: () => _record(context),
          child: const Icon(Icons.mic_rounded, size: 40, color: Colors.white),
        ),
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState();

  @override
  Widget build(BuildContext context) {
    return const Center(
      child: Padding(
        padding: EdgeInsets.fromLTRB(40, 0, 40, 120),
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Icon(Icons.graphic_eq_rounded, size: 72, color: AppColors.accent2),
          SizedBox(height: 20),
          Text('Chantez, on s’occupe du reste',
              textAlign: TextAlign.center, style: TextStyle(fontSize: 22, fontWeight: FontWeight.w700)),
          SizedBox(height: 10),
          Text(
            'Appuyez sur le micro pour enregistrer votre première prise, puis choisissez un effet.',
            textAlign: TextAlign.center,
            style: TextStyle(color: AppColors.textDim, height: 1.4),
          ),
        ]),
      ),
    );
  }
}
