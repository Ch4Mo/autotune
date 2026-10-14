import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:voicefx/voicefx.dart';

import '../theme.dart';

/// Waveform with playback progress; tap or drag to seek.
class WaveformView extends StatelessWidget {
  const WaveformView({super.key, required this.peaks, required this.progress, required this.onSeek, this.dimmed = false});

  final Float32List? peaks;
  final double progress;
  final ValueChanged<double> onSeek;
  final bool dimmed;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, c) {
      void seek(Offset p) => onSeek((p.dx / c.maxWidth).clamp(0.0, 1.0));
      return GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: (d) => seek(d.localPosition),
        onHorizontalDragUpdate: (d) => seek(d.localPosition),
        child: CustomPaint(
          size: Size(c.maxWidth, c.maxHeight),
          painter: _WaveformPainter(peaks, progress, dimmed),
        ),
      );
    });
  }
}

class _WaveformPainter extends CustomPainter {
  _WaveformPainter(this.peaks, this.progress, this.dimmed);

  final Float32List? peaks;
  final double progress;
  final bool dimmed;

  @override
  void paint(Canvas canvas, Size size) {
    final p = peaks;
    if (p == null || p.isEmpty) return;
    const bar = 3.0, gap = 2.0;
    final count = (size.width / (bar + gap)).floor();
    final mid = size.height / 2;
    final played = Paint()..color = dimmed ? AppColors.textDim : AppColors.accent;
    final rest = Paint()..color = AppColors.surfaceHigh;
    for (var i = 0; i < count; i++) {
      final a = (i * p.length / count).floor();
      final b = math.max(a + 1, ((i + 1) * p.length / count).floor());
      var v = 0.0;
      for (var k = a; k < b && k < p.length; k++) {
        v = math.max(v, p[k]);
      }
      // Gentle compression so quiet passages stay visible.
      final h = math.max(2.0, math.sqrt(v) * (size.height - 4));
      final x = i * (bar + gap);
      final r = RRect.fromRectAndRadius(Rect.fromLTWH(x, mid - h / 2, bar, h), const Radius.circular(1.5));
      canvas.drawRRect(r, x / size.width <= progress ? played : rest);
    }
  }

  @override
  bool shouldRepaint(_WaveformPainter old) => old.peaks != peaks || old.progress != progress || old.dimmed != dimmed;
}

/// "Justesse" view: the sung pitch (grey) and the corrected pitch (colour)
/// on a piano-roll grid. Shows what the pitch correction actually does.
class PitchView extends StatelessWidget {
  const PitchView({super.key, required this.curves, required this.progress, required this.onSeek, required this.keyMask});

  final PitchCurves? curves;
  final double progress;
  final ValueChanged<double> onSeek;

  /// 12 booleans: notes of the current key (highlighted rows).
  final List<bool> keyMask;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, c) {
      void seek(Offset p) => onSeek((p.dx / c.maxWidth).clamp(0.0, 1.0));
      return GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: (d) => seek(d.localPosition),
        onHorizontalDragUpdate: (d) => seek(d.localPosition),
        child: CustomPaint(size: Size(c.maxWidth, c.maxHeight), painter: _PitchPainter(curves, progress, keyMask)),
      );
    });
  }
}

class _PitchPainter extends CustomPainter {
  _PitchPainter(this.curves, this.progress, this.keyMask);

  final PitchCurves? curves;
  final double progress;
  final List<bool> keyMask;

  @override
  void paint(Canvas canvas, Size size) {
    final c = curves;
    if (c == null) return;
    final all = [...c.detected, ...c.corrected].where((m) => m > 0);
    if (all.isEmpty) return;
    var lo = all.reduce(math.min).floorToDouble() - 1;
    var hi = all.reduce(math.max).ceilToDouble() + 1;
    if (hi - lo < 12) {
      final mid = (hi + lo) / 2;
      lo = mid - 6;
      hi = mid + 6;
    }
    double y(double m) => size.height - (m - lo) / (hi - lo) * size.height;

    // Rows of the key's notes.
    final row = Paint()..color = AppColors.surfaceHigh.withValues(alpha: 0.6);
    for (var n = lo.floor(); n <= hi.ceil(); n++) {
      if (!keyMask[((n % 12) + 12) % 12]) continue;
      final top = y(n + 0.5), bottom = y(n - 0.5);
      canvas.drawRect(Rect.fromLTRB(0, top, size.width, bottom).deflate(0.5), row);
    }

    void curve(Float32List data, Paint paint) {
      final path = Path();
      var pen = false;
      for (var i = 0; i < data.length; i++) {
        final x = (i + 0.5) / data.length * size.width;
        if (data[i] <= 0) {
          pen = false;
          continue;
        }
        if (!pen) {
          path.moveTo(x, y(data[i]));
          pen = true;
        } else {
          path.lineTo(x, y(data[i]));
        }
      }
      canvas.drawPath(path, paint);
    }

    curve(c.detected, Paint()
      ..color = AppColors.textDim.withValues(alpha: 0.6)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5);
    curve(c.corrected, Paint()
      ..color = AppColors.accent
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.5
      ..strokeCap = StrokeCap.round);

    final x = progress * size.width;
    canvas.drawLine(Offset(x, 0), Offset(x, size.height), Paint()
      ..color = Colors.white70
      ..strokeWidth = 1.5);
  }

  @override
  bool shouldRepaint(_PitchPainter old) => old.curves != curves || old.progress != progress || old.keyMask != keyMask;
}
