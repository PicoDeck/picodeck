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
                     stage_lua_app,
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
    """System menu overlay triggered by the Sym/Menu key, over static_screen
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


class TestPowerStatus:
    """getPowerStatus reads what the header's charging bolt reads."""

    def test_reports_the_keyboard_controllers_charging_state(self, simulator):
        code = """
        local p = picocalc.sys.getPowerStatus()
        picocalc.sys.log("POWER " .. tostring(p.charging) .. " " .. p.percent)
        """
        stage_lua_app(simulator.sd_card_path, "power_status", code)
        for percent, charging in ((42, True), (77, False)):
            simulator.call("set_battery", {"percent": percent, "charging": charging})
            seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
            simulator.launch_app("power_status")
            simulator.wait_for_exit(timeout=10)
            simulator.wait_for_log(rf"^POWER {str(charging).lower()} {percent}$",
                                   timeout=5, since_seq=seq)


class TestAppIds:
    """The doc's rule: 1-79 characters, and the launcher refuses anything else.
    An 80-character id used to be cut to the 79-character one's /data/<id>."""

    def test_id_lengths(self, simulator):
        sd = Path(simulator.sd_card_path)
        code = 'picocalc.sys.log("ID_RAN " .. APP_ID)'
        stage_lua_app(sd, "id79", code, id="a" * 79)
        seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
        simulator.launch_app("id79")
        simulator.wait_for_exit(timeout=10)
        simulator.wait_for_log(rf"^ID_RAN {'a' * 79}$", timeout=5, since_seq=seq)

        stage_lua_app(sd, "id80", code, id="a" * 80)
        seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
        simulator.launch_app("id80")
        simulator.wait_for_exit(timeout=10)
        texts = log_texts(simulator.get_log_lines(seq))
        assert not any(t.startswith("ID_RAN") for t in texts), texts
        log = (sd / "system" / "error.log").read_text()
        assert "invalid app id" in log, log


class TestReboot:
    """The simulator has no reset to model: a reboot ends the app, as the native
    sys->reboot does, and the launcher takes over. It used to hang for good."""

    def test_sys_reboot_ends_the_app(self, simulator):
        code = """
        picocalc.sys.log("BEFORE")
        pcall(picocalc.sys.reboot)
        picocalc.sys.log("AFTER")
        """
        stage_lua_app(simulator.sd_card_path, "sys_reboot", code)
        seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
        simulator.launch_app("sys_reboot")
        out = simulator.wait_for_exit(timeout=10)
        assert out.get("result") == "exit_sentinel", out
        texts = log_texts(simulator.get_log_lines(seq))
        assert "BEFORE" in texts and "AFTER" not in texts, texts
        assert not simulator.call("get_running_app")["running"]

    def test_menu_reboot_from_an_app(self, simulator):
        code = """
        picocalc.display.clear(picocalc.display.BLUE)
        picocalc.display.flush()
        picocalc.sys.log("MENU_READY")
        while true do
            picocalc.input.update()
            picocalc.sys.sleep(20)
        end
        """
        stage_lua_app(simulator.sd_card_path, "menu_reboot", code)
        seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
        simulator.launch_app("menu_reboot")
        simulator.wait_for_log(r"^MENU_READY$", timeout=10, since_seq=seq)
        simulator.keypress("menu")
        time.sleep(0.3)
        # Up wraps from the first row to Exit App, and once more to Reboot.
        for _ in range(2):
            simulator.keypress("up")
            time.sleep(0.2)
        simulator.keypress("enter")
        out = simulator.wait_for_exit(timeout=10)
        assert out.get("result") == "exit_sentinel", out
        assert not simulator.call("get_running_app")["running"]

    def test_menu_reboot_at_the_launcher(self, simulator):
        simulator.keypress("menu")
        time.sleep(0.3)
        simulator.keypress("up")  # wraps from the first row to Reboot
        time.sleep(0.2)
        simulator.keypress("enter")
        time.sleep(0.3)
        # The launcher's loop is what starts an app: it is running again.
        simulator.launch_app("hello")
        deadline = time.monotonic() + 10
        while not simulator.call("get_running_app")["running"]:
            assert time.monotonic() < deadline, "the launcher did not come back"
            time.sleep(0.05)


class TestErrorPositions:
    """main.lua's errors name the file, as a module's do."""

    def test_runtime_error_names_main_lua(self, simulator):
        stage_lua_app(simulator.sd_card_path, "err_pos",
                      "local x = nil\n\nreturn x.field\n")
        simulator.launch_app("err_pos")
        out = simulator.wait_for_exit(timeout=10)
        assert out.get("result") == "error", out
        assert "/apps/.test/err_pos/main.lua:3: attempt to index" in out["error"], out

    def test_load_error_names_main_lua(self, simulator):
        sd = Path(simulator.sd_card_path)
        stage_lua_app(sd, "err_syntax", "local x =\n")
        simulator.launch_app("err_syntax")
        simulator.wait_for_exit(timeout=10)
        log = (sd / "system" / "error.log").read_text()
        assert "/apps/.test/err_syntax/main.lua:" in log, log

    def test_module_error_is_unchanged(self, simulator):
        stage_lua_app(simulator.sd_card_path, "err_mod", 'require("mod")\n',
                      files={"mod.lua": "local x = nil\nreturn x.field\n"})
        simulator.launch_app("err_mod")
        out = simulator.wait_for_exit(timeout=10)
        assert "/apps/.test/err_mod/mod.lua:2: " in out["error"], out
