"""The key that closes the system menu does not reach the app (issue #60).

A game that compares held levels from frame to frame (Doom's DG_GetKey, TIC-80's
input rebuild) reads a key still down when the menu closes as a fresh press: Esc
opened Doom's own menu and threw a TIC-80 cart back to the code editor. So what
is down when the menu opens or closes stays hidden from `input` and `gamepad`,
each key and button until it is released; pressed again, it reads normally.

The fixture counts everything a game can read, per key, frame by frame, and
writes the counts to a file (the serial capture on a device drops lines). The
keys here are injected: an injected Esc is held for 80 ms, longer than the menu
takes to close, which is the leak's simulator form. What the STM32 sends for a
physical key held on (HOLD and PRESSED repeats) is replayed by the unit tests
(tests/unit/test_kbd_hide.c), and the host key auto-repeat by the shim tests
below.

Runs on the simulator and on the device (@pytest.mark.both) except the native
fixture, the launcher's held Esc and the auto-repeat shim.
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
from test_menu_toggle import Screen, _wait_colour, BLUE

pytestmark = [pytest.mark.timeout(180),
              pytest.mark.sd(fixtures=[], reserve=2)]
both = pytest.mark.both

APP = "menu_key_leak"
APP_ID = "net.picodeck.menu_key_leak"
SEEN_FILE = f"/data/{APP_ID}/seen.json"
NATIVE_PROBE = Path(__file__).parent / "fixtures" / "native_level_probe"

# Polls of the fixture that outlast an injected key's 80 ms hold many times over
# (it sleeps 20 ms between polls): the window in which the leak shows.
QUIET_POLLS = 30

# The counts: k.<held|pressed|released|keydown|ev_down|ev_up|ev_char|char|raw>.<name>
# for keys, p.<held|pressed|released>.<name> for gamepad buttons; polls and item
# (the menu item's callback) alongside. Rewritten at most every 100 ms.
FIXTURE = r"""
local pc = picocalc
local d, input, gp, sys, fs = pc.display, pc.input, pc.gamepad, pc.sys, pc.fs

local BTNS = {
    esc = input.BTN_ESC, enter = input.BTN_ENTER, up = input.BTN_UP,
    down = input.BTN_DOWN, f3 = input.BTN_F3, f5 = input.BTN_F5,
}
local PADS = {
    a = gp.PAD_A, b = gp.PAD_B, up = gp.PAD_UP, down = gp.PAD_DOWN,
    r = gp.PAD_R,
}
local CODES = {
    [0xB1] = "esc", [0x0A] = "enter", [0xB5] = "up", [0xB6] = "down",
    [0x83] = "f3", [0x85] = "f5", [0x77] = "w",
}

local seen = { polls = 0 }
local function count(what) seen[what] = (seen[what] or 0) + 1 end
local function name_of(code) return CODES[code] or tostring(code) end

local path = fs.appPath("seen.json")
local function save()
    local f = fs.open(path, "w")
    fs.write(f, pc.json.encode(seen))
    fs.close(f)
end

sys.addMenuItem("Count me", function() count("item") end)

d.clear(d.BLUE)
d.flush()
save()

