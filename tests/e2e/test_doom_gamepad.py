"""DOOM reads the gamepad (issue #25).

Doom lives in its own repo (github.com/PicoDeck/doom): the tests stage its
built main.elf and doom1.wad (PICODECK_DOOM_DIR, default ~/Projects/PicoDeck/
doom; `make` there) and skip without them (skip_allowlist.txt). They start
E1M1 and read the effect off the screen: ammo for fire and weapon changes, a
share of changed view pixels for turning, strafing and Doom's menu.

Injected character keys tap for one poll, too short for Doom's 35 Hz tic, so
the rebinding test swaps button keys (F3, F4) rather than letters. The `pad`
and `keydown` dev commands are not served while a native app runs in the
simulator, so every input here is a button-key click.
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


def _start_game(sim, rebind=None):
    _launch(sim, rebind)
    for _ in range(5):  # title, main menu, episode, skill: Enter
        _tap(sim, "enter")
        _settle(sim)
    time.sleep(2)
    _settle(sim)


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


def test_a_confirms_in_the_menu(simulator):
    """A (F4) alone takes the title screen through New Game, the episode and
    the skill menus, and starts the level."""
    sim = simulator
    _launch(sim)
    for _ in range(4):        # title, main menu, New Game, episode
        _tap(sim, "f4")
    v = _view(sim)            # the skill menu
    _tap(sim, "f4")           # Hurt Me Plenty: the level loads
    deadline = time.time() + 60
    while not _moved(v, _view(sim)) and time.time() < deadline:
        time.sleep(1)
    assert _moved(v, _view(sim)), "A did not start the game from the menu"
    time.sleep(2)
    _settle(sim)
    a0 = _ammo(sim)
    _tap(sim, "f4")           # a press that began in the game fires
    assert _ammo(sim) != a0


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
