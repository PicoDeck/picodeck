"""The OS header bar (src/os/ui.c): a 20px gradient bar with a 1px border,
title on the left, clock / WiFi icon / battery icon on the right.

Geometry is probed from screenshots. Each header row is one solid colour
(the gradient steps per row), so "ink" in a row is any pixel that differs
from that row's colour at x=1, left of the title.
"""

import json
import time
from pathlib import Path

import numpy as np

from helpers import stage_lua_app

HEADER_H = 20          # bar rows 0..19, border at row 20
STATUS_RIGHT = 312     # FB_WIDTH - 8: right edge of the status cluster
BATT_W, BATT_H = 17, 9
BATT_X = STATUS_RIGHT - BATT_W          # 295
BATT_Y = (HEADER_H - BATT_H) // 2       # 5
BOLT_X0, BOLT_X1 = 288, 294             # charging bolt slot, left of the battery
WIFI_X0, WIFI_X1 = 276, 287             # WiFi icon when not charging

# 3x5 digits drawn inside the battery outline in percentage mode.
DIGITS = {
    "0": ["###", "#.#", "#.#", "#.#", "###"],
    "1": [".#.", "##.", ".#.", ".#.", "###"],
    "2": ["###", "..#", "###", "#..", "###"],
    "3": ["###", "..#", ".##", "..#", "###"],
    "4": ["#.#", "#.#", "###", "..#", "..#"],
    "5": ["###", "#..", "###", "..#", "###"],
    "6": ["###", "#..", "###", "#.#", "###"],
    "7": ["###", "..#", "..#", ".#.", ".#."],
    "8": ["###", "#.#", "###", "#.#", "###"],
    "9": ["###", "#.#", "###", "..#", "###"],
}


def battery_mask(pct: int, digits: bool) -> np.ndarray:
    """The expected 17x9 battery icon: a 15x9 outline plus a 2x3 nub, and
    either an 11x5 fill bar or the percentage in 3x5 digits."""
    g = np.zeros((BATT_H, BATT_W), dtype=bool)
    g[0, :15] = g[8, :15] = True
    g[:, 0] = g[:, 14] = True
    g[3:6, 15:17] = True
    if digits:
        s = str(pct)
        x0 = 2 + (11 - (len(s) * 4 - 1)) // 2
        for i, ch in enumerate(s):
            for r, row in enumerate(DIGITS[ch]):
                for c, p in enumerate(row):
                    if p == "#":
                        g[2 + r, x0 + i * 4 + c] = True
    else:
        fill = (pct * 11 + 99) // 100      # any charge shows at least 1px
        g[2:7, 2:2 + fill] = True
    return g


def _rgb(img) -> np.ndarray:
    return np.array(img.convert("RGB")).astype(int)


def ink_mask(arr: np.ndarray, x0: int, x1: int) -> np.ndarray:
    """Header pixels in columns x0..x1-1 that differ from their row's
    background (the colour at x=1)."""
    bar = arr[:HEADER_H]
    bg = bar[:, 1:2, :]
    return np.any(bar[:, x0:x1, :] != bg, axis=2)


def ink_rows(mask: np.ndarray) -> list:
    return [y for y in range(mask.shape[0]) if mask[y].any()]


def wait_for_screen(sim, pred, timeout=5.0):
    """Screenshot until pred(rgb array) holds; returns the last array."""
    deadline = time.time() + timeout
    while True:
        arr = _rgb(sim.screenshot_pil())
        if pred(arr) or time.time() >= deadline:
            return arr
        time.sleep(0.05)


def launcher_header(sim, pred=lambda a: True, timeout=8.0):
    """The launcher screen once its header has drawn (title ink present)."""
    return wait_for_screen(
        sim, lambda a: ink_mask(a, 0, 100).any() and pred(a), timeout)


def battery_ink(arr) -> np.ndarray:
    return ink_mask(arr, BATT_X, BATT_X + BATT_W)[BATT_Y:BATT_Y + BATT_H]


HOLD_FRAME = """
local pc = picocalc
{setup}
local h = pc.ui.drawHeader("Probe")
pc.display.flush()
pc.sys.log("HEADER_H=" .. tostring(h))
while true do
    pc.input.update()
    pc.sys.sleep(20)
end
"""


def run_header_app(sim, name, setup="", requirements=()):
    """Stage and launch an app that draws the header once and holds the
    frame; returns the log line with drawHeader's return value."""
    stage_lua_app(Path(sim.sd_card_path), name, HOLD_FRAME.format(setup=setup),
                  requirements=requirements)
    launcher_header(sim)
    sim.launch_app(name)
    return sim.wait_for_log(r"HEADER_H=", timeout=10)


