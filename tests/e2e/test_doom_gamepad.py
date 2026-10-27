"""DOOM reads the gamepad (issue #25).

Doom lives in its own repo (github.com/PicoDeck/doom): the tests stage its
built main.elf and doom1.wad (PICODECK_DOOM_DIR, default ~/Projects/PicoDeck/
doom; `make` there) and skip without them (skip_allowlist.txt). They start
E1M1 and read the effect off the screen: ammo for fire and weapon changes, a
share of changed view pixels for turning, strafing and Doom's menu.

Injected character keys tap for one poll, too short for Doom's 35 Hz tic, so
the rebinding test swaps button keys (F3, F4) rather than letters. Only the
`pad` and `keydown` dev commands are not served while a native app runs in the
simulator; button keys reach it as clicks, or as inject_button press/release
for a held key.
"""

import json
import os
import shutil
import time
from pathlib import Path

import pytest
from PIL import Image, ImageChops
import io

DOOM = Path(os.environ.get("PICODECK_DOOM_DIR",
                           Path.home() / "Projects" / "PicoDeck" / "doom"))
FILES = ("main.elf", "app.json", "doom1.wad")

pytestmark = [
    pytest.mark.skipif(
        not all((DOOM / f).exists() for f in FILES),
        reason=f"{DOOM} lacks main.elf/doom1.wad: build PicoDeck/doom or set "
               "PICODECK_DOOM_DIR"),
    pytest.mark.sd(fixtures=[], reserve=1),
    pytest.mark.timeout(180),
]


def _stage(sim, rebind=None):
    d = Path(sim.sd_card_path) / "apps" / "doom"
    d.mkdir(parents=True, exist_ok=True)
    for f in FILES:
        shutil.copy(DOOM / f, d / f)
    if rebind:
        p = Path(sim.sd_card_path) / "data" / "com.id.doom" / "gamepad.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(rebind))


def _tap(sim, key, wait=1.0):
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=30)
    time.sleep(wait)


def _img(sim):
    return Image.open(io.BytesIO(sim.screenshot())).convert("RGB")


def _ammo(sim):
    """The status bar's ammo number."""
    return _img(sim).crop((0, 228, 48, 252)).tobytes()


def _view(sim):
    """The 3D view above the weapon (the status-bar face animates, so it is
    left out)."""
    return _img(sim).crop((0, 62, 320, 150))


def _moved(a, b):
    """True when the views differ in over 8% of their pixels: a turn, a
    strafe or Doom's menu (~20%). A monster idling in view is ~0.004%."""
    diff = ImageChops.difference(a, b).convert("L").point(
        lambda v: 255 if v else 0)
    return diff.histogram()[255] > 0.08 * a.width * a.height


def _settle(sim, timeout=30):
    """Wait until the view stops changing (a menu wipe, a level load): the
    emulated Doom is slow, and slower still when the suite runs in parallel.
    Best effort: the title screen and its demo never hold still, and the
    assertions that follow say what they need."""
    deadline = time.time() + timeout
    prev = _view(sim)
    while time.time() < deadline:
        time.sleep(0.7)
        cur = _view(sim)
        if not _moved(prev, cur):
            return
        prev = cur
    return


def _launch(sim, rebind=None):
    _stage(sim, rebind)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("doom")
    sim.wait_for_log(r"DOOM: frame done", timeout=150, since_seq=seq)
    _settle(sim)


def _assert_in_a_level(sim):
    """A level under nobody's control holds still; the title's demo, which
    cycles in behind a menu, never does."""
    v = _view(sim)
    time.sleep(1.5)
    assert not _moved(v, _view(sim)), "no level is running (a demo, or a menu)"


def _start_game(sim, rebind=None):
    _launch(sim, rebind)
    for _ in range(5):  # title, main menu, episode, skill: Enter
        _tap(sim, "enter")
        _settle(sim)
    time.sleep(2)
    _settle(sim)
    _assert_in_a_level(sim)


def test_default_bindings(simulator):
    sim = simulator
    _start_game(sim)
    a0 = _ammo(sim)
    _tap(sim, "f4")                      # A = fire
    a1 = _ammo(sim)
    assert a1 != a0
    _tap(sim, "del")                     # X = next weapon: pistol -> fist
    assert _ammo(sim) != a1
    _tap(sim, "backspace")               # Y = previous weapon: pistol again
    assert _ammo(sim) == a1
    for key in ("right", "left", "f2", "f3"):  # D-pad turn, L/R strafe
        v = _view(sim)
        _tap(sim, key)
        assert _moved(v, _view(sim)), key


