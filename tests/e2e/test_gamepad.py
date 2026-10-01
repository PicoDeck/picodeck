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
import re
import shutil
import time
from pathlib import Path

import pytest

from helpers import stage_lua_app

PAD_PROBE = Path(__file__).parent / "fixtures" / "native_pad_probe"
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


# --- native games (issue #25) -------------------------------------------------
# fixtures/native_pad_probe is built from the games' own adapters: gbc's
# input.c (the Game Boy joypad) and c64's pad_input.h (the joystick source).
# gbc needs a ROM and c64 the chip core, neither of which a test can observe,
# so the probe logs what those adapters produce for the keys we inject.
# Fields: gbc = up down left right a b select start, legacy = the same from a
# copy of the API that looks like firmware without the gamepad, c64 = up down
# left right fire.

PAD_IDLE = "gbc=00000000 legacy=00000000 c64=00000"


def _pad_lines(sim, since):
    return [re.search(r"PAD (gbc=\w+ legacy=\w+ c64=\w+)", t).group(1)
            for t in _texts(sim, since) if "PAD gbc=" in t]


def _pad_start(sim):
    # Staged at run time: one more app under tests/e2e/apps would push the
    # launcher past its 64-app cap.
    shutil.copytree(PAD_PROBE, Path(sim.sd_card_path) / "apps" / "native_pad_probe",
                    dirs_exist_ok=True)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("native_pad_probe")
    sim.wait_for_log(r"PADREADY", timeout=10, since_seq=seq)
    # The probe logs its idle state once at start: wait for it here, so the
    # first press below never collects it.
    sim.wait_for_log(re.escape("PAD " + PAD_IDLE), timeout=10, since_seq=seq)
    return seq


def _pad_finish(sim):
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)


def _pad_press(sim, key, expect):
    """Inject `key` and check that the probe logs exactly its press
    (`expect`) and then its release (idle): the probe logs each change once,
    so a stray button lit on the way in or out fails too."""
    mark = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.keypress(key)
    sim.wait_for_log(re.escape(expect), timeout=10, since_seq=mark)
    sim.wait_for_log(re.escape(PAD_IDLE), timeout=10, since_seq=mark)
    lines = _pad_lines(sim, mark)
    assert lines == [expect, PAD_IDLE], (key, lines)


def test_native_games_follow_default_bound_keys(simulator):
    sim = simulator
    _pad_start(sim)
    # F4 = A: gbc A and the C64 fire button; the old path agrees.
    _pad_press(sim, "f4", "gbc=00001000 legacy=00001000 c64=00001")
    # An arrow is a D-pad direction for both games.
    _pad_press(sim, "up", "gbc=10000000 legacy=10000000 c64=10000")
    # New layout: Tab = Select, F1 = Start. The old firmware path keeps
    # F1 = Select, F2 = Start (F2 is now the gamepad's L: no Game Boy button).
    _pad_press(sim, "tab", "gbc=00000010 legacy=00000000 c64=00000")
    _pad_press(sim, "f1", "gbc=00000001 legacy=00000010 c64=00000")
    _pad_press(sim, "f2", "gbc=00000000 legacy=00000001 c64=00000")
    _pad_finish(sim)


def test_native_games_follow_rebinding(simulator):
    """A per-app override moves A to Z: the game follows the bound key, and
    F4 (no longer bound) does nothing on the gamepad path."""
    sim = simulator
    _write(sim, "data/com.test.native_pad_probe/gamepad.json", {"a": ["Z"]})
    _pad_start(sim)
    _pad_press(sim, "z", "gbc=00001000 legacy=00000000 c64=00001")
    _pad_press(sim, "f4", "gbc=00000000 legacy=00001000 c64=00000")
    _pad_finish(sim)


# ── Pad sources (issue #26) ─────────────────────────────────────────────────
# A physical pad (later Bluetooth or USB) reports its whole PAD_* state as a
# source of its own (src/drivers/pad_source.h), ORed with the keyboard's
# aliases. The `pad` dev command drives the test source the same way; its
# rules (taps, background polls, clears, the wake swallow) are covered by
# tests/unit/test_pad_source.c.

GAP = 0.15  # between steps: the fixture polls every ~16 ms


def _pad_edges(sim, steps, until, since):
    """Run `steps` GAP apart (a str is a pad state, a callable is called),
    wait for the log line `until`, and return the gamepad edges since
    `since`."""
    for step in steps:
        if callable(step):
            step()
        else:
            sim.pad(step)
        time.sleep(GAP)
    sim.wait_for_log(until, timeout=10, since_seq=since)
    return _edges(sim, since)


def _mark(sim, seq):
    return sim.get_log_buffer(tail=1).get("next_seq", seq)


