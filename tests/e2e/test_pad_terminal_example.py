"""apps/terminal_example read from the gamepad: A (or any key) steps through
the demos, the D-pad picks the font, a held Up scrolls the scrollback demo
back, B leaves. Followed through the active terminal's text
(get_terminal_buffer, the live grid) and, for the scrollback view, which
that text does not show, the screen."""
import time

import pytest

from pad_apps import finish, launch, press, region, wait_region, wait_until

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

# Text each "press any key" screen shows, in order. The cursor demo homes the
# cursor and its prompt overwrites its title, so it is known by a later line.
SCREENS = ["=== 16 Color Palette ===", "=== 24-bit RGB Colors ===",
           "Cursor at (10,10)", "=== Box Drawing ===", "=== Font Demo ==="]
TOP_ROWS = (0, 30, 320, 150)  # the terminal's upper rows, away from the cursor


def _text(sim):
    try:
        return "\n".join(sim.get_terminal_buffer()["lines"])
    except RuntimeError:          # no terminal yet
        return ""


def _shows(sim, words):
    wait_until(lambda: words in _text(sim), timeout=10)


def test_terminal_example_on_the_pad(simulator):
    sim = simulator
    launch(sim, "terminal_example", settle=0)
    _shows(sim, "=== PicoDeck Terminal Demo ===")
    for words in SCREENS:
        press(sim, "a")
        _shows(sim, words)
    press(sim, "right")
    _shows(sim, "Font: scientifica_bold")
    press(sim, "a")
    _shows(sim, "=== Animation Demo ===")
    press(sim, "a")
    # 45 lines in a 26-row grid: the title is in the scrollback already.
    _shows(sim, "Use scrollback to view previous lines!")
    time.sleep(0.3)
    live = region(sim, TOP_ROWS)
    sim.pad("up")                 # held: one line back per repeat
    time.sleep(1.0)
    sim.pad("none")
    wait_region(sim, TOP_ROWS, lambda r: r != live)
    finish(sim)


def test_terminal_example_keys_still_continue(simulator):
    """A typed letter and Enter still continue past "Press any key"."""
    sim = simulator
    launch(sim, "terminal_example", settle=0)
    _shows(sim, "=== PicoDeck Terminal Demo ===")
    sim.keypress("x")
    _shows(sim, SCREENS[0])
    sim.keypress("enter")
    _shows(sim, SCREENS[1])
    sim.exit_app()
    sim.wait_for_exit(timeout=10)


def test_terminal_example_held_button_advances_one_screen(simulator):
    """A held A moves past one "Press any key" and waits for its release.
    (The harness cannot repeat a held keyboard key, so the character-repeat
    case on the device is not covered here.)"""
    sim = simulator
    launch(sim, "terminal_example", settle=0)
    _shows(sim, "=== PicoDeck Terminal Demo ===")
    sim.pad("a", 1200)
    _shows(sim, SCREENS[0])
    time.sleep(0.8)
    assert SCREENS[1] not in _text(sim)
    time.sleep(0.8)
    press(sim, "a")
    _shows(sim, SCREENS[1])
    sim.exit_app()
    sim.wait_for_exit(timeout=10)
