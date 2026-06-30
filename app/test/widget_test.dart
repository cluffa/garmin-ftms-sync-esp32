// This is a basic Flutter widget test.
//
// To perform an interaction with a widget in your test, use the WidgetTester
// utility in the flutter_test package. For example, you can send tap and scroll
// gestures. You can also use WidgetTester to find child widgets in the widget
// tree, read text, and verify that the values of widget properties are correct.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:garmin_ftms_app/main.dart';
import 'package:garmin_ftms_app/services/bridge_service.dart';

void main() {
  testWidgets('FTMS Sync app builds and shows connect screen', (WidgetTester tester) async {
    await tester.pumpWidget(
      ChangeNotifierProvider(
        create: (_) => BridgeService(),
        child: const FtmsSyncApp(),
      ),
    );

    // Verify that the app title is present
    expect(find.text('FTMS Sync'), findsWidgets);

    // Verify that the connect screen is shown
    expect(find.text('Connect to ESP32 bridge'), findsOneWidget);

    // Verify that WiFi connect button exists
    expect(find.byType(ElevatedButton), findsWidgets);
  });
}
