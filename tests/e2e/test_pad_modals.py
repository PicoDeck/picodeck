"""OS modals on a controller: ui.confirm and ui.textInput take a pad
source's A (yes / accept) and B (no / cancel), as the launcher does
(kbd_get_pad_nav_pressed). The keyboard keeps working alongside."""
import time

import pytest

# The default card is already at the app cap (tests/e2e/README.md, "App
# cap"): restrict to the one fixture app these tests launch.
pytestmark = pytest.mark.sd(fixtures=["pad_modal_test"])

APP = "pad_modal_test"
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


def test_confirm_takes_a_and_b(simulator):
    sim = simulator
    sim.launch_app(APP)
    _answer(sim, "PM:CONFIRM 1", _pad(sim, "a"), "true")
    _answer(sim, "PM:CONFIRM 2", _pad(sim, "b"), "false")
    _answer(sim, "PM:TEXT 1", _key(sim, "esc"), "nil")
    _answer(sim, "PM:TEXT 2", _key(sim, "enter"), "abc")
    _done(sim)


def test_text_input_takes_a_and_b(simulator):
    sim = simulator
    sim.launch_app(APP)
    _answer(sim, "PM:CONFIRM 1", _key(sim, "enter"), "true")
    _answer(sim, "PM:CONFIRM 2", _key(sim, "esc"), "false")
    _answer(sim, "PM:TEXT 1", _pad(sim, "b"), "nil")
    _answer(sim, "PM:TEXT 2", _pad(sim, "a"), "abc")
    _done(sim)


def test_a_held_when_confirm_opens_does_not_answer(simulator):
    """A held from before the dialog: no answer while held, none on its
    release, and a fresh press answers."""
    sim = simulator
    sim.pad("a")  # held until the next pad call
    sim.launch_app(APP)
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
