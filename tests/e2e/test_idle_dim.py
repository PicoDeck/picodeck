"""Idle dim and app launches (issue #72): the key that wakes a dimmed screen is
swallowed, so an app must not start on a dimmed screen, or its first key is
spent waking it.

The simulator models the firmware's dim and swallow only with --idle-dim
(simulator/stubs/keyboard_stub.c is a port of keyboard.c's; launcher.c and
idle_dim.c are the firmware's own). No panel shows the dim: the get_backlight
RPC reports the level. On the virtual clock the 60 s timeout costs ~1.2 s, and
the tests then slow the clock to real time so the screen cannot dim again while
they run. tests/e2e/test_kbd_hw.py checks the launch on the device.
"""
import json
import time

import pytest

from helpers import app_rel_dir, stage_lua_app
from test_menu_key_hold import hold_shim  # noqa: F401 (a fixture)
from test_menu_key_leak import HostKey, SDLK_RETURN, SDLK_UP

DIM_LEVEL = 10  # IDLE_DIM_LEVEL, src/os/idle_dim.c

PROBE = """
local input = picocalc.input
picocalc.sys.log("PROBE READY")
while true do
  input.update()
  local pressed = input.getButtonsPressed()
  if pressed & input.BTN_UP ~= 0 then picocalc.sys.log("PROBE press up") end
  if pressed & input.BTN_DOWN ~= 0 then picocalc.sys.log("PROBE press down") end
  local ev = input.pollEvent()
  while ev do
    picocalc.sys.log("PROBE event " .. ev.type)
    ev = input.pollEvent()
  end
  if pressed & input.BTN_ESC ~= 0 then return end
  picocalc.sys.sleep(10)
end
"""


def _boot(sim_factory, sd):
    sim = sim_factory(sd, virtual_time=True, extra_args=["--idle-dim"])
    level = sim.call("get_backlight")["level"]
    assert level > DIM_LEVEL, "the test needs a level that dimming changes"
    return sim, level


def _wait_dimmed(sim):
    """Idle until the screen dims, then run the clock at real time."""
    deadline = time.time() + 30
    while not sim.call("get_backlight")["dimmed"]:
        assert time.time() < deadline, "the screen never dimmed"
        time.sleep(0.02)
    sim.set_time_multiplier(1)
    assert sim.call("get_backlight")["level"] == DIM_LEVEL


def _tap(sim, key):
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=10)


def _wait_lit(sim, level):
    """The poll that reads a key wakes the screen just after marking the key
    consumed."""
    deadline = time.time() + 10
    while sim.call("get_backlight") != {"level": level, "dimmed": False}:
        assert time.time() < deadline, "the key did not wake the screen"
        time.sleep(0.005)


def _probe_lines(sim):
    return [e["text"] for e in sim.get_log_lines(0) if "PROBE" in e["text"]]


def _launcher_passes(sim, n=2):
    """Let the launcher loop run: it serves dev commands between its polls."""
    for _ in range(n):
        assert sim.call("dev_command", {"cmd": "ping"})["ok"]


def _launch_probe_dimmed(sim_factory, sd):
    stage_lua_app(sd, "dim_probe", PROBE)
    sim, level = _boot(sim_factory, sd)
    _wait_dimmed(sim)
    sim.launch_app("dim_probe")
    sim.wait_for_log("PROBE READY", timeout=10)
    return sim, level


def test_launch_lights_the_panel(sim_factory, test_sd_card):
    sim, level = _launch_probe_dimmed(sim_factory, test_sd_card)
    state = sim.call("get_backlight")
    assert state == {"level": level, "dimmed": False}, \
        "the app started on a dimmed screen"


def test_first_key_after_a_launch_reaches_the_app(sim_factory, test_sd_card):
    sim, _ = _launch_probe_dimmed(sim_factory, test_sd_card)
    _tap(sim, "up")
    # A second key marks the place: whatever became of the first, this one
    # arrives, and the app read the first before it.
    _tap(sim, "down")
    sim.wait_for_log("PROBE press down", timeout=10)
    lines = _probe_lines(sim)
    assert "PROBE press up" in lines, \
        f"the first key after the launch never reached the app: {lines}"


@pytest.mark.sd(fixtures=[], reserve=1)  # stages a listed app
def test_launcher_swallows_the_wake_and_a_key_launch_still_works(sim_factory,
                                                                 test_sd_card):
    """The launcher's own path: the first Enter on a dimmed screen only wakes
    it, the next launches the selected app, and that app gets its first key."""
    # Sorts first, so it is the selected row.
    stage_lua_app(test_sd_card, "aaa_dim_probe", PROBE, hidden=False)
    sim, level = _boot(sim_factory, test_sd_card)
    _wait_dimmed(sim)

    _tap(sim, "enter")
    _wait_lit(sim, level)
    _launcher_passes(sim)
    assert not sim.call("get_running_app")["running"], \
        "the key that woke the screen launched an app"

    _tap(sim, "enter")
    sim.wait_for_log("PROBE READY", timeout=10)
    assert sim.call("get_running_app")["name"] == "aaa_dim_probe"
    _tap(sim, "up")
    sim.wait_for_log("PROBE press up", timeout=10)


