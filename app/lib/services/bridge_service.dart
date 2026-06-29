import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import '../models/treadmill_state.dart';

/// Manages the connection to the ESP32 bridge.
///
/// Android: USB serial via BridgePlugin native channel (UsbManager, no lib).
/// iOS:     WiFi/TCP via BridgePlugin native channel (NWConnection).
class BridgeService extends ChangeNotifier {
  static const _ch = MethodChannel('com.cluffa.garmin_ftms/bridge');
  static const _ev = EventChannel('com.cluffa.garmin_ftms/bridge_events');

  StreamSubscription? _eventSub;
  final _lineBuf = StringBuffer();

  TreadmillState state = const TreadmillState();
  List<BridgeDevice> devices = [];
  bool connected = false;
  String? connectedDevice;
  String connectionMode = 'disconnected'; // 'usb' | 'wifi' | 'disconnected'

  final _stateCtrl = StreamController<TreadmillState>.broadcast();
  Stream<TreadmillState> get stateStream => _stateCtrl.stream;

  bool get canUsb => Platform.isAndroid;

  Future<bool> connectUsb() async {
    try {
      final ok = await _ch.invokeMethod<bool>('connectUsb') ?? false;
      if (ok) _onConnected('usb');
      return ok;
    } on PlatformException {
      return false;
    }
  }

  Future<bool> connectWifi(String host, {int port = 9000}) async {
    try {
      final ok = await _ch.invokeMethod<bool>(
            'connectWifi', {'host': host, 'port': port}) ??
          false;
      if (ok) _onConnected('wifi');
      return ok;
    } on PlatformException {
      return false;
    }
  }

  void _onConnected(String mode) {
    connectionMode = mode;
    connected = true;
    _eventSub = _ev.receiveBroadcastStream().listen(_onEvent);
    notifyListeners();
  }

  void _onEvent(dynamic data) {
    if (data is! String) return;
    _lineBuf.write(data);
    final buf = _lineBuf.toString();
    final lines = buf.split('\n');
    _lineBuf.clear();
    if (lines.length > 1) _lineBuf.write(lines.last);
    for (final line in lines.sublist(0, lines.length - 1)) {
      _handleLine(line.trim());
    }
  }

  void _handleLine(String line) {
    if (line.isEmpty || !line.startsWith('{')) return;
    try {
      final j = jsonDecode(line) as Map<String, dynamic>;
      final event = j['event'] as String?;
      final cmd = j['cmd'] as String?;
      if (event == 'state') {
        state = TreadmillState.fromJson(j);
        _stateCtrl.add(state);
        notifyListeners();
      } else if (event == 'connected') {
        connectedDevice = j['name'] as String?;
        connected = true;
        notifyListeners();
      } else if (event == 'disconnected') {
        connectedDevice = null;
        notifyListeners();
      } else if (cmd == 'list') {
        final raw = j['devices'] as List<dynamic>? ?? [];
        devices = raw
            .map((d) => BridgeDevice.fromJson(d as Map<String, dynamic>))
            .toList();
        notifyListeners();
      }
    } catch (_) {}
  }

  Future<void> _send(String cmd) =>
      _ch.invokeMethod('send', {'cmd': cmd});

  Future<void> scan() => _send('SCAN');
  Future<void> fetchList() => _send('LIST');
  Future<void> connectDevice(int idx) => _send('CONNECT $idx');
  Future<void> setSpeed(double kmh) => _send('SPEED $kmh');
  Future<void> setIncline(double pct) => _send('INCLINE $pct');
  Future<void> stop() => _send('STOP');
  Future<void> status() => _send('STATUS');

  @override
  void dispose() {
    _eventSub?.cancel();
    _stateCtrl.close();
    _ch.invokeMethod('disconnect');
    super.dispose();
  }
}
