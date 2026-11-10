"""The in-repo Lua games read picocalc.gamepad (issue #25): Snake, the
platformer demo and Guinea Pig Run. Minesweeper's and Neurogram's tests moved
with them to PicoDeck/minesweeper and PicoDeck/neurogram.

Each test stages the real app from apps/ and drives it with the keys the
gamepad binds (F4 for A, an arrow for a D-pad direction), never through BTN_*
directly. The rebinding tests write a per-app /data/<id>/gamepad.json before
launch and check that the bound key, not the default one, drives the game.
The "without a gamepad" tests stage the app with `picocalc.gamepad = nil` put
before its main.lua, as on firmware older than API version 9, and drive it
with the keys it used before the gamepad.

Each test stages one app, so its card holds only hello besides it
(tests/e2e/README.md, "App cap").
"""

import json
import shutil
import time
from pathlib import Path

import pytest

from helpers import app_id_of

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

APPS = Path(__file__).resolve().parents[2] / "apps"


def _stage(sim, name, rebind=None, no_gamepad=False):
    sd = Path(sim.sd_card_path)
    shutil.copytree(APPS / name, sd / "apps" / name, dirs_exist_ok=True)
    if no_gamepad:
        main = sd / "apps" / name / "main.lua"
        main.write_text("picocalc.gamepad = nil\n" + main.read_text())
    if rebind:
        path = sd / "data" / app_id_of(sd, name) / "gamepad.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(rebind))


def _tap(sim, key):
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    time.sleep(0.15)


def _launch(sim, name, rebind=None, no_gamepad=False):
    _stage(sim, name, rebind, no_gamepad)
    sim.launch_app(name)
    time.sleep(1.0)


# ── snake ────────────────────────────────────────────────────────────────────


def _snake_head(sim):
    """Centre (x, y) of the snake's bright-green head cell, or None."""
    img = sim.screenshot_pil().convert("RGB")
    xs, ys = [], []
    w, h = img.size
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if g > 200 and r < 100 and b < 100:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return sum(xs) / len(xs), sum(ys) / len(ys)


def test_snake_turns_on_the_bound_arrow(simulator):
    """The snake starts heading right; Up (PAD_UP's default key) turns it."""
    sim = simulator
    _launch(sim, "snake")
    x0, y0 = _snake_head(sim)
    _tap(sim, "up")
    time.sleep(1.0)
    x1, y1 = _snake_head(sim)
    assert y1 < y0 - 20, (y0, y1)
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)


def test_snake_follows_a_rebinding(simulator):
    """With Up rebound to W, the Up arrow no longer steers and W does."""
    sim = simulator
    _launch(sim, "snake", rebind={"up": ["W"]})
    x0, y0 = _snake_head(sim)
    _tap(sim, "up")
    time.sleep(1.0)
    x1, y1 = _snake_head(sim)
    assert abs(y1 - y0) < 6, (y0, y1)     # still heading right
    _tap(sim, "w")
    time.sleep(1.0)
    x2, y2 = _snake_head(sim)
    assert y2 < y1 - 20, (y1, y2)
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)


# ── platformer_demo ──────────────────────────────────────────────────────────


def _platformer_in_menu(sim):
    """The menu draws the yellow 'PLATFORMER DEMO' title around y=80."""
    img = sim.screenshot_pil().convert("RGB")
    px = img.load()
    return sum(1 for y in range(78, 96) for x in range(60, 260)
               if px[x, y][0] > 200 and px[x, y][1] > 200 and px[x, y][2] < 120) > 40


def test_platformer_starts_on_a(simulator):
    sim = simulator
    _launch(sim, "platformer_demo")
    assert _platformer_in_menu(sim)
    _tap(sim, "f4")                      # PAD_A
    time.sleep(0.5)
    assert not _platformer_in_menu(sim)


def test_platformer_menu_also_takes_enter(simulator):
    """Enter is not bound by default, and the menu still starts on it."""
    sim = simulator
    _launch(sim, "platformer_demo")
    assert _platformer_in_menu(sim)
    _tap(sim, "enter")
    time.sleep(0.5)
    assert not _platformer_in_menu(sim)


