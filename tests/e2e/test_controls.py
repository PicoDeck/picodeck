"""System menu -> Settings -> Controls: rebinding the gamepad (issue #24).

The page edits /system/gamepad.json at the launcher and, inside a running
game, that game's override (/data/<id>/gamepad.json, "This game", the
default) or the global map ("All games"). It saves when it is left (Esc),
rebuilds the installed map, and logs its notices and saves as [CONTROLS]
lines. The fixture (apps/gamepad_test) logs its labels at start, then every
gamepad edge. The page's state rules are unit-tested in
tests/unit/test_gamepad_edit.c; these tests drive the real page with
injected keys, as a player would.
"""

import json
import os
import shutil
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import stage_lua_app

FIXTURE = Path(__file__).parent / "apps" / "gamepad_test" / "main.lua"
GAME = "gamepad_ctl"                 # a fixture copy that runs long enough
GAME_ID = "com.test.gamepad_ctl"
OTHER = "gamepad_ctl_other"

# Main page: Battery, Settings, ...  Settings page: Brightness, Battery %,
# Show FPS, Controls, ...  The Controls cursor starts on Up / Primary, with
# the This game / All games row above it inside a game.
TO_CONTROLS = ["down", "enter", "down", "down", "down", "enter"]
TO_A = ["down"] * 4


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _mark(sim):
    return sim.get_log_buffer(tail=1).get("next_seq", 0)


def _keys(sim, keys, delay=0.2):
    for k in keys:
        sim.keypress(k)
        time.sleep(delay)


def _open_controls(sim):
    sim.keypress("menu")
    time.sleep(0.3)
    _keys(sim, TO_CONTROLS)


def _leave(sim, since, closes=2):
    """Esc out of Controls (it saves), then out of Settings and the menu."""
    _keys(sim, ["esc"])
    line = sim.wait_for_log(r"^\[CONTROLS\] (bindings saved|Could not save)",
                            timeout=10, since_seq=since)
    if "Could not" in line:
        time.sleep(1.7)  # the error stays up for 1.5 s
    _keys(sim, ["esc"] * closes)


def _read(sim, rel):
    path = Path(sim.sd_card_path) / rel
    return json.loads(path.read_text()) if path.exists() else None


def _write(sim, rel, content):
    path = Path(sim.sd_card_path) / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(content))


def _stage_games(sim):
    """Two copies of the gamepad fixture, as the SD card's only apps. The
    launcher keeps the first MAX_APPS (64) apps in directory order, and the
    manifest's apps plus these would pass it, so which ones it kept would
    depend on the filesystem (tmpfs lists new entries first, ext4 and btrfs
    do not). launch_app rescans /apps when it misses a name."""
    apps = Path(sim.sd_card_path) / "apps"
    for app in apps.iterdir():
        shutil.rmtree(app)
    code = FIXTURE.read_text().replace("+ 20000", "+ 120000")
    stage_lua_app(Path(sim.sd_card_path), GAME, code, id=GAME_ID)
    stage_lua_app(Path(sim.sd_card_path), OTHER, code, id="com.test.gamepad_ctl_other")


def _start(sim, app):
    seq = _mark(sim)
    sim.launch_app(app)
    sim.wait_for_log(r"^GP:READY$", timeout=10, since_seq=seq)
    return seq


def _label(sim, seq):
    return next(t for t in _texts(sim, seq) if t.startswith("GP:LABEL "))


def _pad(sim, key):
    """The gamepad edges `key` gives in the running fixture (without the
    held/btn fields). Tab (Select) follows it as an end marker."""
    mark = _mark(sim)
    _keys(sim, [key, "tab"], delay=0.15)
    sim.wait_for_log(r"^GP:RELEASE select$", timeout=10, since_seq=mark)
    edges = [t.split(" held=")[0] for t in _texts(sim, mark)
             if t.startswith("GP:PRESS ") or t.startswith("GP:RELEASE ")]
    assert edges[-2:] == ["GP:PRESS select", "GP:RELEASE select"], edges
    return edges[:-2]


A_TAP = ["GP:PRESS a", "GP:RELEASE a"]


def _finish(sim, seq):
    sim.keypress("esc")
    sim.wait_for_log(r"^GP:DONE$", timeout=10, since_seq=seq)
    sim.wait_for_exit(timeout=10)


# ── All games, from the launcher ────────────────────────────────────────────


def test_launcher_rebinds_a_to_z(simulator):
    """At the launcher the page edits the global map: A on Z, and F4 (A's
    old key) no longer drives anything in the next app."""
    sim = simulator
    _stage_games(sim)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "z"])
    _leave(sim, mark)
    assert _read(sim, "system/gamepad.json") == {"a": ["Z"]}, _texts(sim, mark)

    seq = _start(sim, GAME)
    assert _label(sim, seq).startswith("GP:LABEL a=Z a_alt=nil"), _texts(sim, seq)
    assert _pad(sim, "f4") == []
    assert _pad(sim, "z") == A_TAP
    _finish(sim, seq)


