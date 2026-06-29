#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Log every nearby RSC and FTMS sensor concurrently.

Scans for BLE devices advertising Running Speed & Cadence (0x1814) or the
Fitness Machine Service (0x1826), connects to all of them at once, subscribes
to their measurement characteristics, and prints + CSV-logs timestamped,
parsed notifications from every device simultaneously. Ctrl-C to stop.

Useful for testing the bridge (which presents RSC) and a real treadmill (FTMS)
side by side. Note: many fitness machines accept only ONE BLE connection, so if
the bridge already holds the treadmill you won't also be able to read it
directly here — that's a device limit, not the script.

Run:  ./venv/bin/python sensor_logger.py   (needs: pip install bleak)
"""
import asyncio, json, struct, sys, time
from bleak import BleakScanner, BleakClient

def uuid16(x): return f"0000{x:04x}-0000-1000-8000-00805f9b34fb"

RSC_SVC   = uuid16(0x1814)
FTMS_SVC  = uuid16(0x1826)
WANT_SVCS = {RSC_SVC, FTMS_SVC}

RSC_MEAS   = uuid16(0x2A53)
TREADMILL  = uuid16(0x2ACD)
BIKE       = uuid16(0x2AD2)
ROWER      = uuid16(0x2AD1)
CROSS      = uuid16(0x2ACE)

SCAN_SECONDS = 8


class R:
    """Little-endian cursor reader over a bytes payload."""
    def __init__(self, b): self.b, self.i = b, 0
    def u8(self):  v = self.b[self.i]; self.i += 1; return v
    def u16(self): v = struct.unpack_from("<H", self.b, self.i)[0]; self.i += 2; return v
    def s16(self): v = struct.unpack_from("<h", self.b, self.i)[0]; self.i += 2; return v
    def u24(self): v = self.b[self.i] | (self.b[self.i+1] << 8) | (self.b[self.i+2] << 16); self.i += 3; return v
    def u32(self): v = struct.unpack_from("<I", self.b, self.i)[0]; self.i += 4; return v


def parse_rsc(b):
    f = b[0]; r = R(b); r.i = 1
    d = {"speed_mps": round(r.u16() / 256.0, 3), "cadence_spm": r.u8(),
         "running": bool(f & 0x04)}
    if f & 0x01: d["stride_m"] = r.u16() / 100.0
    if f & 0x02: d["dist_m"] = r.u32() / 10.0
    return d


def parse_treadmill(b):
    r = R(b); f = r.u16(); d = {}
    if not (f & (1 << 0)): d["speed_kmh"] = round(r.u16() / 100.0, 2)
    if f & (1 << 1):  d["avg_speed_kmh"] = round(r.u16() / 100.0, 2)
    if f & (1 << 2):  d["dist_m"] = r.u24()
    if f & (1 << 3):  d["incline_pct"] = r.s16() / 10.0; d["ramp_deg"] = r.s16() / 10.0
    if f & (1 << 4):  d["elev_pos_m"] = r.u16() / 10.0; d["elev_neg_m"] = r.u16() / 10.0
    if f & (1 << 5):  d["inst_pace"] = r.u8()
    if f & (1 << 6):  d["avg_pace"] = r.u8()
    if f & (1 << 7):  d["energy_kcal"] = r.u16(); d["energy_per_hr"] = r.u16(); d["energy_per_min"] = r.u8()
    if f & (1 << 8):  d["hr"] = r.u8()
    if f & (1 << 9):  d["met"] = r.u8() / 10.0
    if f & (1 << 10): d["elapsed_s"] = r.u16()
    if f & (1 << 11): d["remaining_s"] = r.u16()
    if f & (1 << 12): d["force_n"] = r.s16(); d["power_w"] = r.s16()
    return d


def parse_bike(b):
    r = R(b); f = r.u16(); d = {}
    if not (f & (1 << 0)): d["speed_kmh"] = round(r.u16() / 100.0, 2)
    if f & (1 << 1):  d["avg_speed_kmh"] = round(r.u16() / 100.0, 2)
    if f & (1 << 2):  d["cadence_rpm"] = r.u16() / 2.0
    if f & (1 << 3):  d["avg_cadence_rpm"] = r.u16() / 2.0
    if f & (1 << 4):  d["dist_m"] = r.u24()
    if f & (1 << 5):  d["resistance"] = r.s16()
    if f & (1 << 6):  d["power_w"] = r.s16()
    if f & (1 << 7):  d["avg_power_w"] = r.s16()
    if f & (1 << 8):  d["energy_kcal"] = r.u16(); d["energy_per_hr"] = r.u16(); d["energy_per_min"] = r.u8()
    if f & (1 << 9):  d["hr"] = r.u8()
    if f & (1 << 10): d["met"] = r.u8() / 10.0
    if f & (1 << 11): d["elapsed_s"] = r.u16()
    if f & (1 << 12): d["remaining_s"] = r.u16()
    return d


PARSERS = {  # char uuid -> (label, parser)
    RSC_MEAS:  ("RSC",       parse_rsc),
    TREADMILL: ("Treadmill", parse_treadmill),
    BIKE:      ("Bike",      parse_bike),
    ROWER:     ("Rower",     None),   # raw-logged
    CROSS:     ("CrossTrn",  None),
}

_csv = open(time.strftime("sensor_log_%Y%m%d_%H%M%S.csv"), "w", buffering=1)
_csv.write("epoch,iso_time,device,sensor,parsed,raw_hex\n")
_t0 = time.perf_counter()


def log(device, sensor, data, parsed):
    now = time.time()
    raw = data.hex()
    pj = json.dumps(parsed, separators=(",", ":")) if parsed is not None else ""
    iso = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int(now % 1 * 1000):03d}"
    rel = time.perf_counter() - _t0
    print(f"[{rel:7.2f}s] {device:18.18s} {sensor:9s} "
          f"{pj if pj else raw}", flush=True)
    _csv.write(f"{now:.3f},{iso},{device},{sensor},{json.dumps(parsed) if parsed is not None else ''},{raw}\n")


async def handle_device(dev):
    label = dev.name or dev.address
    try:
        async with BleakClient(dev) as c:
            print(f"  connected: {label} ({dev.address})")
            subscribed = 0
            for svc in c.services:
                for ch in svc.characteristics:
                    if "notify" not in ch.properties:
                        continue
                    uid = ch.uuid.lower()
                    if uid not in PARSERS:
                        continue
                    name, parser = PARSERS[uid]
                    def cb(_h, data, name=name, parser=parser, label=label):
                        try:
                            parsed = parser(bytes(data)) if parser else None
                        except Exception as e:
                            parsed = {"parse_error": str(e)}
                        log(label, name, bytes(data), parsed)
                    await c.start_notify(ch, cb)
                    subscribed += 1
                    print(f"    subscribed {label} -> {name}")
            if subscribed == 0:
                print(f"    (no known RSC/FTMS measurement notify char on {label})")
                return
            while c.is_connected:
                await asyncio.sleep(1)
            print(f"  disconnected: {label}")
    except Exception as e:
        print(f"  ! {label}: {e}")


async def main():
    print(f"scanning {SCAN_SECONDS}s for RSC/FTMS devices...")
    found = await BleakScanner.discover(timeout=SCAN_SECONDS, return_adv=True)
    targets = []
    for dev, adv in found.values():
        svcs = {s.lower() for s in (adv.service_uuids or [])}
        if svcs & WANT_SVCS:
            kinds = []
            if RSC_SVC in svcs: kinds.append("RSC")
            if FTMS_SVC in svcs: kinds.append("FTMS")
            print(f"  found {dev.name or dev.address} [{','.join(kinds)}] rssi={adv.rssi}")
            targets.append(dev)
    if not targets:
        print("no RSC/FTMS devices advertising. (Already-connected machines may "
              "not advertise — disconnect other apps and retry.)")
        return
    print(f"connecting to {len(targets)} device(s); Ctrl-C to stop. "
          f"logging to {_csv.name}\n")
    await asyncio.gather(*(handle_device(d) for d in targets))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        _csv.close()