# ── Geometry ────────────────────────────────────────────────────────────────


def test_header_is_a_20px_gradient_bar_with_border_below(simulator):
    arr = launcher_header(simulator)
    border = arr[HEADER_H]
    assert (border == border[0]).all(), "row 20 should be a uniform border line"
    assert not (arr[HEADER_H - 1, 1] == border[0]).all(), \
        "row 19 should still be header background, not border"
    top, bottom = arr[0, 1].sum(), arr[HEADER_H - 1, 1].sum()
    assert top > bottom, f"gradient should darken downwards (top {top}, bottom {bottom})"


def test_title_and_status_are_vertically_centred(simulator):
    arr = launcher_header(simulator)
    for label, (x0, x1) in {"title": (0, 100), "status": (200, 320)}.items():
        rows = ink_rows(ink_mask(arr, x0, x1))
        assert rows, f"no {label} ink in the header"
        above, below = rows[0], HEADER_H - 1 - rows[-1]
        assert abs(above - below) <= 1, \
            f"{label} ink rows {rows[0]}..{rows[-1]}: {above}px above, {below}px below"


def test_draw_header_returns_its_height(simulator):
    line = run_header_app(simulator, "hdr_height")
    assert "HEADER_H=21" in line, line


# ── Colour ──────────────────────────────────────────────────────────────────


def test_header_has_no_saturated_colour_when_healthy(simulator):
    """Full battery, WiFi online: the header is greys and navy only."""
    arr = launcher_header(simulator).reshape(-1, 3)[: (HEADER_H + 1) * 320]
    r, g, b = arr[:, 0], arr[:, 1], arr[:, 2]
    green = (g > np.maximum(r, b) + 16)
    red = (r > b + 16)
    assert not green.any(), f"{green.sum()} green-dominant header pixels"
    assert not red.any(), f"{red.sum()} red-dominant header pixels"


# ── Battery ─────────────────────────────────────────────────────────────────


def test_battery_icon_shows_fill_bar_by_default(simulator):
    arr = launcher_header(simulator)
    np.testing.assert_array_equal(battery_ink(arr), battery_mask(100, digits=False))


def test_battery_percent_setting_draws_digits_in_the_icon(simulator):
    run_header_app(simulator, "hdr_pct",
                   setup='pc.sysconfig.set("battery_pct", "1")',
                   requirements=["sysconfig"])
    arr = wait_for_screen(simulator, lambda a: (battery_ink(a) == battery_mask(100, True)).all())
    np.testing.assert_array_equal(battery_ink(arr), battery_mask(100, digits=True))


def test_low_battery_turns_the_icon_red(simulator):
    simulator.call("set_battery", {"percent": 10})
    arr = launcher_header(simulator, lambda a: (battery_ink(a) == battery_mask(10, False)).all())
    np.testing.assert_array_equal(battery_ink(arr), battery_mask(10, digits=False))
    icon = arr[BATT_Y:BATT_Y + BATT_H, BATT_X:BATT_X + BATT_W][battery_mask(10, False)]
    assert (icon[:, 0] > icon[:, 2] + 40).all(), f"low battery icon not red: {icon[0]}"


def test_charging_shows_a_bolt_left_of_the_battery(simulator):
    arr = launcher_header(simulator)
    assert not ink_mask(arr, BOLT_X0, BOLT_X1).any(), "bolt drawn while not charging"
    simulator.call("set_battery", {"percent": 100, "charging": True})
    arr = launcher_header(simulator, lambda a: ink_mask(a, BOLT_X0, BOLT_X1).any())
    assert ink_mask(arr, BOLT_X0, BOLT_X1).any(), "no bolt while charging"


# ── WiFi ────────────────────────────────────────────────────────────────────


def _wifi_pixels(arr):
    mask = ink_mask(arr, WIFI_X0, WIFI_X1)
    return arr[:HEADER_H, WIFI_X0:WIFI_X1][mask]


def test_wifi_icon_hidden_without_wifi(simulator):
    """Without WiFi the clock slides right into the icon's slot, so look for
    icon-coloured pixels (the off-white icon; the clock is a dimmer grey)."""
    def icon_px(a):
        return (_wifi_pixels(a).sum(axis=1) > 600).sum()
    assert icon_px(launcher_header(simulator)), "online WiFi should show the icon"
    simulator.call("set_wifi_state", {"status": "not_available"})
    arr = launcher_header(simulator, lambda a: not icon_px(a))
    assert not icon_px(arr), "icon still drawn without WiFi"


