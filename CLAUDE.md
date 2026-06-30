# CLAUDE.md

Project-specific notes that aren't obvious from the README or code.

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

## iFit / NordicTrack 6.5S (I_TL) protocol

The treadmill model is **NordicTrack 6.5S** (T6.5S v81), BLE name `I_TL`.

Root cause of zero-notifications: the 6.5S requires a specific **18-command
initialization sequence** written to the write char (0x1534) before it begins
streaming, plus a model-specific 6-phase keepalive poll. Our original code
used wrong poll sequences from a different T-series model and skipped the init
entirely.

Fixed in `machine_ifit.c` (2026-06-29):
- `INIT_00`–`INIT_17`: init sequence from `qdomyos-zwift/proformtreadmill.cpp`,
  sent one command per 500ms timer tick on connect
- `POLL0`–`POLL5`: correct 6-phase keepalive for the 6.5S
- `s_poll_step` state machine in `poll_cb`: runs through all 18 init commands
  first (~9s), then cycles the 6 poll phases

Notification frame format (unchanged, was already correct):
- Header check: bytes 0-3 = `00 12 01 04`
- Speed: `(b[10] | b[11]<<8) / 100` → km/h
- Incline: signed `(b[12] | b[13]<<8) / 100` → %
- Frame length: exactly 20 bytes

Source: https://github.com/cagnulein/qdomyos-zwift (`proformtreadmill.cpp`,
`nordictrack_t65s_treadmill` variant)