def test_pad_source_drives_the_gamepad(simulator):
    """`pad` buttons reach picocalc.gamepad with one press and one release
    edge each, and no key: input.* reports nothing (btn=0)."""
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)
    assert sim.pad("a+right") == "Pad: right+a"
    assert _pad_edges(sim, ["right", "none"], r"^GP:RELEASE right$", mark) == [
        "GP:PRESS right,a held=right,a btn=0",
        "GP:RELEASE a",
        "GP:RELEASE right",
    ], _texts(sim, mark)
    _finish(sim, seq)


def test_pad_and_keyboard_or_together(simulator):
    """A button is held while the keyboard or the pad holds it: the second
    source neither presses it again nor releases it early."""
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)

    def key(action):
        return lambda: sim.call("inject_button",
                                {"button": "f4", "action": action})

    assert _pad_edges(sim, [
        key("press"),      # F4: A pressed
        "a",               # the pad joins: no edge
        key("release"),    # F4 up, the pad still holds A: no edge
        "a+up",            # Up: only its own edge
        "none",            # both released at once
    ], r"^GP:RELEASE ", mark) == [
        f"GP:PRESS a held=a btn={BTN_F4}",
        "GP:PRESS up held=up,a btn=0",
        "GP:RELEASE up,a",
    ], _texts(sim, mark)
    _finish(sim, seq)


def test_pad_disconnect_tap_and_timed_hold(simulator):
    """Disconnecting releases what the pad held (nothing sticks); a press
    and release back to back still reads as one press; a timed hold
    releases itself."""
    sim = simulator
    seq = _start(sim)
    mark = _mark(sim, seq)
    assert _pad_edges(sim, ["b+l", "off"], r"^GP:RELEASE b,l$", mark) == [
        "GP:PRESS b,l held=b,l btn=0", "GP:RELEASE b,l"], _texts(sim, mark)

    mark = _mark(sim, seq)
    sim.pad("y")
    sim.pad("none")  # no gap: possibly both before the app's next poll
    sim.wait_for_log(r"^GP:RELEASE y$", timeout=10, since_seq=mark)
    assert [e.split(" held=")[0] for e in _edges(sim, mark)] == [
        "GP:PRESS y", "GP:RELEASE y"], _texts(sim, mark)

    mark = _mark(sim, seq)
    assert sim.pad("x", hold_ms=100) == "Pad: x for 100 ms"
    sim.wait_for_log(r"^GP:RELEASE x$", timeout=10, since_seq=mark)
    assert [e.split(" held=")[0] for e in _edges(sim, mark)] == [
        "GP:PRESS x", "GP:RELEASE x"], _texts(sim, mark)
    _finish(sim, seq)


def test_pad_command_errors(simulator):
    sim = simulator
    for cmd, want in (("pad", "Usage: pad"),
                      ("pad a+zz", "Error: unknown pad button: zz"),
                      ("pad a+", "Error: unknown pad button: "),
                      ("pad a soon", "Usage: pad")):
        r = sim.call("dev_command", {"cmd": cmd})
        assert not r["ok"] and r["output"].startswith(want), (cmd, r)
    assert sim.pad("NONE") == "Pad: none"
    assert sim.pad("Start+X") == "Pad: x+start"
    assert sim.pad("off") == "Pad: off"


PAD_MENU = """
local pc = picocalc
local d, sys = pc.display, pc.sys
sys.addMenuItem("Pad item", function() sys.log("PM:ITEM") end)
d.clear(d.BLUE)
d.flush()
sys.log("PM:READY")
local deadline = sys.getTimeMs() + 20000
while sys.getTimeMs() < deadline do
    pc.input.update()
    if (pc.gamepad.getButtonsPressed() & pc.gamepad.PAD_A) ~= 0 then
        sys.log("PM:APP_A")
    end
    sys.sleep(16)
end
sys.log("PM:TIMEOUT")
"""


def _menu_open(sim, timeout=10.0):
    """Wait for the system menu over the fixture's blue screen (it darkens
    the frame: the corner is no longer pure blue)."""
    deadline = time.time() + timeout
    while True:
        px = sim.screenshot_pil().convert("RGB").getpixel((20, 40))
        if px[2] < 200:
            return
        if time.time() >= deadline:
            raise AssertionError(f"the system menu did not open: {px}")
        time.sleep(0.05)


def test_pad_home_opens_the_menu_and_a_pad_leaves_the_game(simulator):
    """Home on a pad opens the system menu (the Sym key's path), the pad
    drives the menu (A selects), and with only a pad the player can leave:
    Home, Up (wraps to Exit App), A."""
    sim = simulator
    stage_lua_app(Path(sim.sd_card_path), "pad_menu", PAD_MENU)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("pad_menu")
    sim.wait_for_log(r"^PM:READY$", timeout=10, since_seq=seq)

    sim.pad("home", hold_ms=100)
    _menu_open(sim)
    sim.pad("a", hold_ms=100)  # the first item is the app's own
    sim.wait_for_log(r"^PM:ITEM$", timeout=10, since_seq=seq)

    sim.pad("home", hold_ms=100)
    time.sleep(GAP)
    _menu_open(sim)
    sim.pad("up", hold_ms=100)
    time.sleep(GAP)
    sim.pad("a", hold_ms=100)
    out = sim.wait_for_exit(timeout=15)
    assert out["result"] == "exit_sentinel", out
    texts = _texts(sim, seq)
    # The A presses went to the menu, never to the app.
    assert "PM:APP_A" not in texts and "PM:TIMEOUT" not in texts, texts