@pytest.mark.sd(fixtures=[], reserve=1)  # stages a listed app
def test_pad_press_wakes_the_launcher_and_the_next_one_launches(sim_factory,
                                                                test_sd_card):
    stage_lua_app(test_sd_card, "aaa_dim_probe", PROBE, hidden=False)
    sim, level = _boot(sim_factory, test_sd_card)
    _wait_dimmed(sim)

    sim.pad("a")
    _wait_lit(sim, level)
    _launcher_passes(sim)
    assert not sim.call("get_running_app")["running"], \
        "the pad press that woke the screen launched an app"

    sim.pad("none")
    _launcher_passes(sim)
    sim.pad("a")
    sim.wait_for_log("PROBE READY", timeout=10)


def test_wake_key_in_a_running_app_is_still_swallowed(sim_factory, test_sd_card):
    """The other side of the launch: a screen that dims while an app runs
    still wakes on a key the app never sees."""
    stage_lua_app(test_sd_card, "dim_probe", PROBE)
    sim, level = _boot(sim_factory, test_sd_card)
    sim.launch_app("dim_probe")
    sim.wait_for_log("PROBE READY", timeout=10)
    _wait_dimmed(sim)

    _tap(sim, "up")
    _wait_lit(sim, level)
    _tap(sim, "down")
    sim.wait_for_log("PROBE press down", timeout=10)
    lines = _probe_lines(sim)
    assert "PROBE press up" not in lines, f"the waking key leaked: {lines}"
    # Only the second key's events: down, then up once its hold ends.
    assert lines.count("PROBE event down") == 1, lines


def test_refused_launch_shows_its_reason_on_a_lit_panel(sim_factory, test_sd_card):
    stage_lua_app(test_sd_card, "dim_big", "return\n")
    manifest = test_sd_card / app_rel_dir("dim_big") / "app.json"
    m = json.loads(manifest.read_text())
    m["min_psram_kb"] = 1024 * 1024
    manifest.write_text(json.dumps(m))
    sim, level = _boot(sim_factory, test_sd_card)
    _wait_dimmed(sim)

    sim.launch_app("dim_big")
    out = sim.wait_for_exit(timeout=10)
    assert out["result"] == "load_failed", out
    assert sim.call("get_backlight") == {"level": level, "dimmed": False}


def test_launch_of_a_missing_app_leaves_the_screen_dim(sim_factory, test_sd_card):
    """Nothing starts and nothing is drawn, so nothing needs the panel."""
    sim, _ = _boot(sim_factory, test_sd_card)
    _wait_dimmed(sim)

    sim.launch_app("no_such_app", hidden=False)
    out = sim.wait_for_exit(timeout=10)
    assert out["found"] is False, out
    assert sim.call("get_backlight") == {"level": DIM_LEVEL, "dimmed": True}


# The waking key held on: the host's auto-repeat stands in for the STM32's repeats,
# which for arrows, letters and Enter are fresh PRESSED items, so a key the wake only
# forgot read as a press at its first repeat (tests/unit/test_kbd_hide.c replays the
# device's). On a dimmed launcher, Enter held ~350 ms woke the screen and then started
# the selected app.
@pytest.mark.sd(fixtures=[], reserve=1)  # stages a listed app
def test_enter_held_on_a_dimmed_launcher_starts_nothing(hold_shim, monkeypatch,  # noqa: F811
                                                        sim_factory, test_sd_card):
    monkeypatch.setenv("KEYHOLD_SYM", str(SDLK_RETURN))
    monkeypatch.setenv("KEYHOLD_MS", "1500")
    stage_lua_app(test_sd_card, "aaa_dim_probe", PROBE, hidden=False)
    sim, level = _boot(sim_factory, test_sd_card)
    _wait_dimmed(sim)

    key = HostKey(hold_shim, "enter")
    key.press()
    _wait_lit(sim, level)
    key.wait_released()
    _launcher_passes(sim)
    assert not sim.call("get_running_app")["running"], \
        "the held Enter that woke the screen started an app"

    # Released, the next press is a press.
    _tap(sim, "enter")
    sim.wait_for_log("PROBE READY", timeout=10)


def test_key_held_that_wakes_a_running_app_stays_hidden(hold_shim, monkeypatch,  # noqa: F811
                                                        sim_factory, test_sd_card):
    monkeypatch.setenv("KEYHOLD_SYM", str(SDLK_UP))
    monkeypatch.setenv("KEYHOLD_MS", "1500")
    stage_lua_app(test_sd_card, "dim_probe", PROBE)
    sim, level = _boot(sim_factory, test_sd_card)
    sim.launch_app("dim_probe")
    sim.wait_for_log("PROBE READY", timeout=10)
    _wait_dimmed(sim)

    key = HostKey(hold_shim, "up")
    key.press()
    _wait_lit(sim, level)
    key.wait_released()
    _tap(sim, "down")
    sim.wait_for_log("PROBE press down", timeout=10)
    lines = _probe_lines(sim)
    assert "PROBE press up" not in lines, f"the held key's repeats read as presses: {lines}"

    # Released, the next press is a press.
    _tap(sim, "up")
    sim.wait_for_log("PROBE press up", timeout=10)
