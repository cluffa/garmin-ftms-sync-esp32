import Toybox.Activity;
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.WatchUi;
import Toybox.Communications;
import Toybox.System;

class DataFieldView extends WatchUi.DataField {
    hidden var mConnectionStatus as String;
    hidden var mLastSendTime as Number;
    hidden var mLastMsgTime as Number;
    hidden var mSpeed as Float;          // treadmill speed from phone (km/h)
    hidden var mTargetPaceStr as String; // formatted target from workout step

    function initialize() {
        DataField.initialize();
        mConnectionStatus = "Disconnected";
        mLastSendTime = 0;
        mLastMsgTime = 0;
        mSpeed = 0.0f;
        mTargetPaceStr = "--:-- tgt";
    }

    function setSpeed(speed as Float) as Void {
        mSpeed = speed;
        mLastMsgTime = System.getTimer();
        WatchUi.requestUpdate();
    }

    function onLayout(dc as Dc) as Void {
    }

    hidden function _paceStrFromMs(speedMs as Float) as String {
        var paceMins = 26.8224f / speedMs; // 1609.34/60 = 26.8224 min/mile per m/s
        var mins = paceMins.toNumber();
        var secs = ((paceMins - mins) * 60).toNumber();
        return mins.toString() + ":" + secs.format("%02d");
    }

    function compute(info as Activity.Info) as Void {
        var now = System.getTimer();
        if (mLastMsgTime > 0 && (now - mLastMsgTime < 10000)) {
            mConnectionStatus = "Connected";
        } else {
            mConnectionStatus = "Disconnected";
        }

        var targetPaceLowMs = 0.0f;
        var targetPaceHighMs = 0.0f;
        var currentSpeedMs = 0.0f;

        if (info != null) {
            if (info.currentSpeed != null) {
                currentSpeedMs = info.currentSpeed as Float;
            }
            if ((info has :currentWorkoutStep) && info.currentWorkoutStep != null) {
                var wStep = info.currentWorkoutStep;
                if ((wStep has :step) && wStep.step != null) {
                    wStep = wStep.step;
                }
                if ((wStep has :targetType) &&
                    wStep.targetType == Activity.WORKOUT_STEP_TARGET_SPEED) {
                    if ((wStep has :targetValueLow) && wStep.targetValueLow != null) {
                        targetPaceLowMs = wStep.targetValueLow as Float;
                    }
                    if ((wStep has :targetValueHigh) && wStep.targetValueHigh != null) {
                        targetPaceHighMs = wStep.targetValueHigh as Float;
                    }
                }
            }
        }

        // Update target pace display string
        var targetMidMs = 0.0f;
        if (targetPaceLowMs > 0.0f && targetPaceHighMs > 0.0f) {
            targetMidMs = (targetPaceLowMs + targetPaceHighMs) / 2.0f;
        } else if (targetPaceLowMs > 0.0f) {
            targetMidMs = targetPaceLowMs;
        } else if (targetPaceHighMs > 0.0f) {
            targetMidMs = targetPaceHighMs;
        }
        mTargetPaceStr = targetMidMs > 0.05f
            ? (_paceStrFromMs(targetMidMs) + " tgt")
            : "--:-- tgt";

        // Send workout status to phone every 5s
        var settings = System.getDeviceSettings();
        var phonePaired = (settings has :phoneConnected) && settings.phoneConnected;
        if (phonePaired && (now - mLastSendTime > 5000)) {
            mLastSendTime = now;
            var msg = {};
            msg.put("type", "workoutStatus");
            var targetPace = targetMidMs > 0.0f ? targetMidMs : currentSpeedMs;
            msg.put("targetPace", targetPace);
            if (targetPaceLowMs > 0.0f) { msg.put("targetPaceLow", targetPaceLowMs); }
            if (targetPaceHighMs > 0.0f) { msg.put("targetPaceHigh", targetPaceHighMs); }
            if (currentSpeedMs > 0.0f) { msg.put("currentSpeed", currentSpeedMs); }
            try {
                Communications.transmit(msg, null, new CommListener());
            } catch (e) {
                System.println("Transmit error: " + e.getErrorMessage());
            }
        }
    }

    function onUpdate(dc as Dc) as Void {
        var fgColor = Graphics.COLOR_BLACK;
        var bgColor = Graphics.COLOR_WHITE;
        if (getBackgroundColor() == Graphics.COLOR_BLACK) {
            fgColor = Graphics.COLOR_WHITE;
            bgColor = Graphics.COLOR_BLACK;
        }
        dc.setColor(fgColor, bgColor);
        dc.clear();

        var w = dc.getWidth();
        var h = dc.getHeight();

        // Treadmill pace from phone (top)
        var paceStr = "--:-- /mi";
        if (mSpeed > 0.1f) {
            paceStr = _paceStrFromMs(mSpeed / 3.6f) + " /mi";
        }
        dc.drawText(w / 2, h / 4, Graphics.FONT_MEDIUM, paceStr,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Workout target pace (middle)
        dc.drawText(w / 2, h / 2, Graphics.FONT_SMALL, mTargetPaceStr,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);

        // Connection status (bottom)
        dc.drawText(w / 2, h * 3 / 4, Graphics.FONT_XTINY, mConnectionStatus,
            Graphics.TEXT_JUSTIFY_CENTER | Graphics.TEXT_JUSTIFY_VCENTER);
    }
}

class CommListener extends Communications.ConnectionListener {
    function initialize() {
        Communications.ConnectionListener.initialize();
    }
    function onComplete() as Void {}
    function onError() as Void {
        System.println("Transmit Error");
    }
}