def test_wifi_icon_dims_while_connecting(simulator):
    online = _wifi_pixels(launcher_header(simulator)).sum(axis=1).max()
    simulator.call("set_wifi_state", {"status": "connecting"})
    arr = launcher_header(
        simulator, lambda a: len(_wifi_pixels(a)) and _wifi_pixels(a).sum(axis=1).max() < online)
    px = _wifi_pixels(arr)
    assert len(px), "icon vanished while connecting"
    assert px.sum(axis=1).max() < online, "connecting icon is as bright as online"


def test_wifi_failure_adds_a_red_slash(simulator):
    launcher_header(simulator)
    simulator.call("set_wifi_state", {"status": "failed"})
    arr = launcher_header(
        simulator, lambda a: len(_wifi_pixels(a)) and (_wifi_pixels(a)[:, 0] > _wifi_pixels(a)[:, 2] + 40).any())
    px = _wifi_pixels(arr)
    assert (px[:, 0] > px[:, 2] + 40).any(), "failed WiFi shows no red slash"


# ── Gamepad ─────────────────────────────────────────────────────────────────

# Drawn left of the WiFi icon while a pad source is connected (ui.c k_pad).
PAD_GLYPH = [
    ".###########.",
    "#############",
    "##.######.###",
    "#...####.#.##",
    "##.######.###",
    "#############",
    "####.....####",
    ".##.......##.",
]
PAD_W, PAD_H = 13, 8
PAD_X = WIFI_X0 - 8 - PAD_W             # 255
PAD_Y = (HEADER_H - PAD_H) // 2         # 6
PAD_MASK = np.array([[c == "#" for c in row] for row in PAD_GLYPH])


def _pad_shown(arr) -> bool:
    ink = ink_mask(arr, PAD_X, PAD_X + PAD_W)[PAD_Y:PAD_Y + PAD_H]
    return bool((ink == PAD_MASK).all())


def test_connected_pad_shows_a_gamepad_icon(simulator):
    """A connected pad source (here the `pad` dev command's) puts a gamepad
    left of the WiFi icon; disconnecting it takes the icon away again."""
    assert not _pad_shown(launcher_header(simulator))
    simulator.pad("none")  # connected, nothing held
    arr = launcher_header(simulator, _pad_shown)
    assert _pad_shown(arr), "no gamepad icon while a pad is connected"
    simulator.pad("off")
    arr = launcher_header(simulator, lambda a: not _pad_shown(a))
    assert not _pad_shown(arr), "gamepad icon still drawn after the disconnect"


# ── Overlays ────────────────────────────────────────────────────────────────


FPS_OVER_HEADER = """
local pc = picocalc
while true do
    pc.input.update()
    pc.display.clear(pc.display.BLACK)
    pc.ui.drawHeader("Probe")
    pc.perf.drawFPS()
    pc.display.flush()
    pc.sys.log("FPS_DRAWN")
    pc.sys.sleep(20)
end
"""


def test_default_fps_counter_sits_below_the_header(simulator):
    """perf.drawFPS() with no position must not cover the header's icons:
    the header keeps its colours, and the counter lands top-right below it."""
    stage_lua_app(Path(simulator.sd_card_path), "hdr_fps", FPS_OVER_HEADER)
    launcher_header(simulator)
    simulator.launch_app("hdr_fps")
    simulator.wait_for_log(r"FPS_DRAWN", timeout=10)
    simulator.wait_frames(2)
    arr = _rgb(simulator.screenshot_pil())
    bar = arr[:HEADER_H + 1].reshape(-1, 3)
    r, g, b = bar[:, 0], bar[:, 1], bar[:, 2]
    assert not ((g > np.maximum(r, b) + 16) | (r > b + 16)).any(), \
        "the FPS counter's colour shows inside the header"
    below = arr[HEADER_H + 1:HEADER_H + 20, 240:320]
    assert (below.sum(axis=2) > 0).any(), "no FPS counter below the header"


# ── Setting from the system menu ────────────────────────────────────────────


def test_system_menu_toggles_battery_percent(simulator):
    arr = launcher_header(simulator)
    np.testing.assert_array_equal(battery_ink(arr), battery_mask(100, digits=False))
    # Main page: Battery, Settings, ...  Settings page: Brightness, Battery %, ...
    simulator.keypress("menu")
    time.sleep(0.3)
    simulator.keypress_sequence(["down", "enter", "down", "enter", "esc", "esc"],
                                delay_ms=200)
    arr = launcher_header(
        simulator, lambda a: (battery_ink(a) == battery_mask(100, True)).all())
    np.testing.assert_array_equal(battery_ink(arr), battery_mask(100, digits=True))
    cfg = json.loads((Path(simulator.sd_card_path) / "system" / "config.json").read_text())
    assert cfg.get("battery_pct") == "1", cfg
