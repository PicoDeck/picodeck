"""The in-repo Lua games read picocalc.gamepad (issue #25).

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


def test_nonogram_menu_also_opens_on_enter(simulator):
    """Enter opens a puzzle from the menu, as before; in play it is not A."""
    sim = simulator
    _stage(sim, "nonogram")
    sim.launch_app("nonogram")
    mark = _nonogram_to_play(sim, "enter")
    _tap(sim, "enter")
    assert not [t for t in _lines(sim, mark) if t.startswith("NG:FILL")]
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


# ── minesweeper ──────────────────────────────────────────────────────────────
#
# The 9x9 grid of 32 px cells starts at (16, 36) and the cursor at cell (5, 5).
# The app logs nothing, so the tests compare crops of the screen.

_MINE_CELL = 32


def _mine_cell(sim, cx, cy):
    """The pixels of 1-based cell (cx, cy)."""
    x, y = 16 + (cx - 1) * _MINE_CELL, 36 + (cy - 1) * _MINE_CELL
    return sim.screenshot_pil().convert("RGB").crop(
        (x, y, x + _MINE_CELL, y + _MINE_CELL)).tobytes()


def _spy_footer(sim, name, **kw):
    """Stages `name` with its drawText wrapped to log the footer hint.

    The 9x9 grid of 32 px cells ends at y=324, so on the 320 px screen the
    footer hint is drawn off the bottom and a screenshot cannot show it.
    """
    _stage(sim, name, **kw)
    main = Path(sim.sd_card_path) / "apps" / name / "main.lua"
    main.write_text(
        "local dt = picocalc.display.drawText\n"
        "picocalc.display.drawText = function(x, y, t, ...)\n"
        "  if t:find('Esc:', 1, true) then picocalc.sys.log('HINT:' .. t) end\n"
        "  return dt(x, y, t, ...)\n"
        "end\n" + main.read_text())


def _hint(sim):
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("minesweeper")
    return sim.wait_for_log(r"HINT:", timeout=10, since_seq=seq)


def test_minesweeper_moves_and_acts_on_the_bound_keys(simulator):
    sim = simulator
    _launch(sim, "minesweeper")
    start, right = _mine_cell(sim, 5, 5), _mine_cell(sim, 6, 5)
    _tap(sim, "right")                   # PAD_RIGHT: the cursor leaves (5, 5)
    assert _mine_cell(sim, 5, 5) != start
    assert _mine_cell(sim, 6, 5) != right
    _tap(sim, "left")
    assert _mine_cell(sim, 5, 5) == start
    _tap(sim, "f5")                      # PAD_B flags the cell
    flagged = _mine_cell(sim, 5, 5)
    assert flagged != start
    _tap(sim, "f5")
    assert _mine_cell(sim, 5, 5) == start
    _tap(sim, "backspace")               # PAD_Y marks it with a question mark
    marked = _mine_cell(sim, 5, 5)
    assert marked not in (start, flagged)
    _tap(sim, "backspace")
    assert _mine_cell(sim, 5, 5) == start
    _tap(sim, "f4")                      # PAD_A reveals it
    time.sleep(0.3)
    assert _mine_cell(sim, 5, 5) != start
    assert _still_running(sim)
    sim.keypress("esc")
    assert sim.wait_for_exit(timeout=10)["result"] == "returned"


def test_minesweeper_does_not_reveal_on_enter_in_play(simulator):
    sim = simulator
    _launch(sim, "minesweeper")
    start = _mine_cell(sim, 5, 5)
    _tap(sim, "enter")
    assert _mine_cell(sim, 5, 5) == start
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)


def test_minesweeper_follows_a_rebinding_and_relabels_its_hint(simulator):
    sim = simulator
    _spy_footer(sim, "minesweeper")
    assert "HINT:F4:Reveal F5:Flag Del:Chord Bksp:? Esc:Exit" in _hint(sim)
    sim.exit_app()
    sim.wait_for_exit(timeout=10)

    _spy_footer(sim, "minesweeper", rebind={"a": ["Z"], "b": ["X"], "right": ["D"]})
    assert "HINT:Z:Reveal X:Flag Del:Chord Bksp:? Esc:Exit" in _hint(sim)
    time.sleep(1.0)
    start = _mine_cell(sim, 5, 5)
    _tap(sim, "f4")                      # no longer A
    _tap(sim, "f5")                      # no longer B
    _tap(sim, "right")                   # no longer PAD_RIGHT
    assert _mine_cell(sim, 5, 5) == start
    _tap(sim, "x")                       # B is X now
    assert _mine_cell(sim, 5, 5) != start
    _tap(sim, "x")
    _tap(sim, "d")                       # PAD_RIGHT is D now
    assert _mine_cell(sim, 5, 5) != start
    _tap(sim, "left")
    _tap(sim, "z")                       # A is Z now
    time.sleep(0.3)
    assert _mine_cell(sim, 5, 5) != start
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)


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


def test_nonogram_without_gamepad_plays_on_the_old_keys(simulator):
    sim = simulator
    _stage(sim, "nonogram", no_gamepad=True)
    sim.launch_app("nonogram")
    mark = _nonogram_to_play(sim, "enter")       # Enter opens the first puzzle
    _tap(sim, "enter")                           # Enter fills
    sim.wait_for_log(r"^NG:FILL ", timeout=10, since_seq=mark)
    for key in ("right", "down", "f5", "backspace", "f2", "f3", "f4", "tab",
                "right", "tab"):
        _tap(sim, key)                   # move, block, mark, undo, redo, auto-X, latch
    assert _still_running(sim)
    sim.keypress("esc")                          # back to the menu
    time.sleep(0.5)
    sim.keypress("esc")                          # quit
    assert sim.wait_for_exit(timeout=10)["result"] == "returned"


def test_minesweeper_without_gamepad_plays_on_the_old_keys(simulator):
    sim = simulator
    _spy_footer(sim, "minesweeper", no_gamepad=True)
    assert "HINT:F4:Reveal F5:Flag Del:Chord Bksp:? Esc:Exit" in _hint(sim)
    time.sleep(1.0)
    start = _mine_cell(sim, 5, 5)
    _tap(sim, "f5")                      # flag
    assert _mine_cell(sim, 5, 5) != start
    _tap(sim, "f5")
    assert _mine_cell(sim, 5, 5) == start
    _tap(sim, "right")
    assert _mine_cell(sim, 5, 5) != start
    _tap(sim, "left")
    _tap(sim, "f4")                      # reveal
    time.sleep(0.3)
    assert _mine_cell(sim, 5, 5) != start
    for key in ("backspace", "delete"):
        _tap(sim, key)                   # mark, chord
    assert _still_running(sim)
    sim.keypress("esc")
    assert sim.wait_for_exit(timeout=10)["result"] == "returned"
