import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'src/screens/home_screen.dart';
import 'src/strings.dart';
import 'src/takes.dart';
import 'src/theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await SystemChrome.setPreferredOrientations([DeviceOrientation.portraitUp]);
  final repo = await TakeRepository.instance();
  runApp(VoxTuneApp(repo: repo));
}

class VoxTuneApp extends StatelessWidget {
  const VoxTuneApp({super.key, required this.repo});

  final TakeRepository repo;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: Strings.appName,
      debugShowCheckedModeBanner: false,
      theme: buildTheme(),
      home: HomeScreen(repo: repo),
    );
  }
}
