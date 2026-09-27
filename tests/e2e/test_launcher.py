"""Test basic launcher functionality."""

import json
import time
from pathlib import Path

import numpy as np

from helpers import stage_lua_app


# Launcher list geometry (src/os/launcher.c).
LIST_Y = 42
ITEM_H = 32            # row pitch; the selection bar is ITEM_H - 2
BAR_H = ITEM_H - 2
ICON = 24              # app icon size
ICON_X = 8
ROW_PROBE_X = 6
C_SEL_BG = ((40 >> 3) << 11) | ((80 >> 2) << 5) | (160 >> 3)   # RGB565(40, 80, 160)


def _px(sim, xy):
    return sim.call("get_pixel", {"x": xy[0], "y": xy[1]})["rgb565"]


def _wait_px(sim, xy, pred, timeout=5.0):
    deadline = time.time() + timeout
    while True:
        px = _px(sim, xy)
        if pred(px) or time.time() >= deadline:
            return px
        time.sleep(0.02)


def wait_for_launcher(simulator, timeout=5):
    """Wait until the launcher has rendered (non-uniform framebuffer)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        img = simulator.screenshot_pil()
        arr = np.array(img)[:, :, :3]
        if len(np.unique(arr.reshape(-1, 3), axis=0)) > 2:
            return img
        time.sleep(0.2)
    return simulator.screenshot_pil()


def wait_for_app_running(simulator, timeout=5):
    """Wait until an app is running (not the launcher)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status = simulator.call("get_running_app")
            if status and status.get("name"):
                return status
        except Exception:
            pass
        time.sleep(0.2)
    return None


class TestLauncher:
    """Tests for PicoDeck launcher."""

    def test_launcher_starts(self, simulator):
        """Test that launcher initializes and shows available apps."""
        screenshot = wait_for_launcher(simulator)
        assert screenshot.size == (320, 320)

        arr = np.array(screenshot)[:, :, :3]
        unique_colors = len(np.unique(arr.reshape(-1, 3), axis=0))
        assert unique_colors > 1, \
            "Launcher should have drawn UI elements (screen is uniform color)"

    def test_list_apps(self, simulator):
        """Test that apps are discovered and listed."""
        wait_for_launcher(simulator)

        apps = simulator.list_apps()
        assert len(apps) > 0, "No apps found"

        app_names = [a.lower() for a in apps]
        assert any("hello" in name for name in app_names), \
            f"hello app not found in {apps}"

    def test_launch_app(self, simulator):
        """Test launching an app from the launcher."""
        wait_for_launcher(simulator)

        result = simulator.launch_app("hello")
        assert result, "Failed to launch app"

        status = wait_for_app_running(simulator)
        assert status is not None, "App did not start running"

        screenshot = simulator.screenshot_pil()
        assert screenshot.size == (320, 320)

    def test_navigate_launcher(self, simulator):
        """Down moves the selection highlight from row 0 to row 1.

        Probes the left margin of each list row (x=6, left of the icon),
        which is painted in the row background: C_SEL_BG for the selected
        row. (A whole-screen diff can't fail: the description text scrolls.)
        """
        wait_for_launcher(simulator, timeout=8)
        assert len(simulator.list_apps()) > 1, "manifest SD should hold many apps"

        row0 = (ROW_PROBE_X, LIST_Y + 1)
        row1 = (ROW_PROBE_X, LIST_Y + ITEM_H + 1)
        assert _wait_px(simulator, row0, lambda p: p == C_SEL_BG) == C_SEL_BG, \
            "row 0 should start selected"
        assert _px(simulator, row1) != C_SEL_BG

        simulator.keypress("down")
        assert _wait_px(simulator, row1, lambda p: p == C_SEL_BG) == C_SEL_BG, \
            "Down did not move the highlight to row 1"
        assert _px(simulator, row0) != C_SEL_BG, "row 0 is still highlighted"

    def test_launch_by_directory_name(self, simulator):
        """Regression test: launcher_launch_by_name matches directory names."""
        wait_for_launcher(simulator)

        # Launch by directory name (not app ID or display name)
        result = simulator.launch_app("hello")
        assert result, "Failed to launch app by directory name"

        status = wait_for_app_running(simulator)
        assert status is not None, "App should be running after launch by dir name"


