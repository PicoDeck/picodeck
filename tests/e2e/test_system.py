"""Tests for system functions, app lifecycle, and RPC infrastructure.

Regression targets:
- Sleep breaking early on exit request (commit in lua_bridge_sys.c)
- App launch by directory name (commit e42f4d2)
- Log buffer circular buffer correctness
- Heap info reporting
"""

import json
import time
from pathlib import Path

import pytest

from helpers import (measure_heap_probe, require_heap_metrics_live, log_texts,
                     lua_case_names, run_lua_app)

SYS_CASES = lua_case_names("sys_test")


@pytest.fixture(scope="module")
def sys_run(lua_suite):
    return lua_suite("sys_test")


class TestSystemLua:
    """sys_test (picotest kit): one pytest id per Lua case."""

    @pytest.mark.parametrize("case", SYS_CASES)
    def test_sys_case(self, sys_run, case):
        sys_run.check_case(case)

    def test_sys_suite_complete(self, sys_run):
        sys_run.assert_all_passed(SYS_CASES)


class TestAppLifecycle:
    """Test app launch, exit, and running state via RPC."""

    def test_launch_and_exit(self, simulator):
        """An app runs to completion and the launcher gets control back."""
        run = run_lua_app(simulator, "sys_test", timeout=15)
        run.assert_clean_exit()
        status = simulator.call("get_running_app")
        assert not (status and status.get("name")), status

    def test_launch_nonexistent_app(self, simulator):
        """Launching an unknown app reports found:false and the sim lives."""
        simulator.launch_app("nonexistent_app_xyz")
        outcome = simulator.wait_for_exit(timeout=10)
        assert outcome["found"] is False, outcome
        assert outcome["result"] == "load_failed", outcome
        assert "nonexistent_app_xyz" in (outcome.get("error") or ""), outcome
        assert "uptime_ms" in simulator.call("ping")

    def test_rapid_app_launch_exit(self, simulator):
        """Back-to-back launches each run to completion."""
        for i in range(3):
            run = run_lua_app(simulator, "sys_test", timeout=15)
            assert run.outcome.get("result") == "returned", f"cycle {i}: {run.describe()}"
            assert run.done, f"cycle {i}: {run.describe()}"


class TestHeapInfo:
    """Test heap information reporting."""

    def test_heap_info_structure(self, simulator):
        """Test that get_heap_info returns valid structure."""
        result = simulator.call("get_heap_info")
        assert "lua_heap_free_kb" in result
        assert "lua_heap_used_kb" in result
        assert "psram_total_kb" in result
        assert result["psram_total_kb"] >= 0

    def test_heap_metrics_live(self, simulator):
        """get_heap_info tracks real allocations: lua_heap_free_kb drops by
        at least 1 MB while heap_probe holds ~2 MB of Lua strings, and comes
        back once the app has exited and its VM is closed."""
        before, during, after = measure_heap_probe(simulator)
        assert before - during >= 1024, (
            f"no drop while 2 MB is held: before={before} KB, held={during} KB")
        assert before - during <= 4096, (
            f"implausible drop for 2 MB: before={before} KB, held={during} KB")
        assert abs(before - after) <= 128, (
            f"heap not returned after exit: before={before} KB, after={after} KB")

    def test_heap_info_after_app(self, simulator):
        """An app cycle leaves no more than 100 KB behind."""
        require_heap_metrics_live(simulator)
        free_before = simulator.call("get_heap_info")["lua_heap_free_kb"]
        run_lua_app(simulator, "sys_test", timeout=15).assert_clean_exit()
        free_after = simulator.call("get_heap_info")["lua_heap_free_kb"]
        leak = free_before - free_after
        assert leak < 100, (
            f"Possible heap leak: {leak}KB lost "
            f"(before={free_before}KB, after={free_after}KB)")


class TestLogBuffer:
    """Test the log buffer RPC infrastructure."""

    def test_clear_and_populate(self, simulator):
        """Test clearing log buffer and adding entries."""
        simulator.clear_log()
        logs = simulator.get_log_buffer()
        assert logs.get("count", -1) == 0

        run_lua_app(simulator, "sys_test", timeout=15).assert_clean_exit()

        logs = simulator.get_log_buffer()
        assert logs.get("count", 0) > 0
        assert len(logs.get("lines", [])) > 0

    def test_log_content_matches(self, simulator):
        """Log buffer content matches what the app logged, in order."""
        simulator.clear_log()
        run_lua_app(simulator, "sys_test", timeout=15).assert_clean_exit()
        lines = log_texts(simulator.get_log_buffer()["lines"])

        case_lines = [l for l in lines if l.startswith("[T] CASE ")]
        assert len(case_lines) == len(SYS_CASES), lines
        assert any(l.startswith("LOG_MARKER_") for l in lines), lines
        done = [i for i, l in enumerate(lines) if l.startswith("[T] DONE ")]
        assert done and done[0] > lines.index(case_lines[-1]), lines


