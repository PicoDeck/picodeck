"""TIC-80 reads the gamepad (issue #25).

TIC-80's PicoDeck glue is local-only (apps/tic-80, gitignored): the tests
skip when its built main.elf is missing (PICODECK_TIC80_DIR overrides the
directory; skip_allowlist.txt). A tiny .tic cart latches a coloured square for
every TIC-80 button it sees, so a key's effect is read off the screen. The
studio feeds the keyboard separately: the console's Backspace must still edit.
"""

import io
import json
import os
import shutil
import time
from pathlib import Path

import pytest
from PIL import Image, ImageChops

TIC = Path(os.environ.get("PICODECK_TIC80_DIR",
                          Path(__file__).resolve().parents[2] / "apps" / "tic-80"))

pytestmark = [
    pytest.mark.skipif(
        not (TIC / "main.elf").exists() or not (TIC / "app.json").exists(),
        reason=f"{TIC}/main.elf not built: make in apps/tic-80 or set "
               "PICODECK_TIC80_DIR"),
    pytest.mark.sd(fixtures=[], reserve=1),
    pytest.mark.timeout(180),
]

CART = b"""-- title:  pad
p={}
function TIC()
 cls(0)
 for i=0,7 do
  if btn(i) then p[i]=true end
  if p[i] then rect(i*20+4,20,16,16,i+8) end
 end
end
"""


def _stage(sim, rebind=None):
    d = Path(sim.sd_card_path) / "apps" / "tic-80"
    d.mkdir(parents=True, exist_ok=True)
    for f in ("main.elf", "app.json"):
        shutil.copy(TIC / f, d / f)
    # .tic cart: one CODE chunk (type 5, bank 0): type byte, u16 size, 0.
    (d / "pad.tic").write_bytes(
        bytes([5]) + len(CART).to_bytes(2, "little") + b"\0" + CART)
    if rebind:
        p = Path(sim.sd_card_path) / "data" / "com.nesbox.tic80" / "gamepad.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(rebind))


def _tap(sim, key, wait=0.5):
    r = sim.keypress(key)
    sim.wait_input_consumed(r["input_seq"], timeout=30)
    time.sleep(wait)


def _type(sim, text):
    # Slower than 0.35 s a char and the console drops some.
    for c in text:
        _tap(sim, c, 0.35)


def _shot(sim):
    return Image.open(io.BytesIO(sim.screenshot())).convert("RGB")


def _diff_box(a, b):
    """(width, height) of the box around the pixels where two shots differ."""
    box = ImageChops.difference(a, b).getbbox()
    return (0, 0) if box is None else (box[2] - box[0], box[3] - box[1])


def _squares(sim):
    """Indices (0-7) of the buttons whose square is painted: the cart row at
    y=20..36, shown at 1.25x with the picture offset by (11, 76)."""
    img = _shot(sim)
    found = set()
    for i in range(8):
        x = int((i * 20 + 12) * 1.25) + 11
        y = 76 + int(28 * 1.25)
        if img.getpixel((x, y)) != img.getpixel((x, y + 80)):
            found.add(i)
    return found


def _wait_cart(sim, timeout=60):
    """The cart is running once the screen is down to its black clear (and
    the odd cursor or square pixel), not the console's text."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        colours = _shot(sim).getcolors(maxcolors=64)
        if colours and len(colours) <= 4:
            return
        time.sleep(0.5)
    raise AssertionError("the cart never started")


def _start(sim, rebind=None, load=True):
    _stage(sim, rebind)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("tic-80")
    sim.wait_for_log(r"Studio created successfully", timeout=120, since_seq=seq)
    time.sleep(6)  # the console's intro
    if load:
        _type(sim, "load pad.tic")
        _tap(sim, "enter", 1.5)
        _type(sim, "run")
        _tap(sim, "enter", 1.0)
        _wait_cart(sim)


def test_backspace_still_edits_the_console(simulator):
    """The gamepad's Y is Backspace, and the keyboard feed still carries it
    to the editor. The console cursor blinks in the cell after the text, so a
    shot differs from itself by up to one character cell (~8 px wide at 1.25x);
    a character is a second cell on top of that."""
    CELL = 12
    sim = simulator
    _start(sim, load=False)
    _type(sim, "xyz")
    typed = _shot(sim)
    _tap(sim, "backspace")
    erased = _shot(sim)
    assert _diff_box(typed, erased)[0] > CELL        # the z is gone
    _type(sim, "z")
    again = _shot(sim)
    assert _diff_box(typed, again)[0] <= CELL        # back to xyz, cursor aside


def test_default_buttons(simulator):
    """Each default key latches exactly its own TIC-80 button (up down left
    right a b x y = 0-7); F1, F2 and F3 (Start, L, R) latch none."""
    sim = simulator
    _start(sim)
    assert _squares(sim) == set()
    want = set()
    for key, idx in (("up", 0), ("down", 1), ("left", 2), ("right", 3),
                     ("f4", 4), ("f5", 5), ("del", 6), ("backspace", 7)):
        _tap(sim, key)
        want.add(idx)
        assert _squares(sim) == want, (key, _squares(sim))
    for key in ("f1", "f2", "f3"):
        _tap(sim, key)
        assert _squares(sim) == want, (key, _squares(sim))


def test_follows_a_rebinding(simulator):
    """A moves to F3 and X to F4: F4 latches X (6), not A (4); F3 latches A."""
    sim = simulator
    _start(sim, rebind={"a": ["F3"], "x": ["F4"]})
    _tap(sim, "f4")
    assert _squares(sim) == {6}, _squares(sim)
    _tap(sim, "f3")
    assert _squares(sim) == {4, 6}, _squares(sim)
