"""The OS FPS counter (system menu -> Settings -> Show FPS; src/os/os_overlay.c).

The OS counts presents (or perf.endFrame ticks) and draws "FPS: n" in an
opaque black 52x12 box at the corner chosen by the `show_fps` key of
/system/config.json, over every present: flush, flushRows and flushRegion.
For a partial present whose rows miss the box, the OS pushes the box as a
small window write when its text changes, so the probes below read the
PRESENTED screen (the simulator's GRAM analog), not a framebuffer.

The fixtures paint the whole screen blue, so black pixels can only come from
the counter box, and its text is one of the drawFPS colours (green, yellow,
red; grey for "--" before the first 1 s window completes).
"""

import json
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import stage_lua_app

BOX_W, BOX_H = 52, 12
CORNERS = {"tr": (262, 22), "tl": (6, 22), "br": (262, 302), "bl": (6, 302)}
BG = (0, 0, 255)                     # 0x001F, the fixtures' background
NUMBER_INKS = {(0, 255, 0), (255, 255, 0), (255, 0, 0)}
DASH_INK = (132, 130, 132)           # COLOR_GRAY, "FPS: --"

# Toast geometry (ui_widget_toast at y 280): "TOAST" is 30px wide + 2x8
# padding, centred.
TOAST_Y, TOAST_H = 280, 14
TOAST_W = 5 * 6 + 16
TOAST_X = (320 - TOAST_W) // 2

FIXTURE = """
local d = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local MODE, Y0, Y1 = "{mode}", {y0}, {y1}
{setup}
d.clear(0x001F)
if MODE == "flush" then d.flush() else d.flushRows(0, 319) end
local n = 0
while true do
    input.update()
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then return end
    if p & input.BTN_ENTER ~= 0 then picocalc.ui.toast("TOAST") end
    n = n + 1
    local c = (n % 2 == 0) and 0xFFFF or 0x07FF   -- white / cyan band
    if MODE == "flush" then
        d.clear(0x001F)
        d.fillRect(0, 150, 320, 10, c)
        d.flush()
    else
        d.fillRect(0, Y0, 320, Y1 - Y0 + 1, 0x001F)
        if Y0 <= 150 and Y1 >= 159 then d.fillRect(0, 150, 320, 10, c) end
        d.flushRows(Y0, Y1)
    end
    if n == 3 then sys.log("FPSFIX:READY") end
    sys.sleep(16)
end
"""


def _rgb(img) -> np.ndarray:
    return np.array(img.convert("RGB")).astype(int)


def _box(arr, corner):
    x, y = CORNERS[corner]
    return arr[y:y + BOX_H, x:x + BOX_W]


def _colours(region) -> set:
    return {tuple(p) for p in region.reshape(-1, 3)}


def counter_at(arr, corner, number=True) -> bool:
    """The counter box at `corner`: a black frame around the text, text in
    the colour code (a number) or grey ("--"), and the fixture's blue just
    outside it (the box is exactly BOX_W x BOX_H)."""
    b = _box(arr, corner)
    frame = np.concatenate([b[0], b[-1], b[:, 0], b[:, -1]])
    if frame.any():
        return False
    ink = _colours(b) - {(0, 0, 0)}
    allowed = NUMBER_INKS if number else NUMBER_INKS | {DASH_INK}
    if not ink or not ink <= allowed:
        return False
    x, y = CORNERS[corner]
    return tuple(arr[y - 1, x]) == BG and tuple(arr[y + BOX_H, x + BOX_W]) == BG


def untouched(arr, corner) -> bool:
    return _colours(_box(arr, corner)) == {BG}


def wait_screen(sim, pred, timeout=8.0):
    deadline = time.time() + timeout
    while True:
        arr = _rgb(sim.screenshot_pil())
        if pred(arr) or time.time() >= deadline:
            return arr
        time.sleep(0.05)


def boot(sim_factory, sd, show_fps=None, **kwargs):
    if show_fps is not None:
        (sd / "system" / "config.json").write_text(json.dumps({"show_fps": show_fps}))
    return sim_factory(sd, **kwargs)


def run_fixture(sim, name, mode="flush", y0=0, y1=319, setup=""):
    stage_lua_app(Path(sim.sd_card_path), name,
                  FIXTURE.format(mode=mode, y0=y0, y1=y1, setup=setup))
    sim.launch_app(name)
    sim.wait_for_log("FPSFIX:READY", timeout=15)


# ── Off (the default) ───────────────────────────────────────────────────────


@pytest.mark.parametrize("mode", ["flush", "rows"])
def test_off_by_default_leaves_every_corner_untouched(sim_factory, test_sd_card, mode):
    sim = boot(sim_factory, test_sd_card)
    run_fixture(sim, f"fps_off_{mode}", mode, 150, 159)
    time.sleep(1.5)  # past the first 1 s window
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    for corner in CORNERS:
        assert untouched(arr, corner), f"{corner} corner drawn with show_fps off"


# ── Each corner, flush ──────────────────────────────────────────────────────


@pytest.mark.parametrize("corner", list(CORNERS))
def test_counter_at_chosen_corner_flush(sim_factory, test_sd_card, corner):
    sim = boot(sim_factory, test_sd_card, show_fps=corner)
    run_fixture(sim, f"fps_flush_{corner}")
    arr = wait_screen(sim, lambda a: counter_at(a, corner))
    assert counter_at(arr, corner), f"no FPS counter at {corner}"
    for other in CORNERS:
        if other != corner:
            assert untouched(arr, other), f"{other} drawn with show_fps={corner}"


