"""DOOM's typed keys (issue #64): the number keys select weapons, and text
reaches its cheats and save names, each character once.

A typed character lasts one keyboard poll (getChar). Doom reads input only when
it builds a tic and samples a weapon key only while that key is down, so the
port has to catch every character and hold it down across a build. The tests
stage PicoDeck/doom's build as test_doom_gamepad.py does (PICODECK_DOOM_DIR;
skipped without it) and read the effect, not the key's acceptance: the status
bar's ammo number for weapons and cheats, the save file's header for a name.
"""

import time
from pathlib import Path

import pytest

import test_doom_gamepad as D


def _doom_has_the_fix():
    """Whether the checkout has #64's fix. These tests fail against a Doom from
    before it (v1.0.4 and older), which says nothing about this tree."""
    source = D.DOOM / "dg_picodeck.c"
    return source.exists() and "poll_input" in source.read_text(errors="replace")


pytestmark = [
    pytest.mark.skipif(
        not all((D.DOOM / f).exists() for f in D.FILES),
        reason=f"{D.DOOM} lacks main.elf/doom1.wad: build PicoDeck/doom or set "
               "PICODECK_DOOM_DIR"),
    pytest.mark.skipif(
        not _doom_has_the_fix(),
        reason=f"{D.DOOM} predates PicoDeck/doom's #64 fix (no poll_input in "
               "dg_picodeck.c): update and rebuild it, or set PICODECK_DOOM_DIR"),
    pytest.mark.sd(fixtures=[], reserve=1),
    pytest.mark.timeout(180),
]


def _wait_ammo(sim, ok, what, timeout=15):
    """The ammo number once ok(number) holds: a weapon lowers and rises over
    several tics, and the emulated Doom is slow."""
    deadline = time.time() + timeout
    while True:
        a = D._ammo(sim)
        if ok(a):
            return a
        assert time.time() < deadline, what
        time.sleep(0.2)


def _type(sim, text):
    """Inject the characters with no gap and wait until the OS has read them
    all: one a poll, so at most one of them can land on the poll before a tic."""
    seq = 0
    for ch in text:
        seq = sim.keypress(ch)["input_seq"]
    sim.wait_input_consumed(seq, timeout=30)


def _menu_to(sim, *keys):
    """Open Doom's menu in a running level (it pauses the game) and press the
    keys, one screen at a time."""
    D._tap(sim, "f1")
    D._settle(sim)
    for key in keys:
        D._tap(sim, key, wait=0.5)
        D._settle(sim)


def test_a_digit_selects_its_weapon(simulator):
    sim = simulator
    D._start_game(sim)
    pistol = D._ammo(sim)
    D._tap(sim, "1")
    fist = _wait_ammo(sim, lambda a: a != pistol, "1 did not select the fist")
    D._tap(sim, "2")
    _wait_ammo(sim, lambda a: a == pistol, "2 did not select the pistol")
    D._tap(sim, "1")
    _wait_ammo(sim, lambda a: a == fist, "1 did not select the fist again")


def test_a_digit_is_let_go_and_x_y_still_cycle(simulator):
    """A digit still held would ask for its weapon at every tic and pull the
    weapon straight back after X or Y."""
    sim = simulator
    D._start_game(sim)
    pistol = D._ammo(sim)
    D._tap(sim, "1")
    fist = _wait_ammo(sim, lambda a: a != pistol, "1 did not select the fist")
    D._tap(sim, "del")                   # X = next weapon: fist -> pistol
    _wait_ammo(sim, lambda a: a == pistol, "X did not leave the fist")
    time.sleep(2)
    assert D._ammo(sim) == pistol, "the digit pulled the fist back: stuck key"
    D._tap(sim, "backspace")             # Y = previous weapon: pistol -> fist
    _wait_ammo(sim, lambda a: a == fist, "Y did not cycle back")


def test_a_cheat_typed_in_one_burst_arrives_whole(simulator):
    """Five characters on five consecutive polls, far faster than Doom's tics:
    a cheat fails on one lost character. idkfa raises the pistol's 50 to 200,
    and only it gives the shotgun, whose 50 shells read like the first 50."""
    sim = simulator
    D._start_game(sim)
    fifty = D._ammo(sim)
    _type(sim, "idkfa")
    two_hundred = _wait_ammo(sim, lambda a: a != fifty, "idkfa did not arrive")
    D._tap(sim, "3")
    _wait_ammo(sim, lambda a: a == fifty, "3 did not select the shotgun")
    D._tap(sim, "2")
    _wait_ammo(sim, lambda a: a == two_hundred, "2 did not select the pistol")


def _wait_row_still(sim, timeout=20):
    """Wait until the first save slot's row stops changing: typed characters
    reach Doom a tic or two apart, and a button press does not wait for them."""
    row = lambda: D._img(sim).crop((60, 110, 320, 125)).tobytes()
    deadline = time.time() + timeout
    prev = row()
    while time.time() < deadline:
        time.sleep(1)
        cur = row()
        if cur == prev:
            return
        prev = cur


def test_a_save_name_gets_each_key_once(simulator):
    """A name typed in one burst, with a doubled character in it, reaches the
    save file's header exactly as typed (Doom upper-cases it): nothing lost,
    nothing fired twice. The port's mkdir and rename are stubs, so the test makes
    the directory (without it, confirming a name exits Doom) and the file stays
    temp.dsg."""
    sim = simulator
    saves = Path(sim.sd_card_path) / "apps" / "doom" / ".savegame"
    saves.mkdir(parents=True)
    D._start_game(sim)
    _menu_to(sim, "down", "down", "down", "enter", "enter")  # Save Game, slot 1
    _type(sim, "a1b22c")
    _wait_row_still(sim)
    D._tap(sim, "enter")
    save = saves / "temp.dsg"
    deadline = time.time() + 30
    while not (save.exists() and save.stat().st_size >= 24):
        assert time.time() < deadline, f"no save header in {save}"
        time.sleep(0.2)
    assert save.read_bytes()[:24].rstrip(b"\0") == b"A1B22C"


def _message(sim):
    """Where Doom's menu prints a message, over the paused level: a band below
    D._view's, which is why that one does not see it."""
    return D._img(sim).crop((20, 145, 300, 175))


def test_enter_is_one_key_not_two(simulator):
    """On the device Enter makes a press and the char '\\n'. The press reaches
    Doom through the key maps, so the char must not reach it as a second key:
    it would answer the "press a key" message that the same Enter just raised.
    Episode 2 of the shareware game raises one."""
    sim = simulator
    D._start_game(sim)
    D._tap(sim, "f1")
    D._settle(sim)
    before = _message(sim)
    for key in ("enter", "down", "enter"):
        D._tap(sim, key, wait=0.5)
        D._settle(sim)
    shown = _message(sim)
    assert D._moved(before, shown), "the message did not come up"
    D._tap(sim, "\n")
    D._settle(sim)
    assert not D._moved(shown, _message(sim)), "'\\n' answered the message"
    D._tap(sim, "x")                     # a printable key does answer it
    D._settle(sim)
    assert D._moved(shown, _message(sim)), "the message took no key"
