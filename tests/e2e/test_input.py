"""Tests for input injection and app-level input handling.

Verifies that button presses and character injection via RPC are
received by running Lua apps through the picocalc.input API.
"""

import time
import pytest


class TestButtonInjection:
    """Test that injected buttons are received by apps."""

    def test_directional_buttons(self, simulator):
        """Test that directional button presses are logged by the app."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        for direction in ["up", "down", "left", "right"]:
            simulator.keypress(direction)
            time.sleep(0.1)

        # Give app time to process all inputs
        time.sleep(0.3)

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        for direction in ["up", "down", "left", "right"]:
            assert f"BTN:{direction}" in lines, (
                f"Button '{direction}' not received by app. Logs: {lines}"
            )

    def test_function_keys(self, simulator):
        """Test that F1-F5 function keys are received."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        for fkey in ["f1", "f2", "f3", "f4", "f5"]:
            simulator.keypress(fkey)
            time.sleep(0.1)

        time.sleep(0.3)

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        for fkey in ["f1", "f2", "f3", "f4", "f5"]:
            assert f"BTN:{fkey}" in lines, (
                f"Function key '{fkey}' not received by app. Logs: {lines}"
            )

    def test_enter_button(self, simulator):
        """Test that enter button is received."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        simulator.keypress("enter")
        time.sleep(0.3)

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]
        assert "BTN:enter" in lines, f"Enter not received. Logs: {lines}"


class TestCharacterInjection:
    """Test character injection via inject_char RPC."""

    def test_inject_letter(self, simulator):
        """Test injecting a letter character."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        simulator.keypress("A")
        time.sleep(0.3)

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]
        assert "CHAR:A" in lines, f"Character 'A' not received. Logs: {lines}"

    def test_inject_multiple_chars(self, simulator):
        """Test injecting a sequence of characters."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        for ch in ["H", "i", "!"]:
            simulator.keypress(ch)
            time.sleep(0.1)

        time.sleep(0.3)

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]
        for ch in ["H", "i", "!"]:
            assert f"CHAR:{ch}" in lines, (
                f"Character '{ch}' not received. Logs: {lines}"
            )


class TestKeyChars:
    """Enter's and Backspace's chars ('\\n', '\\b') reach getChar and
    ui.textInput when typed as chars (inject_char, JSON-escaped on the
    control channel). An Enter *button* tap stays a down and an up with no
    char, as on the device (test_hw_device.py's keyboard edges)."""

    def _lines(self, simulator):
        logs = simulator.get_log_buffer()
        return [(l if isinstance(l, str) else l.get("text", ""))
                for l in logs.get("lines", [])]

    def _open_text_box(self, simulator):
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)
        simulator.keypress("#")
        simulator.wait_for_log("TEXT_PROMPT", timeout=5)
        # ui.textInput drops input queued before it opens; let it open first.
        time.sleep(0.3)

    def test_typed_newline_and_backspace_reach_getchar(self, simulator):
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        simulator.keypress("\n")
        time.sleep(0.2)
        simulator.keypress("\b")
        time.sleep(0.3)

        lines = self._lines(simulator)
        assert "CHARCODE:10" in lines, f"'\\n' not received. Logs: {lines}"
        assert "CHARCODE:8" in lines, f"'\\b' not received. Logs: {lines}"

    def test_enter_button_tap_has_no_char(self, simulator):
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        simulator.keypress("enter")
        time.sleep(0.3)

        lines = self._lines(simulator)
        assert "BTN:enter" in lines, f"Enter not received. Logs: {lines}"
        assert not any(l.startswith("CHARCODE:") for l in lines), (
            f"a button tap gave a char. Logs: {lines}")

    def test_enter_accepts_text_input(self, simulator):
        self._open_text_box(simulator)
        for key in ["h", "i", "\n"]:
            simulator.keypress(key)
            time.sleep(0.15)
        simulator.wait_for_log("TEXT:hi", timeout=5)

    def test_backspace_edits_text_input(self, simulator):
        self._open_text_box(simulator)
        for key in ["a", "b", "\b", "c", "\n"]:
            simulator.keypress(key)
            time.sleep(0.15)
        simulator.wait_for_log("TEXT:ac", timeout=5)


class TestEscExit:
    """Test that ESC key causes app exit."""

    def test_esc_exits_app(self, simulator):
        """Test that pressing ESC causes the input_test app to exit."""
        simulator.clear_log()
        simulator.launch_app("input_test")
        simulator.wait_for_log("INPUT_READY", timeout=10)

        simulator.keypress("esc")

        try:
            simulator.wait_for_log("INPUT_EXIT", timeout=5)
        except TimeoutError:
            pytest.fail("App did not exit on ESC key")

        # Simulator should still be responsive
        result = simulator.call("ping")
        assert "uptime_ms" in result