def test_keyboard_aliases_do_not_navigate_the_menus(simulator):
    """Only pad sources read as menu navigation: F4 and F5 (the keyboard's
    gamepad A and B) stay plain keys in the system menu, not Enter and Esc
    (kbd_get_pad_nav_pressed reads the sources alone)."""
    sim = simulator
    stage_lua_app(Path(sim.sd_card_path), "pad_menu", PAD_MENU)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("pad_menu")
    sim.wait_for_log(r"^PM:READY$", timeout=10, since_seq=seq)

    sim.keypress("menu")
    _menu_open(sim)
    sim.keypress("f4")  # as Enter it would choose the app's item and close
    time.sleep(GAP)
    sim.keypress("f5")  # as Esc it would close the menu
    time.sleep(0.5)
    _menu_open(sim, timeout=0)  # still open
    assert "PM:ITEM" not in _texts(sim, seq), _texts(sim, seq)

    sim.keypress("enter")  # the first item is the app's own
    sim.wait_for_log(r"^PM:ITEM$", timeout=10, since_seq=seq)
    sim.exit_app()
    sim.wait_for_exit(timeout=15)
    assert _texts(sim, seq).count("PM:ITEM") == 1, _texts(sim, seq)


# The same path on either target: the device's serial capture drops log
# lines, so this fixture writes its edges to a file.
PAD_TARGET_ID = "com.test.pad_target"
PAD_TARGET = """
local pc = picocalc
local sys, gp, d = pc.sys, pc.gamepad, pc.display
local edges = {}
local function save()
    local f = pc.fs.open(pc.fs.appPath("pad_edges.txt"), "w")
    pc.fs.write(f, table.concat(edges, "\\n") .. "\\n")
    pc.fs.close(f)
end
sys.resetIdleTimer()
d.clear(d.BLUE)
d.flush()
save()
local deadline = sys.getTimeMs() + 60000
while sys.getTimeMs() < deadline do
    sys.resetIdleTimer()  -- never dim: a press that wakes the screen is swallowed
    pc.input.update()
    local p, r = gp.getButtonsPressed(), gp.getButtonsReleased()
    if p ~= 0 or r ~= 0 then
        edges[#edges + 1] = string.format("P%d R%d H%d", p, r, gp.getButtons())
        save()
    end
    sys.sleep(16)
end
"""


def _read_edges(target, timeout=10.0, want=None):
    path = f"/data/{PAD_TARGET_ID}/pad_edges.txt"
    deadline = time.monotonic() + timeout
    while True:
        try:
            lines = [ln for ln in target.read_file(path).decode().splitlines()
                     if ln]
            if want is None or len(lines) >= want:
                return lines
        except FileNotFoundError:
            pass
        if time.monotonic() >= deadline:
            raise AssertionError(f"{path}: wanted {want} edges")
        time.sleep(0.1)


def _target_menu_open(target, timeout=10.0):
    import io
    from PIL import Image
    deadline = time.monotonic() + timeout
    while True:
        img = Image.open(io.BytesIO(target.screenshot())).convert("RGB")
        if img.getpixel((20, 40))[2] < 200:  # darkened: no longer pure blue
            return
        if time.monotonic() >= deadline:
            raise AssertionError("the system menu did not open")
        time.sleep(0.1)


@pytest.mark.both
def test_pad_source_on_target(target):
    """The `pad` dev command's source reaches picocalc.gamepad (A + Right:
    one press, one release), and a pad alone leaves the game: Home opens
    the system menu, Up wraps to Exit App, A chooses it."""
    target.stage_lua_app("pad_target", PAD_TARGET, id=PAD_TARGET_ID)
    target.delete_file(f"/data/{PAD_TARGET_ID}/pad_edges.txt")
    assert target.launch_app("pad_target")["launched"]
    _read_edges(target)  # the app is up

    try:
        target.pad("a+right")
        time.sleep(0.3)
        target.pad("none")
        assert _read_edges(target, want=2) == ["P24 R0 H24", "P0 R24 H0"]

        target.pad("home", hold_ms=150)
        _target_menu_open(target)
        target.pad("up", hold_ms=150)
        time.sleep(0.3)
        target.pad("a", hold_ms=150)
        out = target.wait_for_exit(timeout=20)
        # The device cannot tell the exit sentinel from a return.
        assert out["result"] in ("exit_sentinel", "returned"), out
        assert _read_edges(target) == ["P24 R0 H24", "P0 R24 H0"]  # no A
    finally:
        # The hardware session outlives this test: never leave the test pad
        # connected (header icon, launcher navigation) for the next one.
        target.pad("off")
