"""apps/hello, the app template, read from the gamepad.

A pauses the animation, B (or Esc) exits, and the footer names A's key with
gamepad.getLabel(), so it follows a rebinding. Driven by the test pad, a
controller's path (tests/e2e/pad_apps.py).
"""
import time

import pytest

from pad_apps import finish, finish_key, launch, press, rebind, region, tap

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

BOX_ROW = (0, 100, 320, 120)       # the bouncing box (rows 100-119)
FOOTER_LEFT = (0, 303, 140, 320)   # "<A's key>: pause  Esc: exit"


def _box_samples(sim, n=4, gap=0.15):
    """n pictures of the box's row, gap seconds apart."""
    shots = []
    for _ in range(n):
        shots.append(region(sim, BOX_ROW))
        time.sleep(gap)
    return shots


def test_a_pauses_and_b_exits(simulator):
    sim = simulator
    launch(sim, "hello")
    assert len(set(_box_samples(sim))) > 1, "the box should move"
    press(sim, "a")
    assert len(set(_box_samples(sim))) == 1, "A should pause the box"
    press(sim, "a")
    assert len(set(_box_samples(sim))) > 1, "A again should resume it"
    finish(sim)


def test_esc_still_exits(simulator):
    launch(simulator, "hello")
    finish_key(simulator, "esc")


def test_footer_names_a_by_its_key_and_follows_a_rebind(simulator):
    sim = simulator
    launch(sim, "hello")
    default_footer = region(sim, FOOTER_LEFT)
    finish(sim)
    rebind(sim, "hello", {"a": ["W"]})
    launch(sim, "hello")
    assert region(sim, FOOTER_LEFT) != default_footer
    tap(sim, "w")                      # W is A now: it pauses the box
    assert len(set(_box_samples(sim))) == 1
    finish(sim)
