"""Test basic launcher functionality."""

import time

import numpy as np


# Launcher list geometry (src/os/launcher.c).
LIST_Y = 42
ITEM_H = 28
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
