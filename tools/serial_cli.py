#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""
Mac test CLI for the ftms-bridge serial command interface.

Usage:
  ./serial_cli.py                   # auto-detect port
  ./serial_cli.py /dev/cu.usbserial-0001

Commands at the prompt:
  scan          — start BLE scan on device
  list          — list discovered treadmills
  connect <n>   — connect to device at index n
  speed <kmh>   — set target speed in km/h
  incline <pct> — set target incline in %
  status        — show current treadmill state
  quit / Ctrl-D — exit
"""
import sys, json, threading, time
import serial, serial.tools.list_ports

# ---- port detection -------------------------------------------------------

def find_port():
    candidates = []
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        vid = p.vid
        # CP210x (Heltec V3), CH340, FTDI, or ESP32-C6 native USB CDC
        if vid in (0x10C4, 0x1A86, 0x0403, 0x303A) or "cp210" in desc or "ch340" in desc:
            candidates.append(p.device)
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        print("Multiple USB-serial ports found:")
        for i, c in enumerate(candidates):
            print(f"  {i}: {c}")
        idx = input("Select [0]: ").strip() or "0"
        return candidates[int(idx)]
    # fallback: show all and let user pick
    all_ports = [p.device for p in serial.tools.list_ports.comports()]
    if not all_ports:
        sys.exit("No serial ports found. Is the device plugged in?")
    print("All serial ports:")
    for i, p in enumerate(all_ports):
        print(f"  {i}: {p}")
    idx = input("Select [0]: ").strip() or "0"
    return all_ports[int(idx)]

# ---- background reader ----------------------------------------------------

def reader(ser):
    """Print JSON events/responses from the device."""
    buf = b""
    while True:
        try:
            chunk = ser.read(256)
        except Exception:
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                print(f"\r< {json.dumps(obj)}")
            except Exception:
                print(f"\r< {line.decode(errors='replace')}")
            # re-print the prompt without a newline
            print("> ", end="", flush=True)

# ---- main -----------------------------------------------------------------

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    print(f"Opening {port} at 115200…")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(0.5)  # let device settle after DTR toggle

    t = threading.Thread(target=reader, args=(ser,), daemon=True)
    t.start()

    print("Type 'help' for commands. Ctrl-D or 'quit' to exit.\n")
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        low = line.lower()
        if low in ("quit", "exit", "q"):
            break
        if low == "help":
            print("  scan | list | connect <n> | speed <kmh> | incline <pct> | stop | status")
            continue
        # map friendly aliases to wire commands
        if low == "scan":
            cmd = "SCAN"
        elif low == "list":
            cmd = "LIST"
        elif low == "status":
            cmd = "STATUS"
        elif low.startswith("connect "):
            cmd = f"CONNECT {low.split(None,1)[1]}"
        elif low.startswith("speed "):
            cmd = f"SPEED {low.split(None,1)[1]}"
        elif low.startswith("incline "):
            cmd = f"INCLINE {low.split(None,1)[1]}"
        elif low == "stop":
            cmd = "STOP"
        else:
            cmd = line.upper()
        ser.write((cmd + "\n").encode())

    ser.close()
    print("bye")

if __name__ == "__main__":
    main()
