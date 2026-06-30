# CLAUDE.md

> **Note to agents:** `AGENTS.md` and `GEMINI.md` are symlinks to this file. Always edit `CLAUDE.md` to update instructions.

Project-specific notes that aren't obvious from the README or code.

## Commands

```sh
# Firmware — build or build+flash
./build.sh heltec-v3               # build only
./build.sh heltec-v3 flash         # build + flash (auto-detects port)
./build.sh heltec-v3 flash /dev/cu.usbserial-0001

# Flutter phone app
cd app && flutter run              # run on connected device
cd app && flutter build apk        # release APK

# Garmin Connect IQ data field (requires Connect IQ SDK + developer key)
# CI uses blackshadev/garmin-connectiq-build-action; locally use monkeyc CLI:
# monkeyc -f garmin_data_field/monkey.jungle -d fenix8solar51mm -o app.prg
```

## Building (local macOS quirk)

The ESP-IDF Python venv here is for **3.13**, but the system `python3` is 3.14, so
a plain `source ~/esp/esp-idf/export.sh` fails ("virtual environment … not found").
Pin the interpreter first:

```sh
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env
export ESP_PYTHON=/opt/homebrew/bin/python3.13
source ~/esp/esp-idf/export.sh
```

## Targets

| Board | dir | target |
|-------|-----|--------|
| Heltec WiFi LoRa 32 v3 | `boards/heltec-v3` | **`esp32s3`** |
| Seeed XIAO ESP32-C6 | `boards/xiao-c6` | `esp32c6` |

If a `boards/*/build` dir has a stale target, `battery.c` fails to compile
(`adc_cali_create_scheme_curve_fitting` is S3-only). Fix: `idf.py set-target esp32s3`.

## Debugging without the button

UART0 (`/dev/cu.usbserial-0001` on the Heltec) is **both** the console log and the
serial command interface (`serial_ctrl.c`): `SCAN` / `LIST` / `CONNECT <idx>` /
`STATUS` / `SPEED` / `INCLINE` / `STOP`, plus pushed `{"event":"state",…}` JSON.
Drive a device switch and read live incline/speed over this port without touching
the hardware. NimBLE GATT verbose logs (iFit keepalive writes to `att_handle=14`)
flood this port — filter them when reading.

## Architecture invariant: one machine connection at a time

`machine_ftms` and `machine_ifit` are separate BLE-central adapters but feed **one
shared state callback** (`machine_set_data_cb`). They can each hold an independent
connection, so if both are connected the two notification streams clobber each
other in the shared state — e.g. an FTMS sensor's incline showing up under an iFit
label. `machine_connect()` therefore tears down the *other* protocol
(`machine_{ftms,ifit}_disconnect()`, which also cancels an in-flight connect)
before connecting, and `on_evt()` suppresses auto-reconnect while the other
protocol is connecting/connected. **Do not reintroduce simultaneous connections.**

## Forward Data Stream (Treadmill -> Garmin)

The primary implemented data stream broadcasts treadmill metrics to the watch:
* `Treadmill (FTMS/iFit)` -> `ESP32 (BLE Central)` -> `Garmin Watch (BLE RSC Peripheral)`
* The ESP32 connects to the treadmill, parses its proprietary or standard data frames into a generic `treadmill_state_t` via `bridge_core`, and then `garmin_rsc.c` handles exposing it as a standard BLE Running Speed and Cadence (RSC) sensor that any Garmin watch can pair with.

## Reverse Data Stream (Garmin -> Treadmill) (TODO)

There is a pending feature to automatically control treadmill speed based on Garmin workout pace targets via a reverse data stream: `Garmin Watch (ConnectIQ)` -> `Phone App` -> `ESP32` -> `Treadmill`.

Implementation status across pending branches:
1. **Watch (`garmin-data-field` branch):** DataField reads `Activity.Info.currentWorkoutStep` for target pace (m/s); falls back to `currentSpeed`. Sends `workoutStatus` with `targetPace`, `targetPaceLow`, `targetPaceHigh` every 5s.
2. **Phone App (`garmin-data-field` branch):** `garmin_ciq_service.dart` handles `workoutStatus` messages — converts target pace from m/s to km/h and calls `_bridge.setSpeed()`; exposes `targetSpeedLowKmh`/`targetSpeedHighKmh` getters.
3. **Phone -> ESP32 Link (`feat/ble-nus-phone-control` branch):** Fully implemented. The Flutter app uses BLE NUS to send control strings (e.g. `speed 10.0`) to the ESP32.
4. **ESP32 -> Treadmill:** Fully implemented. `nus_ctrl.c` and `ctrl_dispatch.c` parse incoming speed commands and apply them to the treadmill via FTMS/iFit.


## iFit / NordicTrack 6.5S (I_TL) protocol

The treadmill model is **NordicTrack 6.5S** (T6.5S v81), BLE name `I_TL`.

Requires an **18-command init sequence** on write char `0x1534` before it streams,
then a **6-phase keepalive poll** — both implemented in `machine_ifit.c` via the
`s_poll_step` state machine (init takes ~9s, then poll cycles). Source:
`qdomyos-zwift/proformtreadmill.cpp` (`nordictrack_t65s_treadmill` variant).

Notification frame format:
- Header: bytes 0-3 = `00 12 01 04`
- Speed: `(b[10] | b[11]<<8) / 100` → km/h
- Incline: signed `(b[12] | b[13]<<8) / 100` → %
- Frame length: exactly 20 bytes