def test_moving_a_key_names_both_buttons(simulator):
    """F5 (B's key) bound to A leaves B unbound, and says so. Backspace (Y's
    key) can be captured: Clear is C, not Backspace."""
    sim = simulator
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "f5", "right", "enter", "backspace"])
    _leave(sim, mark)
    texts = _texts(sim, mark)
    assert "[CONTROLS] F5 moved from B to A" in texts, texts
    assert "[CONTROLS] Bksp moved from Y to A Alt" in texts, texts
    assert _read(sim, "system/gamepad.json") == {
        "a": ["F5", "Bksp"], "b": [], "y": []}


def test_clear_unbinds_the_focused_cell(simulator):
    """C clears Up's primary; W, Up's alternate from the file, stays."""
    sim = simulator
    _stage_games(sim)
    _write(sim, "system/gamepad.json", {"up": ["Up", "W"]})
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, ["c"])
    _leave(sim, mark)
    assert _read(sim, "system/gamepad.json") == {"up": ["", "W"]}

    seq = _start(sim, GAME)
    assert _pad(sim, "up") == []
    assert _pad(sim, "w") == ["GP:PRESS up", "GP:RELEASE up"]
    _finish(sim, seq)


def test_reset_to_defaults_takes_two_presses(simulator):
    """R once asks again; R twice resets, and the global file goes."""
    sim = simulator
    _write(sim, "system/gamepad.json", {"a": ["Z"]})
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, ["r"])
    sim.wait_for_log(r"^\[CONTROLS\] Press R again to reset to defaults$",
                     timeout=5, since_seq=mark)
    assert _read(sim, "system/gamepad.json") == {"a": ["Z"]}
    _keys(sim, ["r"])
    sim.wait_for_log(r"^\[CONTROLS\] Reset to defaults$", timeout=5, since_seq=mark)
    _leave(sim, mark)
    assert not (Path(sim.sd_card_path) / "system" / "gamepad.json").exists()


# ── Capture: refused keys, the menu key, Esc ────────────────────────────────


def test_refused_keys_keep_capturing_and_menu_key_cancels(simulator):
    """A digit is refused by name and Shift quietly (it starts the menu
    key's chord, Shift+F5); capture goes on and Z then binds. The menu key
    cancels a capture: B keeps F5, and a key typed after it binds nothing."""
    sim = simulator
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "shift", "1", "z"])
    _keys(sim, ["down", "enter", "shift", "menu", "x"])
    _leave(sim, mark)
    texts = _texts(sim, mark)
    assert "[CONTROLS] 1 can't be bound" in texts, texts
    assert not [t for t in texts if "Shift" in t], texts
    assert _read(sim, "system/gamepad.json") == {"a": ["Z"]}, texts


def test_corrupt_file_is_shown_as_defaults_and_replaced(simulator):
    """Launches ignore a corrupt global file, so the page shows the defaults
    (and says so), and a save replaces the file."""
    sim = simulator
    path = Path(sim.sd_card_path) / "system" / "gamepad.json"
    path.write_text('{"a": ["Z"')
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "f5"])
    _leave(sim, mark)
    texts = _texts(sim, mark)
    assert "[CONTROLS] Ignored a corrupt bindings file" in texts, texts
    assert "[CONTROLS] F5 moved from B to A" in texts, texts
    assert _read(sim, "system/gamepad.json") == {"a": ["F5"], "b": []}


@pytest.mark.skipif(hasattr(os, "geteuid") and os.geteuid() == 0,
                    reason="root reads a mode-000 file")
def test_unreadable_file_keeps_the_page_shut(simulator):
    """A bindings file that is there but cannot be read (here: no read
    permission; on the device a read error or no memory) may read fine next
    time: the page says so and does not open, so it cannot show the
    defaults and then save them over the file."""
    sim = simulator
    path = Path(sim.sd_card_path) / "system" / "gamepad.json"
    path.write_text('{"a": ["Z"]}')
    path.chmod(0)
    try:
        mark = _mark(sim)
        _open_controls(sim)
        sim.wait_for_log(r"^\[CONTROLS\] Could not read the bindings$",
                         timeout=5, since_seq=mark)
        time.sleep(1.7)  # the message stays up for 1.5 s
        _keys(sim, ["esc", "esc"])                     # Settings, the menu
        assert not [t for t in _texts(sim, mark)
                    if t.startswith("[CONTROLS] bindings saved")]
    finally:
        path.chmod(0o644)
    assert _read(sim, "system/gamepad.json") == {"a": ["Z"]}


