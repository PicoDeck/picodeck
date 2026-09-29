"""picocalc.gamepad: PAD_* buttons aliased to keys (issue #23).

The fixture (apps/gamepad_test) logs its labels, then every gamepad edge
together with the input.* press mask of the same frame. Keys are injected the
way the MCP keypress tool does (inject_button / inject_char), which drives the
gamepad through the same decode as a physical key. The decode rules
themselves (taps inside one poll, quiet holds, background polls, injected
latches) are covered by tests/unit/test_kbd_pad.c.
"""

import time

BTN_UP = 1 << 0
BTN_F4 = 1 << 10
BTN_TAB = 1 << 17


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _start(sim, app="gamepad_test"):
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(app)
    sim.wait_for_log(r"^GP:READY$", timeout=10, since_seq=seq)
    return seq


def _press(sim, key, seq):
    """Inject `key`; returns the log seq to read its edges from."""
    mark = sim.get_log_buffer(tail=1).get("next_seq", seq)
    sim.keypress(key)
    return mark


def _edges(sim, since):
    return [t for t in _texts(sim, since)
            if t.startswith("GP:PRESS ") or t.startswith("GP:RELEASE ")]


def _finish(sim, seq):
    sim.keypress("esc")
    sim.wait_for_log(r"^GP:DONE$", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)


def test_default_labels_and_arguments(simulator):
    sim = simulator
    seq = _start(sim)
    texts = _texts(sim, seq)
    assert ("GP:LABEL a=F4 a_alt=nil x=Del up_alt=nil select=Tab"
            in texts), texts
    # Not one PAD_* button, a slot other than 0/1, a non-integer: errors.
    assert "GP:BADARGS false false false" in texts, texts
    _finish(sim, seq)


def test_bound_keys_light_their_buttons(simulator):
    """F4, an arrow and Tab light A, Up and Select, each with one press and
    one release edge, while input.* still reports the key itself."""
    sim = simulator
    seq = _start(sim)
    for key, pad, btn in (("f4", "a", BTN_F4), ("up", "up", BTN_UP),
                          ("tab", "select", BTN_TAB)):
        mark = _press(sim, key, seq)
        sim.wait_for_log(rf"^GP:RELEASE {pad}$", timeout=10, since_seq=mark)
        assert _edges(sim, mark) == [
            f"GP:PRESS {pad} held={pad} btn={btn}",
            f"GP:RELEASE {pad}",
        ], _texts(sim, mark)
    _finish(sim, seq)


def test_unbound_keys_do_not_light_the_gamepad(simulator):
    """Enter and letters are not bound by default."""
    sim = simulator
    seq = _start(sim)
    mark = _press(sim, "enter", seq)
    time.sleep(0.15)  # past the 80 ms injected hold: no chord with F5
    sim.keypress("w")
    time.sleep(0.15)
    sim.keypress("f5")  # a bound key after them, as a marker
    sim.wait_for_log(r"^GP:RELEASE b$", timeout=10, since_seq=mark)
    assert [e.split(" held=")[0] for e in _edges(sim, mark)] == [
        "GP:PRESS b", "GP:RELEASE b"], _texts(sim, mark)
    _finish(sim, seq)