local last_save = sys.getTimeMs()
while true do
    sys.resetIdleTimer()
    input.update()
    seen.polls = seen.polls + 1

    local held, pressed, released =
        input.getButtons(), input.getButtonsPressed(), input.getButtonsReleased()
    for name, bit in pairs(BTNS) do
        if (held & bit) ~= 0 then count("k.held." .. name) end
        if (pressed & bit) ~= 0 then count("k.pressed." .. name) end
        if (released & bit) ~= 0 then count("k.released." .. name) end
    end
    for code, name in pairs(CODES) do
        if input.isKeyDown(code) then count("k.keydown." .. name) end
    end
    local pheld, ppressed, preleased =
        gp.getButtons(), gp.getButtonsPressed(), gp.getButtonsReleased()
    for name, bit in pairs(PADS) do
        if (pheld & bit) ~= 0 then count("p.held." .. name) end
        if (ppressed & bit) ~= 0 then count("p.pressed." .. name) end
        if (preleased & bit) ~= 0 then count("p.released." .. name) end
    end
    while true do
        local e = input.pollEvent()
        if not e then break end
        count("k.ev_" .. e.type .. "." .. name_of(e.key))
    end
    local c = input.getChar()
    if c then count("k.char." .. name_of(string.byte(c))) end
    local raw = input.getRawKey()
    if raw and raw ~= 0 then count("k.raw." .. name_of(raw)) end

    local now = sys.getTimeMs()
    if now - last_save >= 100 then
        save()
        last_save = now
    end
    sys.sleep(20)
