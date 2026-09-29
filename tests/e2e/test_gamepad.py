"""picocalc.gamepad: PAD_* buttons aliased to keys (issue #23).

The fixture (apps/gamepad_test) logs its labels, then every gamepad edge
together with the input.* press mask of the same frame. The bindings files
(/system/gamepad.json, /data/<id>/gamepad.json) are read at app launch. Keys are injected the
way the MCP keypress tool does (inject_button / inject_char), which drives the
gamepad through the same decode as a physical key. The decode rules
themselves (taps inside one poll, quiet holds, background polls, injected
latches) are covered by tests/unit/test_kbd_pad.c.
"""

import json
import time
from pathlib import Path

from helpers import stage_lua_app

FIXTURE = Path(__file__).parent / "apps" / "gamepad_test" / "main.lua"

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


def _write(sim, rel, content):
    path = Path(sim.sd_card_path) / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content if isinstance(content, str) else json.dumps(content))


def _keys(sim, keys, until, since, full=False):
    """Inject `keys` 150 ms apart (past the 80 ms injected hold), wait for
    the log line `until`, and return the gamepad edges logged since `since`
    (without the held and btn fields unless `full`)."""
    for k in keys:
        sim.keypress(k)
        time.sleep(0.15)
    sim.wait_for_log(until, timeout=10, since_seq=since)
    edges = _edges(sim, since)
    return edges if full else [e.split(" held=")[0] for e in edges]


def test_override_file_changes_this_app_only(simulator):
    """/data/<id>/gamepad.json overrides the buttons it lists for that app;
    its other buttons, and every other app, keep the global map."""
    sim = simulator
    _write(sim, "system/gamepad.json", {"up": ["Up", "W"]})
    _write(sim, "data/com.test.gamepad_test/gamepad.json", {"a": ["Z"]})
    stage_lua_app(Path(sim.sd_card_path), "gamepad_other", FIXTURE.read_text(),
                  id="com.test.gamepad_other")

    # The app with the override: Z is A (a letter, injected as a char: one
    # tap), F4 is nothing, W from the global map is Up's alternate. The
    # letters release cleanly: the later presses hold only their own button.
    seq = _start(sim)
    assert ("GP:LABEL a=Z a_alt=nil x=Del up_alt=W select=Tab"
            in _texts(sim, seq)), _texts(sim, seq)
    mark = sim.get_log_buffer(tail=1).get("next_seq", seq)
    assert _keys(sim, ["f4", "z", "w", "tab"], r"^GP:RELEASE select$",
                 mark, full=True) == [
        "GP:PRESS a held=a btn=0", "GP:RELEASE a",
        "GP:PRESS up held=up btn=0", "GP:RELEASE up",
        f"GP:PRESS select held=select btn={BTN_TAB}", "GP:RELEASE select",
    ], _texts(sim, mark)
    _finish(sim, seq)

    # Another app: the global map, no override.
    seq = _start(sim, "gamepad_other")
    assert ("GP:LABEL a=F4 a_alt=nil x=Del up_alt=W select=Tab"
            in _texts(sim, seq)), _texts(sim, seq)
    mark = sim.get_log_buffer(tail=1).get("next_seq", seq)
    assert _keys(sim, ["z", "f4"], r"^GP:RELEASE a$", mark) == [
        "GP:PRESS a", "GP:RELEASE a"], _texts(sim, mark)
    _finish(sim, seq)


def test_corrupt_bindings_files_mean_defaults(simulator):
    """A corrupt global file and a corrupt override never fail the launch:
    the app gets the defaults."""
    sim = simulator
    _write(sim, "system/gamepad.json", '{"a": ["Z"')
    _write(sim, "data/com.test.gamepad_test/gamepad.json", "not json")
    seq = _start(sim)
    assert ("GP:LABEL a=F4 a_alt=nil x=Del up_alt=nil select=Tab"
            in _texts(sim, seq)), _texts(sim, seq)
    mark = sim.get_log_buffer(tail=1).get("next_seq", seq)
    assert _keys(sim, ["f4"], r"^GP:RELEASE a$", mark) == [
        "GP:PRESS a", "GP:RELEASE a"], _texts(sim, mark)
    _finish(sim, seq)


CLEAR_BEFORE_POLL = """
local pc = picocalc
local gp, input, log = pc.gamepad, pc.input, pc.sys.log
log("GC:READY")
pc.sys.sleep(700)        -- the test injects F4 now; nothing polls it yet
input.clearState()       -- a clear before any poll read the injection
local pad, btn = false, false
for _ = 1, 30 do
    input.update()
    pad = pad or (gp.getButtonsPressed() & gp.PAD_A) ~= 0
    btn = btn or (input.getButtonsPressed() & input.BTN_F4) ~= 0
    pc.sys.sleep(16)
end
log("GC:PRESSED pad=" .. tostring(pad) .. " btn=" .. tostring(btn))
"""


def test_injection_pending_at_a_clear_keeps_its_press(simulator):
    """A click injected but not yet read by a poll when the keyboard state is
    cleared is published, with its press edge, by the next poll: on the
    gamepad as on BTN_* (the device's pending one-shot)."""
    sim = simulator
    stage_lua_app(Path(sim.sd_card_path), "gamepad_clear", CLEAR_BEFORE_POLL)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("gamepad_clear")
    sim.wait_for_log(r"^GC:READY$", timeout=10, since_seq=seq)
    sim.keypress("f4")
    line = sim.wait_for_log(r"^GC:PRESSED ", timeout=10, since_seq=seq)
    assert line == "GC:PRESSED pad=true btn=true", _texts(sim, seq)
