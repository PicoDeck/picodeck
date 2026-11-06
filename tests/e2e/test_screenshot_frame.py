"""The `screenshot` stream end to end (issue #61): a frame the simulator builds
with the firmware's own framing code (src/os/serial_frame.h, the RPC's format
"serial"), read by the host's own reader (tools/picodeck_mcp.py) over a port
that damages it.

test_hw_target_protocol.py holds the reader to the format as the tests spell it;
this holds the format's two real ends to each other. The device's console gate is
firmware only: test_screenshot_integrity.py has the stress on real hardware.
"""

from __future__ import annotations

import base64
import io

import pytest

import hw_target

pm = hw_target.load_picodeck_mcp()

pytestmark = [pytest.mark.sd(fixtures=[], reserve=1)]

APP = "scrn_frame"

# Every pixel a function of its place, and no byte pair a palindrome, so a shift
# or a byte-order mistake changes the picture.
FIXTURE = """
local d = picocalc.display
for y = 0, 319 do
    for x = 0, 319 do
        d.setPixel(x, y, ((x * 7 + y * 13) * 257 + y) & 0xFFFF)
    end
end
d.flush()
picocalc.sys.log("SF:READY")
while true do
    picocalc.sys.resetIdleTimer()
    picocalc.sys.sleep(20)
end
"""


class StreamPort:
    """The pyserial calls the reader makes, over the bytes each `screenshot`
    answers with."""

    def __init__(self, streams):
        self.streams = streams
        self.out = bytearray()
        self.commands = 0
        self.timeout = 0.2
        self.unread_at_close = 0

    in_waiting = property(lambda self: len(self.out))

    def write(self, data):
        assert data == b"screenshot\n"
        self.commands += 1
        self.out += self.streams.pop(0) if self.streams else b""

    def flush(self):
        pass

    def read(self, n=1):
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def close(self):
        self.unread_at_close += len(self.out)
        self.out.clear()


def _reader(monkeypatch, streams) -> StreamPort:
    port = StreamPort(streams)
    monkeypatch.setattr(pm, "open_serial", lambda p, timeout=5: port)
    monkeypatch.setattr(pm, "HAS_SERIAL", True)
    monkeypatch.setattr(pm, "SCRN_STALL_S", 0.05)
    monkeypatch.setattr(pm, "RETRY_DELAY_S", 0.0)
    return port


@pytest.fixture
def showing(simulator):
    """The simulator running the fixture."""
    target = hw_target.SimTarget(simulator)
    target.stage_lua_app(APP, FIXTURE)
    mark = target.log_cursor()
    assert target.launch_app(APP)["launched"]
    target.wait_for_log("SF:READY", timeout=20, since=mark)
    return simulator


def _stream(sim) -> bytes:
    return base64.b64decode(sim.call("screenshot", {"format": "serial"})["data"])


def _rgb(png: bytes):
    from PIL import Image
    return Image.open(io.BytesIO(png)).convert("RGB")


def test_host_reads_the_firmware_framing(showing, monkeypatch):
    """Header, pixels in panel order, trailer: the reader's picture is the
    picture the simulator's own screenshot shows, to the pixel."""
    stream = _stream(showing)
    assert stream[:4] == b"SCRN"
    port = _reader(monkeypatch, [stream])
    png = pm.do_screenshot_hardware("/dev/fake", timeout=0.5)
    assert port.commands == 1
    assert _rgb(png).tobytes() == _rgb(showing.screenshot()).tobytes()
    # The reader's frame is verified: the bytes it took are the RPC's raw frame,
    # byte-swapped (host order there, panel order on the wire).
    raw = base64.b64decode(showing.call("screenshot", {"format": "raw"})["data"])
    swapped = bytearray(len(raw))
    swapped[0::2], swapped[1::2] = raw[1::2], raw[0::2]
    assert stream[12:12 + len(raw)] == bytes(swapped)


@pytest.mark.parametrize("name, damage", [
    ("log_line_mid_frame", lambda s: s[:100012] + b"[HTTP] closed\r\n" + s[100012:]),
    ("lost_bytes", lambda s: s[:50012] + s[50019:]),
    ("truncated", lambda s: s[:100012]),
    ("flipped_bit", lambda s: s[:5000] + bytes([s[5000] ^ 1]) + s[5001:]),
])
def test_damaged_real_frame_is_retried(showing, monkeypatch, name, damage):
    stream = _stream(showing)
    port = _reader(monkeypatch, [damage(stream), stream])
    png = pm.do_screenshot_hardware("/dev/fake", timeout=0.5)
    assert port.commands == 2, name
    assert port.unread_at_close == 0, name
    assert _rgb(png).tobytes() == _rgb(showing.screenshot()).tobytes(), name


@pytest.mark.parametrize("name, damage", [
    ("log_line_mid_frame", lambda s: s[:100012] + b"[HTTP] closed\r\n" + s[100012:]),
    ("flipped_bit", lambda s: s[:5000] + bytes([s[5000] ^ 1]) + s[5001:]),
])
def test_damaged_real_frame_keeps_failing_raises(showing, monkeypatch, name, damage):
    stream = damage(_stream(showing))
    port = _reader(monkeypatch, [stream] * 5)
    with pytest.raises(RuntimeError, match="attempts"):
        pm.do_screenshot_hardware("/dev/fake", timeout=0.5)
    assert port.commands == 3, name
