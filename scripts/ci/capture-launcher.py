#!/usr/bin/env python3
"""Capture the launcher as the web demo shows it, for picodeck.net's home page.

Runs the native simulator headless on a copy of the web demo's SD image (the
apps picodeck.net/try runs) and saves a 320x320 PNG of the screen once the
launcher is up. Not in --test-mode, which pins the header clock to a fixed
date: like the web demo, the clock stays unset and isn't drawn. The mock WiFi
is always up; the battery is set to full. capture-launcher.sh adds it to picodeck-web-sim.zip
as launcher.png.

Usage: capture-launcher.py SIMULATOR_BINARY WEB_SD_DIR OUT_PNG
"""
import base64
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "e2e"))
from picodeck_simulator import PicodeckSimulator  # noqa: E402

SEL_BG = 0x2A94          # RGB565(40, 80, 160): launcher.c C_SEL_BG, the selection bar
SEL_PROBE = (200, 57)    # inside row 0's bar (list at y=42, 32px rows)


def launcher_drawn(sim) -> bool:
    """True once the launcher list is on screen with its first app selected."""
    fb = base64.b64decode(sim.call("screenshot", {"format": "raw"})["data"])
    x, y = SEL_PROBE
    (px,) = struct.unpack_from("<H", fb, (y * 320 + x) * 2)
    return px in (SEL_BG, ((SEL_BG & 0xFF) << 8) | (SEL_BG >> 8))  # either byte order


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    binary, sd_dir, out = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as tmp:
        # The simulator writes config and saves to its SD card; keep the build's clean.
        sd = Path(tmp) / "sd"
        shutil.copytree(sd_dir, sd)
        sim = PicodeckSimulator(binary_path=binary, sd_card_path=str(sd), headless=True)
        sim.start()
        try:
            deadline = time.monotonic() + 30
            while not launcher_drawn(sim):
                if time.monotonic() > deadline:
                    raise TimeoutError("the launcher did not appear within 30 s")
                time.sleep(0.25)
            sim.call("set_battery", {"percent": 100, "charging": False})
            time.sleep(1.5)  # a few frames for the header to redraw
            png = sim.screenshot()
        finally:
            sim.stop()
    if not png.startswith(b"\x89PNG"):
        print("capture-launcher: the screenshot is not a PNG", file=sys.stderr)
        return 1
    Path(out).write_bytes(png)
    print(f"capture-launcher: {out} ({len(png)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