class TestWifiState:
    """Test WiFi state reporting and mock control."""

    def test_get_wifi_state(self, simulator):
        """Test that get_wifi_state returns valid structure."""
        result = simulator.call("get_wifi_state")
        assert "status" in result
        # Mirrors wifi_status_t in src/os/os.h. "online" (internet confirmed) is
        # a distinct, stronger state than "connected" (IP assigned, internet
        # unverified) — it was missing here, so this failed against a simulator
        # that had finished connecting.
        assert result["status"] in (
            "connected", "connecting", "disconnected", "online", "failed"
        )

    def test_set_wifi_error_injection(self, simulator):
        """Test WiFi error injection for network fault testing."""
        result = simulator.call("set_wifi_state", {"status": "disconnected"})
        assert result.get("ok")
        assert simulator.call("get_wifi_state")["status"] == "disconnected"

        result = simulator.call("set_wifi_state", {"status": "connected"})
        assert result.get("ok")
        assert simulator.call("get_wifi_state")["status"] != "disconnected"


class TestButtonInjection:
    """Test button/key injection for input testing."""

    def test_inject_button(self, simulator):
        """Test that inject_button doesn't crash."""
        result = simulator.call("inject_button", {
            "button": "up",
            "action": "press",
        })
        assert result.get("ok"), f"inject_button failed: {result}"

        result = simulator.call("inject_button", {
            "button": "up",
            "action": "release",
        })
        assert result.get("ok"), f"inject_button release failed: {result}"

    def test_inject_char(self, simulator):
        """Test character injection."""
        result = simulator.call("inject_char", {"char": "A"})
        assert result.get("ok"), f"inject_char failed: {result}"

    def test_get_button_state(self, simulator):
        """Test reading button state."""
        result = simulator.call("get_button_state")
        assert "buttons" in result
        assert "buttons_pressed" in result
        assert "buttons_released" in result


def _wait_pixel(sim, x, y, pred, timeout=5.0):
    """Poll get_pixel until pred(pixel) holds; return the last pixel."""
    deadline = time.time() + timeout
    while True:
        px = sim.call("get_pixel", {"x": x, "y": y})
        if pred(px) or time.time() >= deadline:
            return px
        time.sleep(0.02)


def _brightness(px):
    return px["r"] + px["g"] + px["b"]


# A point outside the menu panel (200 px wide, centred) on static_screen's
# white field.
PROBE = (6, 40)


class TestSystemMenu:
    """System menu overlay triggered by the system menu key, over static_screen
    (every frame identical, so any change is the menu's)."""

    def _open_static(self, sim):
        sim.launch_app("static_screen")
        sim.wait_for_log("^SS:READY$", timeout=10)
        white = _wait_pixel(sim, *PROBE, lambda p: p["rgb565"] == 0xFFFF)
        assert white["rgb565"] == 0xFFFF, white

    def test_menu_open_and_dismiss(self, simulator):
        """Menu changes the display; Esc restores the exact app frame."""
        self._open_static(simulator)
        simulator.wait_frames(2)
        simulator.call("display_diff", {"action": "capture"})

        simulator.keypress("menu")
        _wait_pixel(simulator, *PROBE, lambda p: p["rgb565"] != 0xFFFF)
        diff = simulator.call("display_diff", {"action": "compare"})
        assert diff["changed_pixels"] > 1000, diff

        simulator.keypress("esc")
        _wait_pixel(simulator, *PROBE, lambda p: p["rgb565"] == 0xFFFF)
        simulator.wait_frames(2)
        diff = simulator.call("display_diff", {"action": "compare"})
        assert diff["changed_pixels"] == 0, (
            f"frame after dismissing the menu differs from before: {diff}")
        status = simulator.call("get_running_app")
        assert status and status.get("name"), \
            f"the app should still be running after Esc: {status}"

    def test_menu_darkens_display(self, simulator):
        """The overlay darkens the app frame outside the menu panel."""
        self._open_static(simulator)
        before = simulator.call("get_pixel", {"x": PROBE[0], "y": PROBE[1]})

        simulator.keypress("menu")
        after = _wait_pixel(simulator, *PROBE,
                            lambda p: _brightness(p) < _brightness(before))
        assert _brightness(after) <= 0.75 * _brightness(before), (
            f"menu did not darken the frame: before={before} after={after}")

        simulator.keypress("esc")


class TestConfigPersistence:
    """Per-app config written by one run is read back from disk by the next."""

    def test_config_survives_app_restart(self, simulator):
        first = run_lua_app(simulator, "sys_test", timeout=15)
        first.assert_all_passed(SYS_CASES)
        cfg_file = (Path(simulator.sd_card_path) / "data" / "net.picodeck.sys_test"
                    / "config.json")
        saved = json.loads(cfg_file.read_text())
        assert saved.get("e2e_runs") == "1", saved

        second = run_lua_app(simulator, "sys_test", timeout=15)
        second.assert_all_passed(SYS_CASES)
        texts = log_texts(second.log)
        assert "SYS:RUNS prev=1" in texts, (
            "second run did not load the first run's saved value\n"
            + second.describe())
