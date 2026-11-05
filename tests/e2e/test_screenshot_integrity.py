"""A frame that does not change reads back unchanged, however much is being
logged while it transfers (issue #61).

The first test runs on both targets. On the device the two after it are the
stress: `lognoise` logs from Core 1 and from a Core 0 interrupt (the places the
console gate has to stop), and each transfer goes through the reader's single
attempt, so a tear fails here instead of being retried away. They prove the gate:
a clean run means nothing landed inside a transfer, and the gate's count of
dropped bytes means there was something to stop.
"""

from __future__ import annotations

import io
import re

import pytest

pytestmark = [pytest.mark.timeout(300), pytest.mark.sd(fixtures=[], reserve=1)]

APP = "scrn_static"
APP_ID = "net.picodeck.scrn_static"

# Stripes in every colour field: a shifted stream cannot look like this frame. It
# flushes once, so nothing the OS draws at a present can change the frame.
FIXTURE = r"""
local d, sys = picocalc.display, picocalc.sys
for y = 0, 319 do
    d.fillRect(0, y, 320, 1, (y * 331) & 0xFFFF)
end
d.flush()
local T = sys.loadlib("picotest")
T.done()
local n = 0
while true do
    sys.resetIdleTimer()
    n = n + 1
    if n % 8 == 0 then sys.log("noise " .. n) end
    sys.sleep(5)
end
"""


def _start(target):
    target.ensure_test_kit()
    target.stage_lua_app(APP, FIXTURE, id=APP_ID)
    target.delete_file(f"/data/{APP_ID}/test_results.json")
    assert target.launch_app(APP)["launched"]
    target.wait_for_results(APP_ID, timeout=20)


def _pixels(png: bytes) -> bytes:
    from PIL import Image
    return Image.open(io.BytesIO(png)).convert("RGB").tobytes()


def _gate_counts(target) -> tuple:
    for line in target.command("lognoise", timeout=3.0):
        m = re.search(r"LogNoise: period_ms=(\d+) transfers=(\d+) dropped_bytes=(\d+)",
                      line)
        if m:
            return tuple(int(g) for g in m.groups())
    raise AssertionError("no LogNoise reply (firmware without the console gate?)")


@pytest.fixture
def running(target):
    _start(target)
    yield target
    if target.kind == "hw":
        target.command("lognoise off", timeout=3.0)
    target.exit_app()


@pytest.fixture
def noisy(running):
    """The fixture app, with the device logging every 2 ms from Core 1 and from
    a Core 0 interrupt on top."""
    running.command("lognoise 2", timeout=3.0)
    assert _gate_counts(running)[0] == 2
    return running


@pytest.mark.both
def test_static_frame_stays_the_same(running):
    first = _pixels(running.screenshot())
    assert len(set(first)) > 2, "the fixture's stripes are not on screen"
    for i in range(1, 12):
        assert _pixels(running.screenshot()) == first, f"frame {i} differs"


@pytest.mark.hardware
def test_binary_screenshot_under_log_noise(noisy):
    """One attempt each: the host's retry would hide a tear."""
    pm = noisy.pm
    _, transfers0, dropped0 = _gate_counts(noisy)
    with pm._exclusive_serial(noisy.port):
        first = _pixels(pm._do_screenshot_hardware_once(noisy.port, timeout=10.0))
        for i in range(1, 30):
            png = pm._do_screenshot_hardware_once(noisy.port, timeout=10.0)
            assert _pixels(png) == first, f"frame {i} differs"
    _, transfers, dropped = _gate_counts(noisy)
    assert transfers - transfers0 == 30
    assert dropped > dropped0, "no log output met a transfer: not a stress"


@pytest.mark.hardware
def test_base64_transfers_under_log_noise(noisy):
    """The line-framed transfers: their own lines pass and the noise does not.
    A getb64 of a known file, and a screenshot64, again with one attempt each."""
    pm = noisy.pm
    data = bytes(range(256)) * 16
    path = f"/data/{APP_ID}/noise.bin"
    noisy.write_file(path, data)
    with pm._exclusive_serial(noisy.port):
        for i in range(8):
            assert pm._do_get_file_b64_once(noisy.port, path) == data, f"read {i}"
        first = _pixels(pm._do_screenshot_b64_once(noisy.port))
        assert _pixels(pm._do_screenshot_b64_once(noisy.port)) == first
    noisy.delete_file(path)
