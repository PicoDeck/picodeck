"""The key that answers an OS modal does not reach the app, and neither does the
key held through a hand-off between the launcher and an app.

The same bug as issue #60 (tests/e2e/test_menu_key_leak.py), at the other places
the OS takes the keyboard and gives it back: ui.confirm, ui.textInput, fs.browse,
the title-bar text input (text_input_show), an app's launch and an app's exit. A
native app reaches the same C functions; the simulator's native ui-> modals are
stubs that return false at once, so its only native case here is the launch. The key is still
down when the OS gives the keyboard back, so a game comparing levels reads it as
a press, and on the device a held arrow, letter or Enter repeats as a fresh
press. ui.textInput also left everything typed at it queued for pollEvent.

Runs on the simulator and on the device (@pytest.mark.both) except where the test
says otherwise. What the STM32 sends for a physical key is replayed by the unit
tests (tests/unit/test_kbd_hide.c).
"""

from __future__ import annotations

import json
import re
import shutil
import time
from pathlib import Path

import pytest

import hw_target
from test_menu_key_hold import hold_shim  # noqa: F401 (a fixture)
from test_menu_key_leak import (APP, APP_ID, FIXTURE, Probe, QUIET_POLLS, _about,
                                _hold, _open_menu, _release)
from test_menu_toggle import BLUE, Screen, _into_settings, _wait_colour

pytestmark = [pytest.mark.timeout(180),
              pytest.mark.sd(fixtures=[], reserve=3)]
both = pytest.mark.both

SEEN_FILE = f"/data/{APP_ID}/seen.json"
NATIVE_LEVEL = Path(__file__).parent / "fixtures" / "native_level_probe"
GRACE = 0.6  # past ui.confirm's 400 ms
WIFI_SETTINGS_ROW = 7  # on the Settings page, with Bluetooth's row

# test_menu_key_leak's counting fixture, plus: F1 opens the modal named by MODE,
# then the counting goes on in the same frame, as the rest of a game's frame
# would, F2 calls sys.exit.
MODAL_FIXTURE = """
local MODE = "%s"
local function open_modal()
    local pc = picocalc
    local r
    if MODE == "confirm" then r = pc.ui.confirm("Sure?")
    elseif MODE == "text" then r = pc.ui.textInput("Name?", "")
    elseif MODE == "browse" then r = pc.fs.browse()
    end
    return tostring(r)
end
""" + FIXTURE.replace(
    "    seen.polls = seen.polls + 1\n",
    "    seen.polls = seen.polls + 1\n"
    "    if input.getButtonsPressed() & input.BTN_F1 ~= 0 then\n"
    "        seen.result = open_modal(); count('modal'); save()\n"
    "    end\n"
    "    if input.getButtonsPressed() & input.BTN_F2 ~= 0 then sys.exit() end\n")


def _start(target, mode="none"):
    target.stage_lua_app(APP, MODAL_FIXTURE % mode, id=APP_ID)
    target.delete_file(SEEN_FILE)
    target.write_file(f"/data/{APP_ID}/gamepad.json",
                      b'{"up":["Up"],"a":["F4"],"b":["F5"]}')
    assert target.launch_app(APP)["launched"]
    _wait_colour(target, BLUE, "the fixture's frame never showed")
    probe = Probe(target)
    probe.after_polls(5)
    return probe


def _finish(target):
    target.exit_app()
    target.wait_for_exit(timeout=15)


def _answer_with_held_key(target, probe, key):
    """The modal is up: answer it with key, still down when it returns."""
    _hold(target, key)
    seen = probe.wait("modal")
    seen = probe.after_polls()
    return seen


