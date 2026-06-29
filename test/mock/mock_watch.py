#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Mock Garmin watch: connect to the bridge's RSC service and log notifications."""
import asyncio, struct
from bleak import BleakScanner, BleakClient

RSC_SVC  = "00001814-0000-1000-8000-00805f9b34fb"
RSC_MEAS = "00002a53-0000-1000-8000-00805f9b34fb"
RSC_FEAT = "00002a54-0000-1000-8000-00805f9b34fb"
NAME = "garmin-ftms-sync"

def parse_rsc(b):
    flags = b[0]
    speed = struct.unpack_from("<H", b, 1)[0] / 256.0   # m/s
    cad   = b[3]
    off = 4
    stride = dist = None
    if flags & 0x01:
        stride = struct.unpack_from("<H", b, off)[0]/100.0; off += 2
    if flags & 0x02:
        dist = struct.unpack_from("<I", b, off)[0]/10.0; off += 4
    running = bool(flags & 0x04)
    return f"flags=0x{flags:02x} speed={speed:.2f}m/s cad={cad} dist={dist} running={running}"

async def main():
    print("scanning for", NAME, "...")
    dev = await BleakScanner.find_device_by_name(NAME, timeout=15)
    if not dev:
        # fall back: scan by service uuid
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: RSC_SVC in (ad.service_uuids or []), timeout=10)
    if not dev:
        print("DEVICE NOT FOUND"); return
    print("found:", dev.address, dev.name)
    async with BleakClient(dev) as c:
        print("connected:", c.is_connected)
        svcs = c.services
        rsc = svcs.get_service(RSC_SVC)
        print("RSC service present:", rsc is not None)
        feat = await c.read_gatt_char(RSC_FEAT)
        print("RSC Feature bytes:", feat.hex(), "=> 0x%04x" % struct.unpack("<H", feat)[0],
              "(expect 0x0006)")
        got = []
        def cb(_, data):
            got.append(bytes(data))
            print("  NOTIFY:", parse_rsc(bytes(data)))
        await c.start_notify(RSC_MEAS, cb)
        print("subscribed; listening 12s for notifications...")
        await asyncio.sleep(12)
        await c.stop_notify(RSC_MEAS)
        print(f"RESULT: {len(got)} notifications received")

asyncio.run(main())
