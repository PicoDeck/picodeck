"""apps/wikipedia read from the gamepad, offline. With WiFi pinned
"connecting" it waits and B leaves; with WiFi unavailable it shows its
error, which waits for a key, then the empty home screen, where A opens
nothing and B quits. Articles and search need the network and are not
driven here; the search field stays on the keyboard."""
import time

import pytest

from pad_apps import (count_pixels, finish, finish_key, launch, press,
                      rebind, set_wifi, stage, tap, wait_until)

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

ERROR_BAND = (0, 150, 320, 175)   # draw_error's red message at y = 160
SEARCH_LINE = (0, 60, 320, 75)    # draw_search_input's "<query>_" line


def _red(p):
    return p[0] > 180 and p[1] < 80 and p[2] < 80


def _lit(p):
    return p != (0, 0, 0)


def test_wikipedia_waiting_for_wifi_leaves_on_b(simulator):
    sim = simulator
    set_wifi(sim, "connecting")
    launch(sim, "wikipedia")
    finish(sim)


def test_wikipedia_error_then_empty_home_on_the_pad(simulator):
    sim = simulator
    set_wifi(sim, "not_available")
    launch(sim, "wikipedia", settle=1.0)
    assert count_pixels(sim, ERROR_BAND, _red) > 20, "the error should stay up"
    press(sim, "a")                     # any button returns home
    wait_until(lambda: count_pixels(sim, ERROR_BAND, _red) == 0)
    press(sim, "a")                     # A on the empty list opens nothing
    finish(sim)                         # B on the home screen quits


def test_wikipedia_search_field_stays_on_the_keyboard(simulator):
    """In the search field the pad's B is not Esc: typed text, Backspace and
    Esc are the keyboard's. Esc there goes home, and Esc on home quits."""
    sim = simulator
    set_wifi(sim, "not_available")
    launch(sim, "wikipedia", settle=1.0)
    tap(sim, "x")                       # leave the error screen
    tap(sim, "s")                       # S: the search field
    empty = count_pixels(sim, SEARCH_LINE, _lit)
    tap(sim, "c")
    tap(sim, "a")
    typed = count_pixels(sim, SEARCH_LINE, _lit)
    assert typed > empty, "typed letters should show in the field"
    press(sim, "b")                     # the pad's B does not cancel typing
    time.sleep(0.3)
    assert count_pixels(sim, SEARCH_LINE, _lit) == typed
    tap(sim, "esc")                     # Esc cancels: home
    finish_key(sim, "esc")              # Esc on home quits


def test_wikipedia_s_bound_to_a_still_opens_search(simulator):
    """S opens the search field. Bound to A (a player's choice), typing S
    must still do only that: the home screen reads its letters first."""
    sim = simulator
    stage(sim, "wikipedia")
    rebind(sim, "wikipedia", {"a": ["S"]})
    set_wifi(sim, "not_available")
    launch(sim, "wikipedia", settle=1.0)
    tap(sim, "x")                       # leave the error screen
    tap(sim, "s")                       # S: the search field (and A)
    empty = count_pixels(sim, SEARCH_LINE, _lit)
    tap(sim, "c")
    assert count_pixels(sim, SEARCH_LINE, _lit) > empty, "S should open the search field"
    tap(sim, "esc")
    finish(sim)