# ── flushRows only: out of the rows, straddling them, inside them ───────────


@pytest.mark.parametrize("corner,y0,y1", [
    ("tr", 150, 159),   # box rows 22..33 outside the band: window push
    ("bl", 150, 159),   # box rows 302..313 outside the band
    ("tr", 0, 27),      # the band straddles the box
    ("br", 0, 319),     # every call sends the whole screen
])
def test_counter_at_chosen_corner_flush_rows(sim_factory, test_sd_card, corner, y0, y1):
    sim = boot(sim_factory, test_sd_card, show_fps=corner)
    run_fixture(sim, f"fps_rows_{corner}_{y0}", "rows", y0, y1)
    # "--" first, then a number once a 1 s window completes: two pushes
    # when the box lies outside the band.
    arr = wait_screen(sim, lambda a: counter_at(a, corner))
    assert counter_at(arr, corner), f"no FPS counter at {corner} for flushRows({y0}, {y1})"
    for other in CORNERS:
        if other != corner:
            assert untouched(arr, other), f"{other} drawn with show_fps={corner}"


def test_no_window_push_while_hardware_scrolled(sim_factory, test_sd_card):
    """With a non-zero scroll offset the panel shows GRAM rows at scrolled
    positions, so the box is neither pushed nor drawn: no black pixel
    anywhere on screen."""
    sim = boot(sim_factory, test_sd_card, show_fps="tr")
    run_fixture(sim, "fps_scrolled", "rows", 150, 159,
                setup="d.setScrollArea(0, 320, 160)\nd.setScrollOffset(64)")
    time.sleep(1.5)
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    black = (arr.sum(axis=2) == 0)
    assert not black.any(), (
        f"counter pixels on screen while scrolled: {int(black.sum())} black px, "
        f"first at {tuple(np.argwhere(black)[0])}")


# ── Toasts over flushRows ───────────────────────────────────────────────────


def _toast_region(arr):
    return arr[TOAST_Y:TOAST_Y + TOAST_H, TOAST_X:TOAST_X + TOAST_W]


def test_toast_reaches_panel_and_clears_with_flush_rows(sim_factory, test_sd_card):
    """A toast outside the flushRows band is pushed when it appears and, when
    it expires, the app's own pixels are pushed back (none of it was left
    in the app's draw buffer)."""
    sim = boot(sim_factory, test_sd_card, virtual_time=True)
    run_fixture(sim, "fps_toast", "rows", 150, 159)
    assert _colours(_toast_region(_rgb(sim.screenshot_pil()))) == {BG}
    r = sim.keypress("enter")
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    arr = wait_screen(sim, lambda a: _colours(_toast_region(a)) != {BG})
    assert BG not in _colours(_toast_region(arr)), "toast not pushed to the panel"
    # 3 s of sim time later the toast is gone and the fixture's blue is back.
    arr = wait_screen(sim, lambda a: _colours(_toast_region(a)) == {BG}, timeout=15)
    assert _colours(_toast_region(arr)) == {BG}, "expired toast left pixels behind"


# ── The Settings item ───────────────────────────────────────────────────────


def _show_fps(sim):
    cfg = json.loads((Path(sim.sd_card_path) / "system" / "config.json").read_text())
    return cfg.get("show_fps")


def _wait_cfg(sim, want, timeout=5.0):
    deadline = time.time() + timeout
    while True:
        try:
            got = _show_fps(sim)
        except (OSError, ValueError):
            got = None
        if got == want or time.time() >= deadline:
            return got
        time.sleep(0.05)


def test_settings_item_cycles_and_applies_on_close(sim_factory, test_sd_card):
    sim = boot(sim_factory, test_sd_card)
    run_fixture(sim, "fps_menu")
    # Main page (in an app): Battery, Settings, ...  Settings page:
    # Brightness, Battery %, Show FPS, ...
    sim.keypress("menu")
    time.sleep(0.3)
    sim.keypress_sequence(["down", "enter", "down", "down"], delay_ms=200)
    for want in ["tr", "tl", "br", "bl", "0", "tr"]:
        sim.keypress("enter")
        assert _wait_cfg(sim, want) == want, f"Show FPS did not cycle to {want}"
    sim.keypress_sequence(["esc", "esc"], delay_ms=200)
    arr = wait_screen(sim, lambda a: counter_at(a, "tr", number=False))
    assert counter_at(arr, "tr", number=False), "counter not shown after the menu closed"


# ── Native apps ─────────────────────────────────────────────────────────────


@pytest.mark.sd(extra=[("apps/hello_c", "apps/hello_c")])
def test_counter_over_native_app(sim_factory, test_sd_card):
    """The native display->flush draws the counter too (hello_c clears to
    black every frame, so only the text shows)."""
    sim = boot(sim_factory, test_sd_card, show_fps="tl")
    sim.launch_app("hello_c")
    sim.wait_frames(3, timeout=10)

    def ink(a):
        return _colours(_box(a, "tl")) - {(0, 0, 0)}

    arr = wait_screen(sim, lambda a: ink(a) and ink(a) <= NUMBER_INKS)
    assert ink(arr) and ink(arr) <= NUMBER_INKS, f"no counter over hello_c: {ink(arr)}"
