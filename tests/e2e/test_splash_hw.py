"""The boot splash on the device (src/os/splash.c): reboot, read the
animation's measurements with the `splash` dev command, and check that
Core 1 went back to its own work (specs/2026-10-10-boot-splash-design.md,
"Testing")."""
import re
import time

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(180)]

FIRST_LIGHT_MS = 400      # the first frame, after QMI PSRAM and display init
MIN_FPS = 30
END_MS = 2800             # SPLASH_END_MS
# Boot to the launcher was ~4.9 s before the splash moved to Core 1 (host
# timestamps on a dev `reboot` with a computer attached, 2026-10-10).
HANDOVER_MS = 5200


def splash_stats(target) -> dict:
    for line in target.command("splash", timeout=3.0):
        if "[DEV] Splash:" in line:
            body = line.split("Splash:", 1)[1]
            out = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", body)}
            out["state"] = re.search(r"state=(\w+)", body).group(1)
            return out
    raise AssertionError("no reply to `splash`")


def audio_ticks(target) -> int:
    for line in target.command("audiostat", timeout=3.0):
        m = re.search(r"\bticks=(\d+)", line)
        if m:
            return int(m.group(1))
    raise AssertionError("no reply to `audiostat`")


def test_boot_splash_on_core1(target):
    wifi_before = target.status()["wifi"]
    target.reboot()
    s = splash_stats(target)
    fps = s["anim_frames"] / (END_MS / 1000)
    print(f"splash: {s}, {fps:.1f} fps")
    assert s["state"] == "done", s
    assert s["first_light_ms"] <= FIRST_LIGHT_MS, s
    assert fps >= MIN_FPS, s
    assert s["ended_ms"] <= s["handover_ms"] <= HANDOVER_MS, s
    assert s["waited_ms"] <= END_MS + 500, s
    assert s["foreign_flushes"] == 0, s
    # Core 1 runs core1_entry again: its 1 ms service tick advances...
    a = audio_ticks(target)
    time.sleep(1.0)
    assert audio_ticks(target) > a
    # ...and, where it was online before, WiFi comes back.
    if wifi_before in ("connected", "online"):
        deadline = time.monotonic() + 30
        while target.status()["wifi"] not in ("connected", "online"):
            assert time.monotonic() < deadline, "WiFi did not reconnect after the reboot"
            time.sleep(1.0)
