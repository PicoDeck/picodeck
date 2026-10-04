"""picocalc.gamepad.getNavPressed / getButtonsRepeated: menus and lists on
the gamepad (D-pad auto-repeat, Enter and Esc folded into A and B).

The fixture (apps/gamepad_nav_test) logs every non-zero mask with its time
and reads each twice per frame. The repeat clock is covered by
tests/unit/test_btn_repeat.c and the binding check by tests/unit/test_kbd_pad.c.
"""
import json
import re
import time
from pathlib import Path

PAD_DOWN, PAD_A, PAD_B, PAD_X, PAD_START = 1 << 1, 1 << 4, 1 << 5, 1 << 6, 1 << 10
APP = "gamepad_nav_test"
APP_ID = "com.test.gamepad_nav_test"


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _mark(sim, seq=0):
    return sim.get_log_buffer(tail=1).get("next_seq", seq)


def _start(sim):
    seq = _mark(sim)
    sim.launch_app(APP)
    sim.wait_for_log(r"^NAV:READY ", timeout=10, since_seq=seq)
    return seq


def _lines(sim, since, kind):
    """(mask, t) for each NAV:<kind> line logged since `since`."""
    out = []
    for t in _texts(sim, since):
        m = re.match(rf"^NAV:{kind} (\d+) t=(\d+)$", t)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def _stable(sim, seq):
    unstable = [t for t in _texts(sim, seq) if t.startswith("NAV:UNSTABLE")]
    assert not unstable, unstable


def _finish(sim, seq):
    sim.keypress("esc")
    sim.wait_for_log(r"^NAV:DONE$", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)
    _stable(sim, seq)


def _hold(sim, state, seconds):
    sim.pad(state)
    time.sleep(seconds)
    sim.pad("none")
    time.sleep(0.15)


def test_helpers_exist(simulator):
    seq = _start(simulator)
    assert "NAV:READY true true" in _texts(simulator, seq)
    _finish(simulator, seq)


def test_held_dpad_repeats_in_nav(simulator):
    """Down held 0.7 s: one press, then repeats after the 200 ms delay."""
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)
    _hold(sim, "down", 0.7)
    nav = [l for l in _lines(sim, mark, "NAV") if l[0] == PAD_DOWN]
    assert len(nav) >= 4, _texts(sim, mark)   # press, then 200, 280, 360 ms ...
    assert nav[1][1] - nav[0][1] >= 150, nav  # the first repeat waits the delay
    _finish(sim, seq)


def test_a_presses_once_in_nav_but_repeats_in_getButtonsRepeated(simulator):
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)
    _hold(sim, "a", 0.7)
    assert [m for m, _ in _lines(sim, mark, "NAV")] == [PAD_A], _texts(sim, mark)
    assert len([m for m, _ in _lines(sim, mark, "REP") if m == PAD_A]) >= 4
    _finish(sim, seq)


def test_enter_and_esc_fold_into_a_and_b(simulator):
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)
    sim.keypress("enter")
    sim.wait_for_log(rf"^NAV:NAV {PAD_A} ", timeout=10, since_seq=mark)
    mark = _mark(sim, mark)
    sim.keypress("esc")
    sim.wait_for_log(r"^NAV:DONE$", timeout=10, since_seq=seq)
    assert [m for m, _ in _lines(sim, mark, "NAV")] == [PAD_B], _texts(sim, mark)
    sim.wait_for_exit(timeout=10)
    _stable(sim, seq)


def test_bound_enter_is_its_button_only(simulator):
    """With Enter bound to Start (this app's override), Enter gives Start
    and no A: a player who bound it gets that button's meaning alone."""
    sim = simulator
    override = Path(sim.sd_card_path) / "data" / APP_ID / "gamepad.json"
    override.parent.mkdir(parents=True, exist_ok=True)
    override.write_text(json.dumps({"start": ["Enter"]}))
    try:
        seq = _start(sim)
        mark = _mark(sim, seq)
        sim.keypress("enter")
        sim.wait_for_log(rf"^NAV:NAV {PAD_START} ", timeout=10, since_seq=mark)
        time.sleep(0.15)
        assert [m for m, _ in _lines(sim, mark, "NAV")] == [PAD_START], \
            _texts(sim, mark)
        _finish(sim, seq)
    finally:
        override.unlink(missing_ok=True)


def test_setrepeat_zero_turns_gamepad_repeat_off(simulator):
    sim = simulator
    seq = _start(sim)
    sim.pad("x", 100)
    sim.wait_for_log(r"^NAV:NOREPEAT$", timeout=10, since_seq=seq)
    time.sleep(0.2)
    mark = _mark(sim, seq)
    _hold(sim, "down", 0.6)
    assert [m for m, _ in _lines(sim, mark, "NAV")] == [PAD_DOWN], _texts(sim, mark)
    _finish(sim, seq)