def test_esc_can_be_bound(simulator):
    """While capturing, Esc is a key like any other: it binds and does not
    leave the page (the next cell's binding lands too)."""
    sim = simulator
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "esc", "down", "enter", "x"])
    _leave(sim, mark)
    assert _read(sim, "system/gamepad.json") == {"a": ["Esc"], "b": ["X"]}


def test_failed_save_keeps_the_previous_file(simulator):
    """When the save fails (here the .tmp name is taken by a directory) the
    page says so, the old file stays, and the next app gets the old map."""
    sim = simulator
    _stage_games(sim)
    _write(sim, "system/gamepad.json", {"a": ["Z"]})
    (Path(sim.sd_card_path) / "system" / "gamepad.json.tmp").mkdir()
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["down", "enter", "q"])
    _leave(sim, mark)
    assert "[CONTROLS] Could not save the bindings" in _texts(sim, mark)
    assert _read(sim, "system/gamepad.json") == {"a": ["Z"]}

    seq = _start(sim, GAME)
    assert _label(sim, seq).startswith("GP:LABEL a=Z"), _texts(sim, seq)
    assert _pad(sim, "q") == []
    assert _pad(sim, "f5") == ["GP:PRESS b", "GP:RELEASE b"]
    _finish(sim, seq)


# ── This game, inside a running game ────────────────────────────────────────


def test_this_game_override_applies_at_once_and_only_there(simulator):
    """Inside a game the page edits that game's override: the game sees A on
    Z as soon as the menu closes, and another game keeps the global map."""
    sim = simulator
    _stage_games(sim)
    seq = _start(sim, GAME)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "z"])
    _leave(sim, mark)
    assert _read(sim, f"data/{GAME_ID}/gamepad.json") == {"a": ["Z"]}
    assert _read(sim, "system/gamepad.json") is None
    time.sleep(0.3)
    assert _pad(sim, "f4") == []
    assert _pad(sim, "z") == A_TAP
    _finish(sim, seq)

    seq = _start(sim, OTHER)
    assert _label(sim, seq).startswith("GP:LABEL a=F4"), _texts(sim, seq)
    assert _pad(sim, "z") == []
    assert _pad(sim, "f4") == A_TAP
    _finish(sim, seq)


def test_this_game_moving_an_inherited_key_overrides_its_button(simulator):
    """F5 moved off inherited B onto A: both become part of the override."""
    sim = simulator
    _stage_games(sim)
    seq = _start(sim, GAME)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "f5"])
    _leave(sim, mark)
    assert "[CONTROLS] F5 moved from B to A" in _texts(sim, mark)
    assert _read(sim, f"data/{GAME_ID}/gamepad.json") == {"a": ["F5"], "b": []}
    assert _read(sim, "system/gamepad.json") is None
    _finish(sim, seq)


def test_all_games_from_inside_a_game(simulator):
    """The scope row switched to All games edits the global map."""
    sim = simulator
    _stage_games(sim)
    seq = _start(sim, GAME)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, ["up", "enter", "down"] + TO_A + ["enter", "k"])
    _leave(sim, mark)
    assert _read(sim, "system/gamepad.json") == {"a": ["K"]}
    assert _read(sim, f"data/{GAME_ID}/gamepad.json") is None
    _finish(sim, seq)


def test_reset_this_game_deletes_the_override(simulator):
    sim = simulator
    _stage_games(sim)
    _write(sim, "system/gamepad.json", {"up": ["Up", "W"]})
    _write(sim, f"data/{GAME_ID}/gamepad.json", {"a": ["Z"]})
    seq = _start(sim, GAME)
    assert _label(sim, seq).startswith("GP:LABEL a=Z"), _texts(sim, seq)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, ["r", "r"])
    _leave(sim, mark)
    assert "[CONTROLS] This game reset to All games" in _texts(sim, mark)
    assert not (Path(sim.sd_card_path) / "data" / GAME_ID / "gamepad.json").exists()
    assert _read(sim, "system/gamepad.json") == {"up": ["Up", "W"]}
    time.sleep(0.3)
    assert _pad(sim, "z") == []
    assert _pad(sim, "f4") == A_TAP
    _finish(sim, seq)