@pytest.mark.parametrize("key", ["f1", "esc"])
def test_start_and_esc_toggle_the_menu(simulator, key):
    sim = simulator
    _start_game(sim)
    v = _view(sim)
    _tap(sim, key)
    _settle(sim)
    assert _moved(v, _view(sim)), f"{key} did not open the menu"
    _tap(sim, key)
    _settle(sim)
    assert not _moved(v, _view(sim)), f"a second {key} did not close it"


def _to_the_skill_menu(sim, taps=3):
    """F4 (A) alone: title -> main menu, New Game, episode. The next press
    picks the skill and loads the level."""
    for _ in range(taps):
        _tap(sim, "f4")


def test_a_confirms_in_the_menu(simulator):
    """A alone takes the title screen through New Game, the episode and the
    skill menus, into a level the player controls. On a build where A is fire
    only, each F4 reopens the main menu and the title cycles into its demo,
    which never holds still and does not answer the D-pad."""
    sim = simulator
    _launch(sim)
    _to_the_skill_menu(sim, 4)           # the fourth picks the skill
    time.sleep(2)
    _settle(sim)
    _assert_in_a_level(sim)
    v = _view(sim)
    _tap(sim, "right")                   # and the player turns
    assert _moved(v, _view(sim))
    a0 = _ammo(sim)
    _tap(sim, "f4")                      # a press that began in the game fires
    assert _ammo(sim) != a0


def test_a_held_from_the_menu_into_the_level_does_not_fire(simulator):
    """The A press that picks the skill is still down when the level starts:
    it is the menu's, and must not fire until it is released and pressed
    again."""
    sim = simulator
    _launch(sim)
    _to_the_skill_menu(sim)
    sim.call("inject_button", {"button": "f4", "action": "press"})
    try:
        time.sleep(3)
        _settle(sim)
        _assert_in_a_level(sim)              # the held press picked the skill
        a0 = _ammo(sim)
        time.sleep(3)
        assert _ammo(sim) == a0, "the held menu press fired"
    finally:
        sim.call("inject_button", {"button": "f4", "action": "release"})
    time.sleep(1)
    assert _ammo(sim) == a0
    _tap(sim, "f4")
    assert _ammo(sim) != a0


def test_a_held_while_start_closes_the_menu_does_not_fire(simulator):
    """A pressed on a menu item and still held when Start closes the menu must
    not fire in the resumed game. The skill-pick case above cannot show this:
    Doom's level load clears every held key itself, so only a menu that closes
    without a level load tests that a menu press stays the menu's."""
    sim = simulator
    _start_game(sim)
    _tap(sim, "f1")                          # Start: menu open, game paused
    _settle(sim)
    sim.call("inject_button", {"button": "f4", "action": "press"})   # New Game
    try:
        time.sleep(1.0)
        _settle(sim)
        _tap(sim, "f1")                      # Start: menu closes, game resumes
        _settle(sim)
        a0 = _ammo(sim)
        time.sleep(2.0)
        assert _ammo(sim) == a0, "the menu's held A fired in the game"
    finally:
        sim.call("inject_button", {"button": "f4", "action": "release"})


def test_a_answers_yes_at_the_quit_prompt(simulator):
    """Start opens the menu, Up wraps to Quit Game, A raises the prompt and A
    answers yes: the 'y' path end to end, and the app returns."""
    sim = simulator
    _start_game(sim)
    _tap(sim, "f1")
    _settle(sim)
    _tap(sim, "up")
    _tap(sim, "f4")
    _settle(sim)
    _tap(sim, "f4")
    r = sim.wait_for_exit(timeout=60)
    assert r["result"] == "returned", r


def test_follows_a_rebinding(simulator):
    """A and R swap keys (F3 fires, F4 strafes right)."""
    sim = simulator
    _start_game(sim, rebind={"a": ["F3"], "r": ["F4"]})
    a0 = _ammo(sim)
    v = _view(sim)
    _tap(sim, "f4")                      # R now: strafes, does not fire
    assert _ammo(sim) == a0
    assert _moved(v, _view(sim))
    _tap(sim, "f3")                      # A now: fires
    assert _ammo(sim) != a0
