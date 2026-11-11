"""The small demo apps read the gamepad: B leaves each of them, fonttest's
D-pad cycles fonts and 3dtest's A toggles manual mode, its Start cycles the
fill mode and a held D-pad turns the model. Driven by the test pad
(tests/e2e/pad_apps.py)."""
import time

import pytest

from pad_apps import finish, launch, mark, press, region, wait_region

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

FONT_LINE = (0, 0, 320, 18)   # fonttest's "<i>/<n> <font> <w>x<h>" line
TITLE = (0, 0, 150, 20)       # the header's title: "3D PicoCalc - AUTO - WIRE"
MODEL = (60, 60, 260, 260)    # 3dtest's model, around the centre (160, 160)


def test_graphicstest_leaves_on_b(simulator):
    launch(simulator, "graphicstest")
    finish(simulator)


def test_scrolltest_leaves_on_b(simulator):
    sim = simulator
    seq = mark(sim)
    launch(sim, "scrolltest", settle=0)
    sim.wait_for_log(r"^SCROLLTEST:PHASE ", timeout=10, since_seq=seq)
    finish(sim)


def test_fonttest_dpad_cycles_fonts(simulator):
    sim = simulator
    launch(sim, "fonttest")
    first = region(sim, FONT_LINE)
    press(sim, "right")
    wait_region(sim, FONT_LINE, lambda r: r != first)
    press(sim, "left")
    wait_region(sim, FONT_LINE, lambda r: r == first)
    finish(sim)


def test_3dtest_a_toggles_manual_mode(simulator):
    sim = simulator
    launch(sim, "3dtest")
    auto = region(sim, TITLE)
    press(sim, "a")
    manual = wait_region(sim, TITLE, lambda r: r != auto)
    press(sim, "start")                  # fill mode: WIRE -> FILL
    wait_region(sim, TITLE, lambda r: r not in (auto, manual))
    press(sim, "a")
    wait_region(sim, TITLE, lambda r: r not in (auto, manual))  # AUTO - FILL
    finish(sim)


def test_3dtest_held_dpad_turns_the_model_in_manual_mode(simulator):
    sim = simulator
    launch(sim, "3dtest")
    press(sim, "a")                      # manual: the model holds still
    time.sleep(0.3)
    still = region(sim, MODEL)
    time.sleep(0.3)
    assert region(sim, MODEL) == still, "manual mode should not rotate by itself"
    sim.pad("right")                     # held: turns about Y every frame
    time.sleep(0.5)
    sim.pad("none")
    time.sleep(0.2)
    assert region(sim, MODEL) != still, "a held Right should turn the model"
    finish(sim)