def _selected_row(simulator):
    """Screenshot (RGB array) once row 0 shows the selection bar."""
    wait_for_launcher(simulator, timeout=8)
    _wait_px(simulator, (ROW_PROBE_X, LIST_Y + 1), lambda p: p == C_SEL_BG)
    return np.array(simulator.screenshot_pil().convert("RGB")).astype(int)


def test_selection_bar_is_30px_on_a_32px_pitch(simulator):
    arr = _selected_row(simulator)
    sel = arr[LIST_Y + 1, ROW_PROBE_X]
    col = [(arr[y, ROW_PROBE_X] == sel).all() for y in range(LIST_Y - 1, LIST_Y + ITEM_H + 1)]
    rows = [LIST_Y - 1 + i for i, on in enumerate(col) if on]
    assert rows == list(range(LIST_Y, LIST_Y + BAR_H)), \
        f"selection bar covers rows {rows[:1]}..{rows[-1:]}, want {LIST_Y}..{LIST_Y + BAR_H - 1}"


def test_app_icon_is_24px_and_centred_in_its_row(simulator):
    arr = _selected_row(simulator)
    sel = arr[LIST_Y + 1, ROW_PROBE_X]
    bar = arr[LIST_Y:LIST_Y + BAR_H, ICON_X - 2:ICON_X + ICON + 2]
    icon = ~np.all(bar == sel, axis=2)          # pixels that are not bar colour
    ys, xs = np.nonzero(icon)
    assert len(ys), "no icon drawn in the selected row"
    h, w = ys.max() - ys.min() + 1, xs.max() - xs.min() + 1
    assert (w, h) == (ICON, ICON), f"icon is {w}x{h}, want {ICON}x{ICON}"
    above, below = ys.min(), BAR_H - 1 - ys.max()
    assert above == below, f"icon has {above}px above and {below}px below it"


def _rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


C_ICON_BG = _rgb565(12, 16, 48)          # background behind every generated icon
C_CART_LABEL = _rgb565(239, 232, 212)
C_NETWORK = _rgb565(50, 200, 150)


def test_app_without_icon_gets_a_cartridge(sim_factory, test_sd_card):
    """No icon.png: the launcher draws a cartridge in the app's category
    colour on the dark icon background, with the first letter on its label."""
    app = stage_lua_app(test_sd_card, "aardvark", "return\n")
    manifest = json.loads((app / "app.json").read_text())
    manifest.update(name="Aardvark", category="network")   # sorts first
    (app / "app.json").write_text(json.dumps(manifest))
    sim = sim_factory(test_sd_card)
    wait_for_launcher(sim, timeout=8)
    x0, y0 = ICON_X, LIST_Y + (BAR_H - ICON) // 2
    assert _wait_px(sim, (x0, y0), lambda p: p == C_ICON_BG) == C_ICON_BG, \
        "icon corner is not the dark icon background"
    assert _px(sim, (x0 + 5, y0 + 5)) == C_CART_LABEL, "no cartridge label"
    assert _px(sim, (x0 + 4, y0 + 12)) == C_NETWORK, "cartridge body is not the category colour"
    # 'A' at 2x on the label: its top bar starts at label x 7 + 2
    letter = _px(sim, (x0 + 9, y0 + 6))
    assert letter not in (C_CART_LABEL, C_NETWORK, C_ICON_BG), "no letter on the label"
    assert _px(sim, (x0 + 7, y0 + 6)) == C_CART_LABEL, "letter drawn left of its glyph"
