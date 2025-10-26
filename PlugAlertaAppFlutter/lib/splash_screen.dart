import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';

class SplashScreen extends StatelessWidget {
  final String assetName;
  final Duration duration;

  const SplashScreen({
    super.key,
    this.assetName = 'assets/logo_splash.svg',
    this.duration = const Duration(seconds: 3),
  });

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF1a1a2e),
      body: Center(
        child: SvgPicture.asset(
          assetName,
          height: 200,
          fit: BoxFit.contain,
          colorFilter: const ColorFilter.mode(
            Colors.green,
            BlendMode.srcIn,
          ),
        ),
      ),
    );
  }

  static Future<void> showAndNavigate(
    BuildContext context,
    Widget nextScreen,
    {Duration? duration}
  ) async {
    final startTime = DateTime.now();
    
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (context) => SplashScreen(duration: duration ?? const Duration(seconds: 3)),
    );

    final elapsed = DateTime.now().difference(startTime);
    final remaining = (duration ?? const Duration(seconds: 3)).inMilliseconds - elapsed.inMilliseconds;
    
    if (remaining > 0) {
      await Future.delayed(Duration(milliseconds: remaining));
    }

    if (context.mounted) {
      Navigator.of(context).pop();
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(builder: (_) => nextScreen),
      );
    }
  }
}