def test_platformer_menu_leaves_enter_to_its_button(simulator):
    """With Enter bound to B, Enter is B alone and does not start the game."""
    sim = simulator
    _launch(sim, "platformer_demo", rebind={"b": ["Enter"]})
    _tap(sim, "enter")
    time.sleep(0.3)
    assert _platformer_in_menu(sim)
    _tap(sim, "f4")
    time.sleep(0.5)
    assert not _platformer_in_menu(sim)


def test_platformer_follows_a_rebinding_and_relabels_its_hint(simulator):
    sim = simulator
    _launch(sim, "platformer_demo")
    default_menu = sim.screenshot()
    sim.exit_app()
    sim.wait_for_exit(timeout=10)

    sim2 = sim
    _launch(sim2, "platformer_demo", rebind={"a": ["Z"]})
    assert sim2.screenshot() != default_menu      # the hint names Z, not F4
    _tap(sim2, "f4")
    time.sleep(0.3)
    assert _platformer_in_menu(sim2)
    _tap(sim2, "z")
    time.sleep(0.5)
    assert not _platformer_in_menu(sim2)


# ── guinea_pig ───────────────────────────────────────────────────────────────

def _guinea_in_menu(sim):
    """The title menu has a dark-green footer panel at the bottom-left."""
    img = sim.screenshot_pil().convert("RGB")
    r, g, b = img.getpixel((2, 312))
    return abs(r - 30) < 10 and abs(g - 60) < 10 and abs(b - 30) < 10


def test_guinea_pig_starts_on_a(simulator):
    sim = simulator
    _launch(sim, "guinea_pig")
    assert _guinea_in_menu(sim)
    _tap(sim, "f4")                      # PAD_A
    time.sleep(0.8)
    assert not _guinea_in_menu(sim)


def test_guinea_pig_menu_also_takes_enter(simulator):
    sim = simulator
    _launch(sim, "guinea_pig")
    assert _guinea_in_menu(sim)
    _tap(sim, "enter")
    time.sleep(0.8)
    assert not _guinea_in_menu(sim)


def test_guinea_pig_follows_a_rebinding(simulator):
    sim = simulator
    _launch(sim, "guinea_pig", rebind={"a": ["Z"]})
    _tap(sim, "f4")
    time.sleep(0.3)
    assert _guinea_in_menu(sim)
    _tap(sim, "z")
    time.sleep(0.8)
    assert not _guinea_in_menu(sim)


# ── Without a gamepad (firmware older than API version 9) ────────────────────


def _still_running(sim):
    """No Lua error has ended the app (--test-mode returns from one at once)."""
    return sim.get_status()["app"].get("running") is True


def test_snake_without_gamepad_steers_on_the_arrows(simulator):
    sim = simulator
    _launch(sim, "snake", no_gamepad=True)
    x0, y0 = _snake_head(sim)
    _tap(sim, "up")
    time.sleep(1.0)
    x1, y1 = _snake_head(sim)
    assert y1 < y0 - 20, (y0, y1)
    sim.keypress("esc")
    assert sim.wait_for_exit(timeout=10)["result"] == "returned"


def test_platformer_without_gamepad_starts_on_enter(simulator):
    sim = simulator
    _launch(sim, "platformer_demo", no_gamepad=True)
    assert _platformer_in_menu(sim)
    _tap(sim, "enter")
    time.sleep(0.5)
    assert not _platformer_in_menu(sim)
    for key in ("right", "up", "left", "enter"):
        _tap(sim, key)                   # move, jump
    assert _still_running(sim)
    sim.exit_app()
    assert sim.wait_for_exit(timeout=10)["result"] != "error"


def test_guinea_pig_without_gamepad_plays_on_the_old_keys(simulator):
    sim = simulator
    _launch(sim, "guinea_pig", no_gamepad=True)
    assert _guinea_in_menu(sim)
    _tap(sim, "enter")                   # start
    time.sleep(0.8)
    assert not _guinea_in_menu(sim)
    for key in ("right", "enter", "f2", "f1", "up", "down", "left"):
        _tap(sim, key)                   # move, jump, dash, squeak, hide
    time.sleep(0.5)
    assert _still_running(sim)
    sim.exit_app()
    assert sim.wait_for_exit(timeout=10)["result"] != "error"
