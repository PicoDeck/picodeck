"""OS modals on a controller: ui.confirm and ui.textInput take a pad
source's A (yes / accept) and B (no / cancel), as the launcher does
(kbd_get_pad_nav_pressed). The keyboard keeps working alongside.

The fixtures (fixtures/pad_modal_test, fixtures/pad_modal_held_test) are
staged hidden per test (helpers.stage_lua_app), so they never count against
the launcher's app cap.
"""
import time
from pathlib import Path

from helpers import stage_lua_app

APP = "pad_modal_test"
APP_ID = "com.test.pad_modal_test"
FIXTURE_SRC = (Path(__file__).parent / "fixtures" / APP / "main.lua").read_text()

# A variant that waits for A to actually be held before it opens the first
# dialog: sim.pad("a") must not reach the launcher (which also launches apps
# on A), and the test needs proof A was held when the dialog opened.
HELD_APP = "pad_modal_held_test"
HELD_APP_ID = "com.test.pad_modal_held_test"
HELD_FIXTURE_SRC = (Path(__file__).parent / "fixtures" / HELD_APP /
                    "main.lua").read_text()

GRACE = 0.6  # past ui_confirm's 400 ms grace


def _mark(sim, seq=0):
    return sim.get_log_buffer(tail=1).get("next_seq", seq)


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _answer(sim, prompt, press, result):
    """Wait for `prompt`, then `press()` and expect `result`."""
    mark = _mark(sim)
    sim.wait_for_log(rf"^{prompt}$", timeout=10, since_seq=0)
    time.sleep(GRACE)
    press()
    sim.wait_for_log(rf"^PM:RESULT {result}$", timeout=10, since_seq=mark)


def _pad(sim, state):
    return lambda: sim.pad(state, 100)


def _key(sim, key):
    return lambda: sim.keypress(key)


def _done(sim):
    sim.wait_for_log(r"^PM:DONE$", timeout=10, since_seq=0)
    sim.wait_for_exit(timeout=10)


def _launch(sim):
    stage_lua_app(Path(sim.sd_card_path), APP, FIXTURE_SRC, id=APP_ID)
    sim.launch_app(APP)


def test_confirm_takes_a_and_b(simulator):
    sim = simulator
    _launch(sim)
    _answer(sim, "PM:CONFIRM 1", _pad(sim, "a"), "true")
    _answer(sim, "PM:CONFIRM 2", _pad(sim, "b"), "false")
    _answer(sim, "PM:TEXT 1", _key(sim, "esc"), "nil")
    _answer(sim, "PM:TEXT 2", _key(sim, "enter"), "abc")
    _done(sim)


def test_text_input_takes_a_and_b(simulator):
    sim = simulator
    _launch(sim)
    _answer(sim, "PM:CONFIRM 1", _key(sim, "enter"), "true")
    _answer(sim, "PM:CONFIRM 2", _key(sim, "esc"), "false")
    _answer(sim, "PM:TEXT 1", _pad(sim, "b"), "nil")
    _answer(sim, "PM:TEXT 2", _pad(sim, "a"), "abc")
    _done(sim)


def test_a_held_when_confirm_opens_does_not_answer(simulator):
    """A held from before the dialog: no answer while held, none on its
    release, and a fresh press answers.

    The fixture itself waits for A to be held (logging PM:WAIT, then
    PM:HELD once picocalc.gamepad.getButtons() shows PAD_A) before it opens
    the first dialog, so sim.pad("a") is only ever read by the app, never by
    the launcher (which also launches the selected app on A), and PM:HELD is
    proof A was actually held when the dialog opened.
    """
    sim = simulator
    stage_lua_app(Path(sim.sd_card_path), HELD_APP, HELD_FIXTURE_SRC,
                  id=HELD_APP_ID)
    sim.launch_app(HELD_APP)
    sim.wait_for_log(r"^PM:WAIT$", timeout=10, since_seq=0)
    sim.pad("a")  # held until the next pad call
    sim.wait_for_log(r"^PM:HELD$", timeout=10, since_seq=0)
    sim.wait_for_log(r"^PM:CONFIRM 1$", timeout=10, since_seq=0)
    time.sleep(GRACE)
    sim.pad("none")
    time.sleep(0.3)
    assert not [t for t in _texts(sim, 0) if t.startswith("PM:RESULT")], \
        _texts(sim, 0)
    mark = _mark(sim)
    sim.pad("a", 100)
    sim.wait_for_log(r"^PM:RESULT true$", timeout=10, since_seq=mark)
    _answer(sim, "PM:CONFIRM 2", _key(sim, "esc"), "false")
    _answer(sim, "PM:TEXT 1", _key(sim, "esc"), "nil")
    _answer(sim, "PM:TEXT 2", _key(sim, "esc"), "nil")
    _done(sim)
