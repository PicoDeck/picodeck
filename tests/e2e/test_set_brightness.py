"""display.setBrightness drives the backlight, for Lua and native apps alike, and
an app cannot leave the device dark: the user's level returns at its exit, shows
in the system menu, and idle dim still works around the app's level.

No panel shows a level here: the get_backlight RPC reports what the firmware's
idle_dim.c last wrote with kbd_set_backlight. Colour and glow need the device.
"""
import json
import shutil
import time
from pathlib import Path

import pytest

from helpers import stage_lua_app

pytestmark = pytest.mark.sd(fixtures=[], reserve=2)

DIM_LEVEL = 10  # IDLE_DIM_LEVEL, src/os/idle_dim.c

PROBE = Path(__file__).parent / "fixtures" / "native_fidelity"

APP = """
local level = {level}
picocalc.display.setBrightness(level)
picocalc.sys.log("BRIGHT READY")
while true do
  picocalc.input.update()
  if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then return end
  picocalc.sys.sleep(10)
end
"""


def _level(sim):
    return sim.call("get_backlight")["level"]


def _wait_level(sim, want, timeout=10):
    deadline = time.time() + timeout
    while _level(sim) != want:
        assert time.time() < deadline, f"backlight {_level(sim)}, wanted {want}"
        time.sleep(0.02)


def _start_lua(sim, level, name="bright_probe"):
    stage_lua_app(sim.sd_card_path, name, APP.format(level=level))
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(name)
    sim.wait_for_log("BRIGHT READY", timeout=10, since_seq=seq)


def _leave(sim):
    assert sim.exit_app().get("ok")
    sim.wait_for_exit(timeout=10)


def test_lua_sets_the_backlight_and_the_exit_restores_the_users_level(simulator):
    user = _level(simulator)
    assert user != 40, "the test needs a level the app changes"
    _start_lua(simulator, 40)
    assert _level(simulator) == 40
    _leave(simulator)
    assert _level(simulator) == user


def test_native_sets_the_backlight_and_the_exit_restores_the_users_level(simulator):
    user = _level(simulator)
    d = Path(simulator.sd_card_path) / "apps" / "brightness"
    d.mkdir(parents=True, exist_ok=True)
    shutil.copy(PROBE / "main.elf", d / "main.elf")
    (d / "app.json").write_text(json.dumps({"id": "com.test.brightness",
                                            "name": "brightness"}))
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("brightness")
    simulator.wait_for_log("BRIGHT READY", timeout=10, since_seq=seq)
    assert _level(simulator) == 40
    _leave(simulator)
    assert _level(simulator) == user


def test_the_menu_shows_at_the_users_level_and_gives_the_app_its_own_back(simulator):
    user = _level(simulator)
    _start_lua(simulator, 0)
    assert _level(simulator) == 0

    seq = simulator.keypress("menu")["input_seq"]
    simulator.wait_input_consumed(seq, timeout=10)
    _wait_level(simulator, user)

    seq = simulator.keypress("esc")["input_seq"]
    simulator.wait_input_consumed(seq, timeout=10)
    _wait_level(simulator, 0)
    _leave(simulator)
    assert _level(simulator) == user


def test_a_lua_error_screen_is_not_left_dark(simulator):
    user = _level(simulator)
    stage_lua_app(simulator.sd_card_path, "bright_error", """
picocalc.display.setBrightness(0)
error("boom")
""")
    simulator.launch_app("bright_error")
    simulator.wait_for_exit(timeout=10)
    assert _level(simulator) == user


def _boot_dimming(sim_factory, sd):
    sim = sim_factory(sd, virtual_time=True, extra_args=["--idle-dim"])
    return sim, _level(sim)


def _wait_dimmed(sim):
    deadline = time.time() + 30
    while not sim.call("get_backlight")["dimmed"]:
        assert time.time() < deadline, "the screen never dimmed"
        time.sleep(0.02)
    sim.set_time_multiplier(1)


def test_a_wake_restores_the_apps_level(sim_factory, test_sd_card):
    sim, user = _boot_dimming(sim_factory, test_sd_card)
    assert user not in (200, DIM_LEVEL)
    _start_lua(sim, 200)
    assert _level(sim) == 200

    _wait_dimmed(sim)
    assert _level(sim) == DIM_LEVEL

    seq = sim.keypress("up")["input_seq"]
    sim.wait_input_consumed(seq, timeout=10)
    _wait_level(sim, 200)
    _leave(sim)
    assert _level(sim) == user


def test_set_brightness_does_not_wake_a_dimmed_screen(sim_factory, test_sd_card):
    # A level above the dim level waits for the wake; one below it applies now.
    sim, _ = _boot_dimming(sim_factory, test_sd_card)
    stage_lua_app(test_sd_card, "bright_dim", """
picocalc.sys.log("BRIGHT READY")
local sent = false
while true do
  picocalc.input.update()
  if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then return end
  if picocalc.sys.getTimeMs() > 90000 and not sent then
    sent = true
    picocalc.display.setBrightness(200)
    picocalc.sys.log("BRIGHT SENT")
  end
  picocalc.sys.sleep(10)
end
""")
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("bright_dim")
    sim.wait_for_log("BRIGHT READY", timeout=10, since_seq=seq)
    _wait_dimmed(sim)
    sim.set_time_multiplier(50)
    sim.wait_for_log("BRIGHT SENT", timeout=20, since_seq=seq)
    sim.set_time_multiplier(1)
    assert sim.call("get_backlight")["dimmed"]
    assert _level(sim) == DIM_LEVEL
    tap = sim.keypress("up")["input_seq"]
    sim.wait_input_consumed(tap, timeout=10)
    _wait_level(sim, 200)
    _leave(sim)


def test_the_users_change_in_the_menu_ends_the_apps_level(simulator):
    # Settings -> Brightness steps the user's level: that is the level from now
    # on, and at exit.
    user = _level(simulator)
    _start_lua(simulator, 40)
    for key in ("menu", "down", "enter", "right"):  # Battery, Settings; Brightness
        seq = simulator.keypress(key)["input_seq"]
        simulator.wait_input_consumed(seq, timeout=10)
        time.sleep(0.05)
    _wait_level(simulator, user + 16)
    changed = _level(simulator)

    seq = simulator.keypress("menu")["input_seq"]  # closes it from any page
    simulator.wait_input_consumed(seq, timeout=10)
    time.sleep(0.2)
    assert _level(simulator) == changed, "the app's 40 came back over the user's change"
    _leave(simulator)
    assert _level(simulator) == changed
