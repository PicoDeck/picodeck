"""Hardware-only checks the simulator cannot make (specs/test-audit-2026-09-24.md
§5.3 R15). Run them on a PicoCalc:

    pytest tests/e2e/test_hw_device.py -v \\
        --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

On the simulator they skip (allow-listed through the `hardware` marker).

One probe app, tests/e2e/hw_apps/hw_probe, runs the mode the test writes to
/data/com.test.hw_probe/mode.json; verdicts come from its picotest results
file, never from serial lines (the capture drops them). The audio test is the
exception that proves the rule: the underrun counter is only printed on
serial ([MOD] lines from Core 1), so it needs two captured lines, not all.
"""

from __future__ import annotations

import json
import re
import tempfile
import time
from pathlib import Path

import pytest

from helpers import write_mod
from hw_target import HW_APPS_DIR, pixel

# The first test pushes hw_probe and reboots (the launcher caches app.json):
# allow more than the suite's 60 s.
pytestmark = [pytest.mark.hardware, pytest.mark.timeout(300)]

PROBE = "hw_probe"
PROBE_ID = "com.test.hw_probe"


def _start_probe(target, mode: str):
    """Push hw_probe (once per session), select `mode` and launch it."""
    if not getattr(target, "_hw_probe_pushed", False):
        target.push_app(HW_APPS_DIR / PROBE, PROBE)
        target._hw_probe_pushed = True
    target.write_file(f"/data/{PROBE_ID}/mode.json",
                      json.dumps({"mode": mode}).encode())
    target.delete_file(f"/data/{PROBE_ID}/test_results.json")
    target.launch_app(PROBE)


def _cases(doc: dict) -> dict:
    return {c["name"]: c for c in doc.get("cases", [])}


def _assert_passed(doc: dict):
    bad = {n: c for n, c in _cases(doc).items() if c["status"] != "PASS"}
    assert not bad, "\n".join(f"{n}: {c['status']} {c.get('detail', '')}"
                              for n, c in bad.items())


def _stop_probe(target):
    r = target.exit_app()
    out = target.wait_for_exit(timeout=15)
    assert out["result"] in ("exit_sentinel", "returned"), (r, out)


# Block centres and the exact RGB888 the device's RGB565 red/green/blue/
# white/yellow/black decode to (display.c stores them high byte first).
COLOR_BLOCKS = [
    ((50, 50), (255, 0, 0), "red"),
    ((160, 50), (0, 255, 0), "green"),
    ((270, 50), (0, 0, 255), "blue"),
    ((50, 160), (255, 255, 255), "white"),
    ((160, 160), (255, 255, 0), "yellow"),
    ((270, 160), (0, 0, 0), "black"),
]


def test_display_byte_order_matches_the_simulator(target):
    """display.c byte order: the colour blocks read back from the device's
    framebuffer (the bytes DMA sends to the panel) are the colours the
    simulator's test_display_colors.py sees. A swapped byte order turns red
    into a dark blue-green and blue into a dark green."""
    _start_probe(target, "color")
    try:
        doc = target.wait_for_results(PROBE_ID, timeout=20)
        _assert_passed(doc)
        png = target.screenshot()
        wrong = []
        for (cx, cy), want, name in COLOR_BLOCKS:
            for dx, dy in ((0, 0), (-40, -40), (40, 40), (-40, 40), (40, -40)):
                got = pixel(png, cx + dx, cy + dy)
                if got != want:
                    wrong.append(f"{name} at ({cx + dx},{cy + dy}): {got} != {want}")
        if wrong:
            Path(tempfile.gettempdir(), "picodeck_hw_color.png").write_bytes(png)
        assert not wrong, "\n".join(wrong)
    finally:
        _stop_probe(target)


def test_c_stack_depth_on_device(target):
    """Lua -> C -> Lua recursion (nested gsub callbacks, a 250-deep __index
    function chain) stops with a catchable "C stack overflow" on the real
    stacks, with headroom left: `stack` reports the app and main-stack peaks
    below their sizes, and nothing faulted. Unbounded pure-Lua recursion
    (~976 frames) ends in "stack overflow" with the timer-armed service hook
    still live: before issue #21's fix its arming walked every frame, the
    timer IRQ never returned and the watchdog reset the device mid-test."""
    before = target.crash_evidence()["crashlog"]
    _start_probe(target, "cstack")
    try:
        doc = target.wait_for_results(PROBE_ID, timeout=30)
        _assert_passed(doc)
        st = target.stack()
        assert st, "no `stack` reply"
        assert st.get("app") == "lua", st
        assert st["app_peak"] < st["app_size"] * 0.9, st
        assert st["msp_peak"] < st["msp_size"], st
    finally:
        _stop_probe(target)
    assert target.crash_evidence()["crashlog"] == before, "a crash was logged"


def test_keyboard_edges_on_device(target):
    """keyboard.c on the device: an injected tap is exactly one down, char,
    up; a button tap one down, up; a chord puts the key between the
    modifier's down and up; no tap produces a repeat edge."""
    _start_probe(target, "keys")
    target.wait_for_results(
        PROBE_ID, timeout=20, until=lambda d: "ready" in _cases(d))
    target.keypress_sequence(["a", "enter", "ctrl+x", "z"], delay_ms=250)
    out = target.wait_for_exit(timeout=40)
    doc = target.read_json(f"/data/{PROBE_ID}/test_results.json")
    assert doc.get("done"), doc
    _assert_passed(doc)
    assert out["result"] == "returned", out


MOD_LINE = re.compile(r"\[MOD\] DMA ISR=(\d+) underruns=(\d+)")


def test_audio_stream_has_no_underruns(target):
    """audio.c's stream underrun counter (audio_stream_debug, printed by
    mod_player.c every ~2 s as "[MOD] DMA ISR=.. underruns=..") stays flat
    while a looping MOD plays: Core 1 keeps the DMA ring fed."""
    with tempfile.TemporaryDirectory() as tmp:
        mod = Path(tmp) / "silence.mod"
        write_mod(mod)
        target.write_file(f"/data/{PROBE_ID}/silence.mod", mod.read_bytes())
    _start_probe(target, "audio")
    try:
        doc = target.wait_for_results(PROBE_ID, timeout=20)
        _assert_passed(doc)
        mark = target.log_cursor()
        time.sleep(10.0)  # no commands: nothing holds the port
        lines = [m for m in (MOD_LINE.search(t) for t in target.get_log_lines(mark))
                 if m]
    finally:
        _stop_probe(target)
    assert len(lines) >= 2, (
        f"only {len(lines)} [MOD] lines captured in 10 s (expected ~4; the "
        "capture drops some, but not all)")
    isr = [int(m.group(1)) for m in lines]
    under = [int(m.group(2)) for m in lines]
    assert isr == sorted(isr) and isr[-1] > isr[0], f"DMA ISR count stalled: {isr}"
    assert under[-1] == under[0], f"stream underruns while playing: {under}"
