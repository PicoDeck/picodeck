"""The system menu gives the app its screen back when it closes.

The menu darkens the app's frame into both framebuffers and draws over it.
When it closes, the OS puts both framebuffers back in the roles they had: the
app's front buffer on the panel, its back buffer as the back buffer. So an
app that redraws only what changed (or nothing at all) needs no repaint.

The fixture (apps/menu_restore) clears buffer A red and flushes, then buffer
B blue and flushes, and then never draws: blue is on the panel and red is the
back buffer. Enter makes it flush without drawing, which shows the back
buffer; Esc returns. Its menu item "Quit fixture" calls sys.exit() from
inside the menu callback, which runs after the menu has closed.

Runs on the simulator and on the device (@pytest.mark.both).
"""

from __future__ import annotations

import io
import time
from pathlib import Path

import pytest

pytestmark = [pytest.mark.both, pytest.mark.timeout(300)]

APP = "menu_restore"
APP_DIR = Path(__file__).resolve().parent / "apps" / APP
MEM_FILE = "/data/net.picodeck.menu_restore/mem.txt"

# The menu panel is 200 px wide and centred, so the centre is under it and
# OUTSIDE is not (clear of the clock text at the bottom left too).
CENTRE = (160, 160)
OUTSIDE = (20, 40)
PROBES = {"centre": CENTRE, "outside": OUTSIDE}

BLUE = (0, 0, 255)
RED = (255, 0, 0)
# RGB565 widened to 8 bits per channel lands within a few counts of the
# pure colour. The menu darkens a pixel to about half brightness (blue
# 255 -> ~123), which must fail this check.
TOLERANCE = 40

# An earlier run that leaked the menu's framebuffer copy (2 x 200 KB) would
# leave the next run that much less PSRAM; run-to-run noise is far smaller.
LEAK_TOLERANCE = 64 * 1024


def _pixels(png: bytes) -> dict:
    from PIL import Image
    img = Image.open(io.BytesIO(png)).convert("RGB")
    return {name: tuple(img.getpixel(xy)) for name, xy in PROBES.items()}


def _near(rgb, want) -> bool:
    return all(abs(a - b) <= TOLERANCE for a, b in zip(rgb, want))


def _all(colour):
    return lambda px: all(_near(rgb, colour) for rgb in px.values())


def _wait_screen(target, pred, what: str, timeout: float = 10.0) -> dict:
    """Poll screenshots until pred(pixels) holds (a condition, not a delay)."""
    deadline = time.monotonic() + timeout
    while True:
        px = _pixels(target.screenshot())
        if pred(px):
            return px
        if time.monotonic() >= deadline:
            pytest.fail(f"{what}: probe pixels {px}")
        time.sleep(0.05)


def _launch(target):
    """Start the fixture and wait for its blue front buffer on the panel."""
    target.push_app(APP_DIR, APP)
    target.delete_file(MEM_FILE)
    r = target.launch_app(APP)
    assert r["launched"], r
    _wait_screen(target, _all(BLUE), "the fixture's blue frame never showed",
                 timeout=15.0)


def _read_mem(target, timeout: float = 10.0) -> int:
    deadline = time.monotonic() + timeout
    while True:
        try:
            return int(target.read_file(MEM_FILE).decode().strip())
        except (FileNotFoundError, ValueError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def _open_menu(target):
    target.keypress("menu")
    _wait_screen(target, lambda px: not _near(px["outside"], BLUE),
                 "the system menu did not appear")


def _menu_round_trip_restores_both_buffers(target):
    """Open and close the menu over the running fixture, then show the back
    buffer and quit."""
    _open_menu(target)
    target.keypress("esc")  # read by the menu: closes it
    _wait_screen(target, _all(BLUE),
                 "after the menu closed, the panel does not show the app's "
                 "own frame (a darkened blue means nothing was restored)")

    target.keypress("enter")  # the fixture flushes without drawing
    _wait_screen(target, _all(RED),
                 "the app's back buffer did not survive the menu")

    target.keypress("esc")
    out = target.wait_for_exit(timeout=15)
    assert out["result"] == "returned", out


def test_menu_close_gives_back_both_framebuffers(target):
    _launch(target)
    _menu_round_trip_restores_both_buffers(target)


def test_menu_item_that_exits_leaks_nothing(target):
    """A menu callback that calls sys.exit() leaves through a longjmp. It
    runs after the menu has closed and freed its copies, so nothing leaks
    and the next run's menu still restores the screen."""
    _launch(target)
    before = _read_mem(target)

    _open_menu(target)
    target.keypress("enter")  # the first item is the fixture's "Quit fixture"
    out = target.wait_for_exit(timeout=15)
    # The simulator reports sys.exit() as the exit sentinel; the device
    # cannot tell it from a return.
    assert out["result"] in ("exit_sentinel", "returned"), out
    # The menu cleared the keyboard before the callback ran, so the Enter
    # that chose the item does not reach the launcher and start an app.
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        assert target.status()["app"] == "launcher", (
            "the Enter that chose the menu item reached the launcher")
        time.sleep(0.1)

    _launch(target)
    after = _read_mem(target)
    assert after >= before - LEAK_TOLERANCE, (
        f"PSRAM free fell from {before} to {after} bytes across a menu "
        "callback that called sys.exit()")
    _menu_round_trip_restores_both_buffers(target)
