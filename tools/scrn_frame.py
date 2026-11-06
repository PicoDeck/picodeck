"""The `screenshot` dev command's binary stream, as the host reads it.

The wire format is src/os/serial_frame.h:

    "SCRN" | width u16 | height u16 | format u16 (565) | flags u16    (12 bytes, LE)
    width * height * 2 bytes of pixels
    "\\n[DEV] SCRN_END fnv1a=xxxxxxxx\\n"                    (when flags & FLAG_TRAILER)

The stream shares its CDC port with the device's log, so anything can come
before the header or after the trailer, and a transfer can arrive damaged. The
pixels are found by position, never by scanning, so read_frame either returns
the frame the device hashed or raises.

A device from before the trailer (issue #61) sends flags 0: its frame comes back
with verified=False. An old host never reads the flags or the trailer.

Used by picodeck_mcp.py and screenshot.py; needs nothing but the Python standard
library and a port with pyserial's read/in_waiting/timeout.
"""

from __future__ import annotations

import re
import struct
import time
from dataclasses import dataclass

MAGIC = b"SCRN"
HEADER_SIZE = 12
FLAG_TRAILER = 0x0001
TRAILER_SIZE = 31
_TRAILER_RE = re.compile(rb"\n\[DEV\] SCRN_END fnv1a=([0-9a-f]{8})\n\Z")

MAX_DIMENSION = 1024   # no panel this firmware drives is larger
FORMAT_RGB565 = 565

# Silence that ends a transfer already under way. The firmware gives up on a
# host that stops reading after 0.5 s and sends the rest in well under a second.
STALL_S = 2.0

# More than this much output with no header is not a log line or two: it is a
# frame whose magic was damaged.
_NO_HEADER_LIMIT = 64 * 1024


class ScrnError(RuntimeError):
    """A damaged transfer. The wording carries "corrupted", "integrity" or
    "stalled", which is what picodeck_mcp._retry_transfer retries."""


@dataclass
class Frame:
    width: int
    height: int
    format: int
    pixels: bytes
    verified: bool  # False: a device with no trailer, so nothing was checked


def fnv1a(data: bytes, h: int = 2166136261) -> int:
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def _find_header(ser, timeout: float) -> bytearray:
    """Everything from the magic on. TimeoutError when the device did not
    answer; ScrnError when it sent a frame's worth of bytes with no magic."""
    buf = bytearray()
    seen = 0
    ser.timeout = min(timeout, 0.1)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(max(1, ser.in_waiting))
        if not chunk:
            continue
        seen += len(chunk)
        buf += chunk
        idx = buf.find(MAGIC)
        if idx >= 0:
            return buf[idx:]
        del buf[:-(len(MAGIC) - 1)]
        if seen > _NO_HEADER_LIMIT:
            raise ScrnError(f"corrupted SCRN stream: no header in {seen} bytes")
    raise TimeoutError("Timed out waiting for SCRN header")


def _read_to(ser, buf: bytearray, size: int, what: str):
    while len(buf) < size:
        chunk = ser.read(min(size - len(buf), 4096))
        if not chunk:
            raise ScrnError(f"SCRN transfer stalled in the {what} "
                            f"({len(buf)}/{size} bytes)")
        buf += chunk


def read_frame(ser, timeout: float, stall_s: float = STALL_S) -> Frame:
    """One frame from a port that has just been sent `screenshot`. timeout
    bounds the wait for the header, stall_s every silence after it. Leaves the
    port's timeout as it found it. On an error the rest of the stream is still
    coming: call drain before closing the port."""
    saved = ser.timeout
    try:
        buf = _find_header(ser, timeout)
        ser.timeout = stall_s
        _read_to(ser, buf, HEADER_SIZE, "header")
        width, height, fmt, flags = struct.unpack_from("<HHHH", buf, len(MAGIC))
        if (not 0 < width <= MAX_DIMENSION or not 0 < height <= MAX_DIMENSION
                or fmt != FORMAT_RGB565 or flags & ~FLAG_TRAILER):
            raise ScrnError(f"corrupted SCRN header: {width}x{height} "
                            f"format {fmt} flags 0x{flags:04x}")
        size = HEADER_SIZE + width * height * 2
        _read_to(ser, buf, size, "pixels")
        pixels = bytes(buf[HEADER_SIZE:size])
        if not flags & FLAG_TRAILER:
            return Frame(width, height, fmt, pixels, False)
        del buf[:size]
        _read_to(ser, buf, TRAILER_SIZE, "trailer")
        m = _TRAILER_RE.match(bytes(buf[:TRAILER_SIZE]))
        if not m:
            raise ScrnError("integrity check failed: no SCRN trailer after "
                            "the pixels (the stream gained or lost bytes)")
        if fnv1a(pixels) != int(m.group(1), 16):
            raise ScrnError("integrity check failed (fnv1a mismatch)")
        return Frame(width, height, fmt, pixels, True)
    finally:
        ser.timeout = saved


def drain(ser, quiet_s: float = 0.25, limit_s: float = 3.0):
    """Read and discard until the device has been silent for quiet_s (or for
    limit_s, on a device that never stops logging). A host that closes on a
    half-read stream leaves the device writing to nobody, and the rest of the
    stream in front of the next attempt's header."""
    saved = ser.timeout
    try:
        ser.timeout = min(quiet_s, 0.05)
        now = time.monotonic()
        deadline = now + limit_s
        last = now
        while now < deadline and now - last < quiet_s:
            data = ser.read(4096)
            now = time.monotonic()
            if data:
                last = now
    except OSError:
        pass  # the port went away: nothing is left to drain
    finally:
        try:
            ser.timeout = saved
        except OSError:
            pass