end
"""


class Probe:
    """The fixture's counts, read from its file."""

    def __init__(self, target):
        self.target = target

    def seen(self, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            try:
                return json.loads(self.target.read_file(SEEN_FILE).decode())
            except (FileNotFoundError, ValueError):
                pass  # not written yet, or caught mid-rewrite
            if time.monotonic() >= deadline:
                pytest.fail(f"{SEEN_FILE} never read")
            time.sleep(0.05)

    def after_polls(self, n: int = QUIET_POLLS, timeout: float = 15.0) -> dict:
        """The counts once the app has polled n more times: it was running,
        reading the keys, all that time."""
        start = self.seen()["polls"]
        deadline = time.monotonic() + timeout
        while True:
            now = self.seen()
            if now["polls"] >= start + n:
                return now
            if time.monotonic() >= deadline:
                pytest.fail(f"the app polled only {now['polls'] - start} times")
            time.sleep(0.05)

    def wait(self, what: str, at_least: int = 1, timeout: float = 10.0) -> dict:
        """Wait for a count to reach at_least."""
        deadline = time.monotonic() + timeout
        while True:
            now = self.seen()
            if now.get(what, 0) >= at_least:
                return now
            if time.monotonic() >= deadline:
                pytest.fail(f"{what} stayed at {now.get(what, 0)}, wanted "
                            f"{at_least}: {_activity(now)}")
            time.sleep(0.05)


def _activity(seen: dict) -> dict:
    return {k: v for k, v in seen.items() if k != "polls"}


def _about(seen: dict, kind: str, name: str) -> dict:
    """The counts for one key (kind "k") or gamepad button ("p")."""
    return {k: v for k, v in seen.items()
            if k.startswith(kind + ".") and k.endswith("." + name)}


def _hold(target, key: str):
    """Down until _release: as a key held through the menu."""
    if target.kind == "sim":
        target.sim.call("inject_button", {"button": key, "action": "press"})
    else:
        target.command(f"keydown {key}")


def _release(target, key: str):
    if target.kind == "sim":
        target.sim.call("inject_button", {"button": key, "action": "release"})
    else:
        target.command(f"keyup {key}")


def _start(target):
    target.stage_lua_app(APP, FIXTURE, id=APP_ID)
    target.delete_file(SEEN_FILE)
    # The bindings these tests press, whatever a device's card has rebound.
    target.write_file(f"/data/{APP_ID}/gamepad.json",
                      b'{"up":["Up"],"a":["F4"],"b":["F5"]}')
    assert target.launch_app(APP)["launched"]
    _wait_colour(target, BLUE, "the fixture's frame never showed")
    probe = Probe(target)
    probe.after_polls(5)
    return Screen(target), probe


def _finish(target):
    target.exit_app()
    target.wait_for_exit(timeout=15)


def _open_menu(target, screen):
    target.keypress("menu")
    screen.wait(True, "the menu did not open")
    time.sleep(0.2)  # the menu's first polls have run


@pytest.fixture
def pad_cleanup(target):
    yield
    try:
        target.pad("off")  # never leave a test pad connected on a device
    except Exception:
        pass


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
        print(f"menu_key_leak cleanup: {e}")


@both
def test_esc_that_closes_the_menu_is_not_a_press(target):
    screen, probe = _start(target)
    _open_menu(target, screen)
    target.keypress("esc")
    screen.wait(False, "Esc did not close the menu")
    seen = probe.after_polls()
    assert _about(seen, "k", "esc") == {}, "the app read the closing Esc"
    # The same key pressed again is a press like any other.
    target.keypress("esc")
    seen = probe.wait("k.released.esc")
    esc = _about(seen, "k", "esc")
    assert esc["k.pressed.esc"] == 1, esc
    assert esc["k.held.esc"] >= 1, esc
    assert esc["k.keydown.esc"] >= 1, esc
    assert esc["k.ev_down.esc"] == 1, esc
    assert esc["k.ev_up.esc"] == 1, esc
    _finish(target)


@both
def test_held_esc_is_hidden_until_it_is_released(target):
    screen, probe = _start(target)
    _open_menu(target, screen)
    _hold(target, "esc")
    screen.wait(False, "Esc did not close the menu")
    seen = probe.after_polls()
    assert _about(seen, "k", "esc") == {}, "the app read the held Esc"
    _release(target, "esc")
    seen = probe.after_polls()
    assert _about(seen, "k", "esc") == {}, "the app read the Esc's release"
    target.keypress("esc")
    seen = probe.wait("k.released.esc")
    assert _about(seen, "k", "esc")["k.pressed.esc"] == 1
    _finish(target)


@both
def test_pad_b_that_closes_the_menu_is_not_a_press(target, pad_cleanup):
    """Held on, as a Bluetooth pad's B: hidden from the gamepad until it comes
    up, then an ordinary button, and held counts from the new press only."""
    screen, probe = _start(target)
    target.pad("none")  # connected, nothing down
    _open_menu(target, screen)
    target.pad("b")
    screen.wait(False, "the pad's B did not close the menu")
    seen = probe.after_polls()
    assert _about(seen, "p", "b") == {}, "the app read the closing B"
    target.pad("none")
    seen = probe.after_polls()
    assert _about(seen, "p", "b") == {}, "the app read the B's release"
    target.pad("b", hold_ms=100)
    seen = probe.wait("p.released.b")
    b = _about(seen, "p", "b")
    assert b["p.pressed.b"] == 1, b
    assert b["p.held.b"] >= 1, b
    assert b["p.released.b"] == 1, b
    _finish(target)


@both
def test_timed_pad_b_that_closes_the_menu_is_not_a_press(target, pad_cleanup):
    """A short press, released by itself just after the menu has closed."""
    screen, probe = _start(target)
    target.pad("none")
    _open_menu(target, screen)
    target.pad("b", hold_ms=100)
    screen.wait(False, "the pad's B did not close the menu")
    seen = probe.after_polls()
    assert _about(seen, "p", "b") == {}, "the app read the closing B"
    _finish(target)


@both
def test_pad_direction_held_through_the_menu_needs_a_new_press(target, pad_cleanup):
    """A direction held from the game on through the menu: hidden until
    released, as the issue asks ("until each is released")."""
    screen, probe = _start(target)
    target.pad("up")
    before = probe.wait("p.pressed.up")
    assert before["p.pressed.up"] == 1
    _open_menu(target, screen)
    target.keypress("esc")
    screen.wait(False, "Esc did not close the menu")
    # What the menu's opening took away reads as let go at once, and stays so.
    settled = probe.after_polls(5)["p.held.up"]
    seen = probe.after_polls()
    up = _about(seen, "p", "up")
    assert up["p.held.up"] == settled, up
    assert up["p.pressed.up"] == 1, up
    target.pad("none")
    probe.after_polls(5)
    target.pad("up", hold_ms=100)
    seen = probe.wait("p.pressed.up", at_least=2)
    assert seen["p.pressed.up"] == 2
    _finish(target)


@both
@pytest.mark.parametrize("key,pad", [("up", "up"), ("f5", "b")])
def test_key_held_through_the_menu_needs_a_new_press(target, key, pad):
    """A keyboard key held from the game on through the menu; both it and the
    gamepad button bound to it are hidden."""
    screen, probe = _start(target)
    _hold(target, key)
    probe.wait(f"k.pressed.{key}")
    _open_menu(target, screen)
    target.keypress("esc")
    screen.wait(False, "Esc did not close the menu")
    settled = probe.after_polls(5)
    seen = probe.after_polls()
    for held in (f"k.held.{key}", f"k.keydown.{key}", f"p.held.{pad}"):
        assert seen.get(held) == settled.get(held), \
            f"{held} moved after the menu closed: {_activity(seen)}"
    assert seen[f"k.pressed.{key}"] == 1
    assert seen[f"p.pressed.{pad}"] == 1
    assert seen[f"k.ev_down.{key}"] == 1
    _release(target, key)
    probe.after_polls(5)
    target.keypress(key)
    seen = probe.wait(f"p.pressed.{pad}", at_least=2)
    assert seen[f"k.pressed.{key}"] == 2
    _finish(target)


@both
def test_other_keys_still_read_while_one_is_hidden(target, pad_cleanup):
    screen, probe = _start(target)
    target.pad("none")
    _open_menu(target, screen)
    target.pad("b")
    screen.wait(False, "the pad's B did not close the menu")
    probe.after_polls(5)
    # B stays down and hidden; a second button on the pad and a key are new.
    target.pad("b+a")
    seen = probe.wait("p.pressed.a")
    assert _about(seen, "p", "b") == {}, seen
    assert seen["p.pressed.a"] == 1
    target.pad("b")
    probe.wait("p.released.a")
    target.keypress("f3")
    seen = probe.wait("k.released.f3")
    assert seen["k.pressed.f3"] == 1
    # The bound key's own button is a separate source of the same button: its
    # press is a fresh one although the pad's B is hidden.
    target.keypress("f5")
    seen = probe.wait("k.pressed.f5")
    assert _about(seen, "p", "b").get("p.pressed.b", 0) == 1
    _finish(target)


@both
def test_held_enter_on_a_menu_item_reaches_only_its_callback(target):
    """The item's callback runs after the menu has closed; the Enter that
    chose it stays hidden until it is released."""
    screen, probe = _start(target)
    _open_menu(target, screen)   # the app's item is the first row
    _hold(target, "enter")
    screen.wait(False, "Enter on the item did not close the menu")
    seen = probe.wait("item")
    seen = probe.after_polls()
    assert seen["item"] == 1
    assert _about(seen, "k", "enter") == {}, "the app read the chosen Enter"
    _release(target, "enter")
    seen = probe.after_polls()
    assert _about(seen, "k", "enter") == {}, "the app read the Enter's release"
    target.keypress("enter")
    seen = probe.wait("k.released.enter")
    assert seen["k.pressed.enter"] == 1
    _finish(target)


def test_launcher_menu_held_esc_does_not_close_it_again(simulator):
    """At the launcher: a hidden Esc neither closes the menu opened over it
    nor sticks once it has come up."""
    sim = simulator
    time.sleep(1.0)
    screen = Screen(sim)
    sim.keypress("menu")
    screen.wait(True, "the launcher's menu did not open")
    time.sleep(0.2)
    sim.call("inject_button", {"button": "esc", "action": "press"})
    screen.wait(False, "Esc did not close the menu")
    sim.keypress("menu")
    screen.wait(True, "the menu did not open again")
    screen_open = screen.changed()
    time.sleep(0.5)
    assert abs(screen.changed() - screen_open) < 300, \
        "the Esc still held closed the menu"
    sim.call("inject_button", {"button": "esc", "action": "release"})
    time.sleep(0.2)
    sim.keypress("esc")
    screen.wait(False, "a new Esc did not close the menu")


@pytest.mark.native
def test_native_app_never_reads_the_closing_key(simulator):
    sim = simulator
    shutil.copytree(NATIVE_PROBE,
                    Path(sim.sd_card_path) / "apps" / "native_level_probe",
                    dirs_exist_ok=True)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("native_level_probe")
    sim.wait_for_log(r"LEVELREADY", timeout=10, since_seq=seq)
    screen = Screen(sim)

    def lines_since(mark):
        """What the app read since mark: every LEVEL line with anything in
        it (one of all zeros is the state falling back to nothing)."""
        return [e.get("text", "") for e in sim.get_log_lines(mark)
                if "LEVEL " in e.get("text", "")
                and re.search(r"=[1-9]", e.get("text", ""))]

    def next_seq():
        return sim.get_log_buffer(tail=1).get("next_seq", 0)

    # Esc: the injected 80 ms hold outlasts the menu's close.
    sim.keypress("menu")
    screen.wait(True, "the menu did not open over the native app")
    time.sleep(0.2)
    mark = next_seq()
    sim.keypress("esc")
    screen.wait(False, "Esc did not close the menu")
    time.sleep(0.6)
    assert lines_since(mark) == [], "the native app read the closing Esc"
    sim.keypress("esc")
    sim.wait_for_log(r"LEVEL btn=\d+ press=\d*[1-9]\d* ", timeout=10, since_seq=mark)

    # The pad's B, held on through the close and then let go.
    sim.pad("none")
    sim.keypress("menu")
    screen.wait(True, "the menu did not open a second time")
    time.sleep(0.2)
    mark = next_seq()
    sim.pad("b")
    screen.wait(False, "the pad's B did not close the menu")
    time.sleep(0.6)
    assert lines_since(mark) == [], "the native app read the closing B"
    sim.pad("none")
    time.sleep(0.3)
    assert lines_since(mark) == [], "the native app read the B's release"
    sim.pad("b", hold_ms=100)
    sim.wait_for_log(r"LEVEL .*padp=\d*[1-9]\d* ", timeout=10, since_seq=mark)
    sim.pad("off")

    sim.exit_app()
    sim.wait_for_log(r"LEVELEXIT", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)


# SDL's key codes for the shim (SDL_keycode.h).
SDLK_ESCAPE = 27
SDLK_RETURN = 13
SDLK_UP = 0x40000052
SDLK_W = ord("w")

# Keys that repeat with no press edge (Esc), with a fresh one each time (Up,
# Enter), and one with no button at all, only events and chars (W).
HOST_KEYS = {"esc": SDLK_ESCAPE, "up": SDLK_UP, "enter": SDLK_RETURN, "w": SDLK_W}


class HostKey:
    """The shim's key: down at press(), up by itself KEYHOLD_MS later. Waits
    are on the shim's own acks, so they hold on a loaded machine."""

    def __init__(self, trigger, name):
        self.trigger = trigger
        self.name = name

    def _wait(self, what, timeout=30.0):
        ack = Path(f"{self.trigger}.{what}")
        deadline = time.monotonic() + timeout
        while not ack.exists():
            if time.monotonic() >= deadline:
                pytest.fail(f"the simulator never took the key's {what}")
            time.sleep(0.01)

    def press(self):
        self.trigger.write_text("go")
        self._wait("down")

    def wait_released(self):
        self._wait("up")


@pytest.fixture(params=sorted(HOST_KEYS))
def host_key(request, hold_shim, monkeypatch):  # noqa: F811
    """The shim holds this host key for 2 s (set before the simulator starts:
    list this fixture before it)."""
    monkeypatch.setenv("KEYHOLD_SYM", str(HOST_KEYS[request.param]))
    monkeypatch.setenv("KEYHOLD_MS", "2000")
    return HostKey(hold_shim, request.param)


@pytest.fixture
def host_key_last_repeat(host_key, monkeypatch):
    """The same, with a repeat and the release handed over in one poll."""
    monkeypatch.setenv("KEYHOLD_LAST_REPEAT", "1")
    return host_key


def _close_menu_on_host_key(target, screen, key):
    """The key goes down in the menu; Esc and Enter (on the first row, the
    app's item) close it, the others are closed by a tap of Esc over them."""
    _open_menu(target, screen)
    key.press()
    if key.name not in ("esc", "enter"):
        target.keypress("esc")
    screen.wait(False, "the menu did not close")


def _press_again_reads(target, probe, name):
    target.keypress(name)
    if name == "w":      # a typed char: down, char and up in one go
        seen = probe.wait("k.char.w")
        assert seen["k.ev_down.w"] == 1, seen
    else:
        seen = probe.wait(f"k.released.{name}")
        assert seen[f"k.pressed.{name}"] == 1, seen


def test_host_key_auto_repeat_stays_hidden(host_key, simulator):
    """A host key held down repeats, Esc with no press edge, an arrow with a
    fresh one each time (as the STM32's HOLD and PRESSED reports). The repeats
    of a key hidden when the menu closed read as nothing, for all 2 s it stays
    down; pressed again, it is a press."""
    target = hw_target.SimTarget(simulator)
    screen, probe = _start(target)
    _close_menu_on_host_key(target, screen, host_key)
    host_key.wait_released()
    seen = probe.after_polls()
    assert _about(seen, "k", host_key.name) == {}, \
        "the app read the held key's repeats"
    assert seen.get("p.pressed.up", 0) == 0, seen
    _press_again_reads(target, probe, host_key.name)
    _finish(target)


def test_repeat_and_release_in_one_poll(host_key_last_repeat, simulator):
    """The HAL keeps press edges and chars apart from its events: a repeat's
    edge and char, read in the poll whose events also bring the release, must
    go with the repeat (the release ended the hiding by then). Left to chance,
    a repeat every 40 ms against a poll every 20 ms, Up read as a press with no
    level behind it in about one run of test_host_key_auto_repeat_stays_hidden
    in fifteen; the shim hands both over together here, so it shows every time."""
    key = host_key_last_repeat
    target = hw_target.SimTarget(simulator)
    screen, probe = _start(target)
    _close_menu_on_host_key(target, screen, key)
    key.wait_released()
    seen = probe.after_polls()
    assert _about(seen, "k", key.name) == {}, \
        "the app read the last repeat of a hidden key"
    assert seen.get("p.pressed.up", 0) == 0, seen
    _press_again_reads(target, probe, key.name)
    _finish(target)


@pytest.fixture
def host_enter(hold_shim, monkeypatch):  # noqa: F811
    """The shim holds Enter for 2 s (list this before the simulator)."""
    monkeypatch.setenv("KEYHOLD_SYM", str(SDLK_RETURN))
    monkeypatch.setenv("KEYHOLD_MS", "2000")
    return HostKey(hold_shim, "enter")


def test_enter_held_on_exit_app_does_not_launch_the_next_app(host_enter, simulator):
    """The launcher reads a press edge, and a held Enter's repeats are press
    edges: without the hiding, Enter on Exit App, held a moment, started
    whatever the launcher had selected."""
    target = hw_target.SimTarget(simulator)
    screen, _ = _start(target)
    _open_menu(target, screen)
    target.keypress("up")               # wraps to Exit App, the last row
    time.sleep(0.2)
    host_enter.press()
    target.wait_for_exit(timeout=15)
    host_enter.wait_released()
    time.sleep(0.5)                     # a launch would have begun by now
    assert not simulator.call("get_running_app")["running"], \
        "the held Enter started an app from the launcher"