def _assert_hidden_until_pressed_again(target, probe, key):
    seen = probe.seen()
    assert _about(seen, "k", key) == {}, f"the app read the answering {key}"
    _release(target, key)
    seen = probe.after_polls(QUIET_POLLS // 2)
    assert _about(seen, "k", key) == {}, f"the app read {key}'s release"
    # The same key pressed again is a press like any other.
    target.keypress(key)
    seen = probe.wait(f"k.released.{key}")
    about = _about(seen, "k", key)
    assert about[f"k.pressed.{key}"] == 1, about
    assert about[f"k.ev_down.{key}"] == 1, about


@pytest.fixture(scope="module", autouse=True)
def remove_fixture_app(request):
    """The simulator's SD card is per test; a device's is not."""
    yield
    if hw_target.target_spec(request.config)[0] != "hw":
        return
    try:
        target = hw_target.session_target(request.config)
        target.ensure_launcher()
        target.delete_file(f"/data/{APP_ID}")
    except Exception as e:  # report, but never fail the tests over it
        print(f"modal_key_leak cleanup: {e}")


@both
@pytest.mark.parametrize("key,result", [("enter", "true"), ("esc", "false")])
def test_confirm_answer_stays_hidden(target, key, result):
    probe = _start(target, "confirm")
    target.keypress("f1")
    time.sleep(GRACE)
    seen = _answer_with_held_key(target, probe, key)
    assert seen["result"] == result, seen
    _assert_hidden_until_pressed_again(target, probe, key)
    _finish(target)


@both
def test_confirm_typed_answer_leaves_no_events(target):
    """y answers through getChar: its events stay in the queue."""
    probe = _start(target, "confirm")
    target.keypress("f1")
    time.sleep(GRACE)
    target.keypress("y")
    seen = probe.wait("modal")
    assert seen["result"] == "true", seen
    seen = probe.after_polls()
    assert _about(seen, "k", "121") == {}, "the app read the answering y"
    _finish(target)


@both
def test_text_input_cancel_stays_hidden(target):
    probe = _start(target, "text")
    target.keypress("f1")
    time.sleep(0.3)
    seen = _answer_with_held_key(target, probe, "esc")
    assert seen["result"] == "nil", seen
    _assert_hidden_until_pressed_again(target, probe, "esc")
    _finish(target)


def test_text_input_leaves_nothing_typed_for_pollevent(simulator):
    """The simulator's keypress types Enter's char, which ui.textInput reads to
    accept; a device's injected Enter is a button, so this runs here only."""
    target = hw_target.SimTarget(simulator)
    probe = _start(target, "text")
    target.keypress("f1")
    time.sleep(0.3)
    for c in "abc":
        target.keypress(c)
        time.sleep(0.05)
    target.keypress("\n")
    seen = probe.wait("modal")
    assert seen["result"] == "abc", seen
    seen = probe.after_polls()
    typed = {k: v for k, v in seen.items()
             if k.startswith("k.") and any(
                 k.endswith("." + n) for n in ("97", "98", "99", "enter"))}
    assert typed == {}, f"the app read what was typed at the dialog: {typed}"
    _finish(target)


@both
@pytest.mark.parametrize("key,picked", [("enter", True), ("esc", False)])
def test_browse_answer_stays_hidden(target, key, picked):
    target.write_file(f"/data/{APP_ID}/pick.txt", b"x")
    probe = _start(target, "browse")
    target.keypress("f1")
    time.sleep(0.3)
    seen = _answer_with_held_key(target, probe, key)
    assert (seen["result"] != "nil") == picked, seen
    _assert_hidden_until_pressed_again(target, probe, key)
    _finish(target)


def _native_levels(sim, mark):
    return [e.get("text", "") for e in sim.get_log_lines(mark)
            if "LEVEL " in e.get("text", "")
            and re.search(r"=[1-9]", e.get("text", ""))]


def _next_seq(sim):
    return sim.get_log_buffer(tail=1).get("next_seq", 0)


@both
def test_key_held_through_a_launch_stays_hidden(target):
    """A key down when the app starts is not the app's: F3 here, as it does
    nothing at the launcher."""
    _hold(target, "f3")
    time.sleep(0.2)
    probe = _start(target)
    seen = probe.after_polls()
    assert _about(seen, "k", "f3") == {}, "the app read a key held at its launch"
    assert _about(seen, "p", "r") == {}, "the gamepad read a key held at the launch"
    _assert_hidden_until_pressed_again(target, probe, "f3")
    _finish(target)


@pytest.mark.native
def test_key_held_through_a_native_launch_stays_hidden(simulator):
    sim = simulator
    shutil.copytree(NATIVE_LEVEL,
                    Path(sim.sd_card_path) / "apps" / "native_level_probe",
                    dirs_exist_ok=True)
    target = hw_target.SimTarget(sim)
    _hold(target, "f3")
    time.sleep(0.2)
    seq = _next_seq(sim)
    sim.launch_app("native_level_probe")
    sim.wait_for_log(r"LEVELREADY", timeout=10, since_seq=seq)
    time.sleep(0.6)
    assert _native_levels(sim, seq) == [], "the native app read a key held at launch"
    _release(target, "f3")
    time.sleep(0.3)
    assert _native_levels(sim, seq) == [], "the native app read its release"
    sim.keypress("f3")
    sim.wait_for_log(r"LEVEL btn=\d+ press=\d*[1-9]\d* ", timeout=10, since_seq=seq)
    sim.exit_app()
    sim.wait_for_log(r"LEVELEXIT", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)


@pytest.fixture
def host_enter(hold_shim, monkeypatch):  # noqa: F811
    """The shim holds Enter for 2 s (list this before the simulator)."""
    monkeypatch.setenv("KEYHOLD_SYM", "13")  # SDLK_RETURN
    monkeypatch.setenv("KEYHOLD_MS", "2000")
    return hold_shim


def test_enter_held_through_an_apps_exit_starts_nothing(host_enter, simulator):
    """An app that quits on a key returns to a launcher that reads press edges,
    and a held Enter's repeats are press edges: without the hiding, the key that
    quit started whatever the launcher had selected (the card's first app)."""
    sim = simulator
    target = hw_target.SimTarget(sim)
    _start(target)
    host_enter.write_text("go")          # Enter goes down...
    time.sleep(0.2)
    target.keypress("f2")                # ...and the app exits with it down
    target.wait_for_exit(timeout=15)
    time.sleep(2.5)                      # past the repeats and the release
    assert not sim.call("get_running_app")["running"], \
        "the held Enter started an app from the launcher"
    # Enter pressed again does start it: the launcher was listening.
    target.keypress("enter")
    deadline = time.monotonic() + 10
    while not sim.call("get_running_app")["running"]:
        assert time.monotonic() < deadline, "Enter no longer starts an app"
        time.sleep(0.05)


def test_enter_held_through_the_wifi_dialog_does_not_answer_the_next(host_enter,
                                                                     simulator):
    """Settings -> Wi-Fi asks for the network, then the password, both with
    text_input_show. Enter answers the first; held, its repeats are press edges,
    and without the hiding they answered the second with an empty password and
    saved the lot."""
    sim = simulator
    target = hw_target.SimTarget(sim)
    screen = Screen(target)
    _start(target)
    _open_menu(target, screen)
    target.keypress("down")              # past the fixture's own menu item
    time.sleep(0.2)
    _into_settings(target)
    for _ in range(WIFI_SETTINGS_ROW):
        target.keypress("down")
        time.sleep(0.2)  # longer than an injected key's 80 ms hold
    target.keypress("enter")
    time.sleep(0.3)
    target.keypress("x")
    time.sleep(0.1)
    host_enter.write_text("go")          # answers the network's dialog
    time.sleep(2.6)                      # past the repeats and the release
    cfg = Path(sim.sd_card_path) / "system" / "config.json"
    saved = cfg.read_text() if cfg.exists() else ""
    assert "wifi_ssid" not in saved, f"the held Enter answered both: {saved}"
    # The second dialog is still up, and answers a key pressed at it.
    target.keypress("x")
    time.sleep(0.1)
    target.keypress("enter")
    deadline = time.monotonic() + 10
    while "wifi_ssid" not in (cfg.read_text() if cfg.exists() else ""):
        assert time.monotonic() < deadline, "the second dialog did not take Enter"
        time.sleep(0.05)
    assert json.loads(cfg.read_text())["wifi_pass"] == "x"
