"""The in-repo Lua games read picocalc.gamepad (issue #25).

Each test stages the real app from apps/ and drives it with the keys the
gamepad binds (F4 for A, an arrow for a D-pad direction), never through BTN_*
directly. The rebinding tests write a per-app /data/<id>/gamepad.json before
launch and check that the bound key, not the default one, drives the game.
"""

import json
import shutil
import time
from pathlib import Path

from helpers import app_id_of

APPS = Path(__file__).resolve().parents[2] / "apps"


def _stage(sim, name, rebind=None):
    sd = Path(sim.sd_card_path)
    shutil.copytree(APPS / name, sd / "apps" / name, dirs_exist_ok=True)
    if rebind:
        path = sd / "data" / app_id_of(sd, name) / "gamepad.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(rebind))


def _tap(sim, key):
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    time.sleep(0.15)


def _launch(sim, name, rebind=None):
    _stage(sim, name, rebind)
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
    _tap(sim, "enter")                   # not bound by default: no effect
    time.sleep(0.3)
    assert _platformer_in_menu(sim)
    _tap(sim, "f4")                      # PAD_A
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
    _tap(sim, "enter")                   # not bound by default: no effect
    time.sleep(0.3)
    assert _guinea_in_menu(sim)
    _tap(sim, "f4")                      # PAD_A
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


# ── nonogram ─────────────────────────────────────────────────────────────────


def _lines(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _nonogram_to_play(sim, a_key):
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.wait_for_log(r"^NG:MENU ", timeout=15, since_seq=seq)
    _tap(sim, a_key)
    sim.wait_for_log(r"^NG:SCENE play", timeout=10, since_seq=seq)
    return sim.get_log_buffer(tail=1).get("next_seq", seq)


def test_nonogram_fills_on_a(simulator):
    sim = simulator
    _stage(sim, "nonogram")
    sim.launch_app("nonogram")
    mark = _nonogram_to_play(sim, "f4")          # A opens the first puzzle
    _tap(sim, "enter")                           # no longer a fill key
    assert not [t for t in _lines(sim, mark) if t.startswith("NG:FILL")]
    _tap(sim, "f4")                              # A fills the cell
    sim.wait_for_log(r"^NG:FILL ", timeout=10, since_seq=mark)
    sim.keypress("esc")


def test_nonogram_follows_a_rebinding(simulator):
    sim = simulator
    _stage(sim, "nonogram", rebind={"a": ["Z"]})
    sim.launch_app("nonogram")
    mark = _nonogram_to_play(sim, "z")
    _tap(sim, "f4")
    assert not [t for t in _lines(sim, mark) if t.startswith("NG:FILL")]
    _tap(sim, "z")
    sim.wait_for_log(r"^NG:FILL ", timeout=10, since_seq=mark)
    sim.keypress("esc")