def test_closing_key_does_not_press_its_button(simulator):
    """Esc bound to A in this game: the Esc that closes the menu is held when
    the menu goes, and the game sees no A press (and no Esc press) from it."""
    sim = simulator
    _stage_games(sim)
    seq = _start(sim, GAME)
    mark = _mark(sim)
    _open_controls(sim)
    _keys(sim, TO_A + ["enter", "esc"])
    _leave(sim, mark)
    assert _read(sim, f"data/{GAME_ID}/gamepad.json") == {"a": ["Esc"]}
    time.sleep(0.5)
    presses = [t for t in _texts(sim, mark) if t.startswith("GP:PRESS")]
    assert presses == [], presses
    assert sim.call("get_running_app").get("running"), "the game exited"
    # The next Esc is a press: A, and the fixture's quit.
    sim.keypress("esc")
    sim.wait_for_log(r"^GP:DONE$", timeout=10, since_seq=seq)
    assert any(t.startswith("GP:PRESS a") for t in _texts(sim, mark))
    sim.wait_for_exit(timeout=10)


# ── Layout ──────────────────────────────────────────────────────────────────

# system_menu.c: a 264 px panel, 13 px rows (8 px font), in a game: title
# 16, the scope row, column heads, 12 buttons, the notice, a 2-line footer.
PANEL_X, PANEL_W, ROW_H = 28, 264, 13
IN_GAME_H = 239
PANEL_Y = (320 - IN_GAME_H) // 2
FIRST_ROW_Y = PANEL_Y + 2 + 16 + 2 * ROW_H     # after the scope row and heads
CELL_X = (PANEL_X + 70, PANEL_X + 166)
CELL_W = 90
BORDER = (82, 101, 148)                        # C_BORDER


def _white(region):
    return (region >= 240).all(axis=-1)


def _light(region):
    """C_DIM_SEL, an inherited binding on the selection bar (200 grey)."""
    return ((region >= 190) & (region <= 215)).all(axis=-1)


def _dim(region):
    """COLOR_GRAY text (128, 128, 128 give or take the RGB565 rounding)."""
    return ((region >= 120) & (region <= 140)).all(axis=-1)


def _ink(arr, button, slot):
    """The text colours in a key cell: "white", "light", "dim"."""
    y = FIRST_ROW_Y + button * ROW_H
    x = CELL_X[slot]
    cell = arr[y + 2:y + 10, x + 4:x + CELL_W]
    return {name for name, test in (("white", _white), ("light", _light),
                                    ("dim", _dim)) if test(cell).any()}


def test_layout_fits_and_dims_inherited_bindings(simulator, tmp_path):
    """The page fits the 320x320 screen inside a game (its tallest form),
    every button row has its name, and in This game the buttons the
    override does not list are drawn dimmed (lighter on the selection bar);
    All games shows them normally. The column heads are not the dimmed grey.
    The screenshots go to the test's tmp dir, which pytest keeps only when
    the test fails."""
    sim = simulator
    _stage_games(sim)
    _write(sim, f"data/{GAME_ID}/gamepad.json", {"a": ["Z"]})
    seq = _start(sim, GAME)
    _open_controls(sim)
    time.sleep(0.3)
    img = sim.screenshot_pil()
    img.save(tmp_path / "controls_this_game.png")
    arr = np.array(img.convert("RGB")).astype(int)

    bottom = PANEL_Y + IN_GAME_H - 1
    assert bottom < 320
    assert tuple(arr[PANEL_Y, PANEL_X]) == BORDER
    assert tuple(arr[bottom, PANEL_X + PANEL_W - 1]) == BORDER
    for b in range(12):
        y = FIRST_ROW_Y + b * ROW_H
        name = arr[y + 2:y + 10, PANEL_X + 10:PANEL_X + 60]
        assert _white(name).any(), f"no name on button row {b}"
    footer = arr[bottom - 24:bottom, PANEL_X + 4:PANEL_X + 200]
    assert _dim(footer).sum() > 200, "no footer hint"

    UP, A, B, X = 0, 4, 5, 6
    assert _ink(arr, A, 0) == {"white"}, "overridden A is dimmed"
    assert _ink(arr, A, 1) == {"white"}
    assert _ink(arr, B, 0) == {"dim"}, "inherited B is not dimmed"
    assert _ink(arr, X, 1) == {"dim"}
    assert _ink(arr, UP, 0) == {"light"}, "focused inherited Up"
    heads = arr[FIRST_ROW_Y - ROW_H + 2:FIRST_ROW_Y - ROW_H + 10,
                PANEL_X + 10:PANEL_X + 200]
    assert not _dim(heads).any(), "the column heads look inherited"
    assert (heads[..., 2] - heads[..., 0] > 60).any(), "no column heads"

    _keys(sim, ["up", "enter"])                # the scope row: All games
    time.sleep(0.3)
    img = sim.screenshot_pil()
    img.save(tmp_path / "controls_all_games.png")
    arr = np.array(img.convert("RGB")).astype(int)
    assert _ink(arr, B, 0) == {"white"}
    assert _ink(arr, A, 0) == {"white"}
    _keys(sim, ["esc", "esc", "esc"])
    _finish(sim, seq)
