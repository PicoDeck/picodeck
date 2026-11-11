"""apps/systest read from the gamepad: L/R and the D-pad's Left/Right step
the tabs (Up/Down after T), A runs the selected function, B leaves the Hello
demo and then the app. The Input tab's raw diagnostics are not driven
here."""
import time

import pytest

from pad_apps import (count_pixels, finish, launch, mark, press, rebind,
                      region, stage, tap, wait_region, wait_until)

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

TAB_BAR = (0, 21, 320, 42)      # ui.drawTabs below the 21-row header
HELLO_BOX = (0, 100, 320, 120)  # the Hello demo's cyan bouncing box


def _cyan(p):
    return p[0] < 80 and p[1] > 180 and p[2] > 180


def test_systest_tabs_and_lists_on_the_pad(simulator):
    sim = simulator
    seq = mark(sim)
    launch(sim, "systest", settle=0)
    sim.wait_for_log(r"^System test log message$", timeout=15, since_seq=seq)
    time.sleep(0.5)
    tests_tab = region(sim, TAB_BAR)
    press(sim, "r")                       # Tests -> Functions (R)
    functions_tab = wait_region(sim, TAB_BAR, lambda r: r != tests_tab)
    press(sim, "l")                       # back (L)
    wait_region(sim, TAB_BAR, lambda r: r == tests_tab)
    press(sim, "right")                   # Functions again (the D-pad)
    wait_region(sim, TAB_BAR, lambda r: r == functions_tab)
    assert count_pixels(sim, HELLO_BOX, _cyan) == 0
    press(sim, "a")                       # run "Hello World Demo"
    wait_until(lambda: count_pixels(sim, HELLO_BOX, _cyan) > 200)
    press(sim, "b")                       # B leaves the demo, not the app
    wait_until(lambda: count_pixels(sim, HELLO_BOX, _cyan) == 0)
    press(sim, "left")                    # Tests
    wait_region(sim, TAB_BAR, lambda r: r == tests_tab)
    tap(sim, "t")                         # T: Up/Down step the tabs instead
    press(sim, "down")
    wait_region(sim, TAB_BAR, lambda r: r == functions_tab)
    finish(sim)                           # B on the Functions list exits


def test_systest_tab_step_does_not_move_the_list(simulator):
    sim = simulator
    seq = mark(sim)
    launch(sim, "systest", settle=0)
    sim.wait_for_log(r"^System test log message$", timeout=15, since_seq=seq)
    time.sleep(0.5)
    tests_tab = region(sim, TAB_BAR)
    tap(sim, "t")                         # T works on the Tests tab: Up/Down step
    press(sim, "down")                    # Tests -> Functions
    functions_tab = wait_region(sim, TAB_BAR, lambda r: r != tests_tab)
    press(sim, "down")                    # leaves Functions
    wait_region(sim, TAB_BAR, lambda r: r != functions_tab)
    press(sim, "l")                       # back; L never moves a list
    wait_region(sim, TAB_BAR, lambda r: r == functions_tab)
    press(sim, "a")                       # still the first entry
    wait_until(lambda: count_pixels(sim, HELLO_BOX, _cyan) > 200)
    press(sim, "b")
    finish(sim)


def test_systest_r_bound_to_r_only_reruns(simulator):
    """R reruns the tests. Bound to the R button (a player's choice), typing
    R must still only rerun, not also step to the Functions tab."""
    sim = simulator
    stage(sim, "systest")
    rebind(sim, "systest", {"r": ["R"]})
    seq = mark(sim)
    launch(sim, "systest", settle=0)
    sim.wait_for_log(r"^System test log message$", timeout=15, since_seq=seq)
    time.sleep(0.5)
    tests_tab = region(sim, TAB_BAR)
    seq = mark(sim)
    tap(sim, "r")
    sim.wait_for_log(r"^System test log message$", timeout=5, since_seq=seq)
    time.sleep(0.5)
    assert region(sim, TAB_BAR) == tests_tab
    finish(sim)
