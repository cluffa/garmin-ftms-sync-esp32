import Flutter
import Foundation
import Network

/// iOS bridge to the ESP32 — WiFi/WebSocket path (no USB serial on iOS).
/// Channel: com.cluffa.garmin_ftms/bridge  (MethodChannel)
/// Events:  com.cluffa.garmin_ftms/bridge_events  (EventChannel)
///
/// connectWifi(host: String, port: Int) opens a TCP socket to the ESP32's
/// serial-over-WiFi server. The ESP32 firmware would need a TCP server task
/// that mirrors the USB serial JSON protocol — same line format, same commands.
///
/// For now the simulator can't reach real hardware, so connectWifi returns
/// false gracefully and the UI shows "No connection".
class BridgePlugin: NSObject, FlutterStreamHandler {
    private let methodChannel: FlutterMethodChannel
    private let eventChannel: FlutterEventChannel
    private var eventSink: FlutterEventSink?

    private var connection: NWConnection?
    private var receiveBuffer = Data()

    static func register(with messenger: FlutterBinaryMessenger) {
        _ = BridgePlugin(messenger: messenger)
    }

    init(messenger: FlutterBinaryMessenger) {
        methodChannel = FlutterMethodChannel(
            name: "com.cluffa.garmin_ftms/bridge",
            binaryMessenger: messenger)
        eventChannel = FlutterEventChannel(
            name: "com.cluffa.garmin_ftms/bridge_events",
            binaryMessenger: messenger)
        super.init()
        methodChannel.setMethodCallHandler(handle)
        eventChannel.setStreamHandler(self)
    }

    private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "connectUsb":
            // No USB serial on iOS — return false so the UI can offer WiFi instead
            result(false)
        case "connectWifi":
            let args = call.arguments as? [String: Any]
            let host = args?["host"] as? String ?? ""
            let port = args?["port"] as? Int ?? 9000
            connectWifi(host: host, port: port, result: result)
        case "send":
            let args = call.arguments as? [String: Any]
            let cmd = (args?["cmd"] as? String ?? "") + "\n"
            send(cmd, result: result)
        case "disconnect":
            connection?.cancel()
            connection = nil
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private func connectWifi(host: String, port: Int, result: @escaping FlutterResult) {
        guard !host.isEmpty, let nwPort = NWEndpoint.Port(rawValue: UInt16(port)) else {
            result(false); return
        }
        let conn = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: .tcp)
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                result(true)
                self?.startReceive()
            case .failed, .cancelled:
                result(false)
            default: break
            }
        }
        conn.start(queue: .global(qos: .utility))
        connection = conn
    }

    private func send(_ text: String, result: @escaping FlutterResult) {
        guard let conn = connection else { result(nil); return }
        let data = Data(text.utf8)
        conn.send(content: data, completion: .contentProcessed { _ in result(nil) })
    }

    private func startReceive() {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 65536) {
            [weak self] data, _, isComplete, _ in
            guard let self else { return }
            if let data { self.onData(data) }
            if !isComplete { self.startReceive() }
        }
    }

    private func onData(_ data: Data) {
        receiveBuffer.append(data)
        while let newline = receiveBuffer.firstIndex(of: UInt8(ascii: "\n")) {
            let lineData = receiveBuffer[receiveBuffer.startIndex...newline]
            receiveBuffer = receiveBuffer[receiveBuffer.index(after: newline)...]
            if let text = String(data: lineData, encoding: .utf8) {
                DispatchQueue.main.async { self.eventSink?(text) }
            }
        }
    }

    // FlutterStreamHandler
    func onListen(withArguments _: Any?, eventSink: @escaping FlutterEventSink) -> FlutterError? {
        self.eventSink = eventSink; return nil
    }
    func onCancel(withArguments _: Any?) -> FlutterError? {
        eventSink = nil; return nil
    }
}
