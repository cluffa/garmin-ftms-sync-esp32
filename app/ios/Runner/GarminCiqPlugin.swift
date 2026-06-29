import Flutter
import Foundation

/// Wraps the Garmin ConnectIQ iOS SDK.
///
/// TODO: Add the ConnectIQ iOS SDK framework:
///   1. Download from https://developer.garmin.com/connect-iq/sdk/
///   2. Drag ConnectIQ.framework into ios/Runner/ in Xcode
///   3. Add to Runner target → General → Frameworks, Libraries, Embedded Content
///   4. Uncomment the ConnectIQ implementation below
///
/// Until then this compiles as a no-op stub so the channel is wired up.
class GarminCiqPlugin: NSObject {
    private let channel: FlutterMethodChannel

    static func register(with messenger: FlutterBinaryMessenger) {
        _ = GarminCiqPlugin(messenger: messenger)
    }

    init(messenger: FlutterBinaryMessenger) {
        channel = FlutterMethodChannel(
            name: "com.cluffa.garmin_ftms/ciq",
            binaryMessenger: messenger)
        super.init()
        channel.setMethodCallHandler(handle)
        // TODO: initConnectIQ()
    }

    private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "pushState":
            // TODO: forward state dict to paired CIQ app on the watch
            // let args = call.arguments as? [String: Any]
            // connectIQ.sendMessage(args, to: device, through: app, ...)
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // Called by the ConnectIQ SDK delegate when the watch sends a command:
    // channel.invokeMethod("onCommand", arguments: ["type": "speed", "value": 8.5])
}
