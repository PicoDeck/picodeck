"""apps/picoforge's project browser read from the gamepad: the D-pad moves,
X deletes the selected project after a confirm (B declines, A accepts),
B quits; with no projects A opens nothing. The editors stay on the
keyboard."""
import json
import time
from pathlib import Path

import pytest

from pad_apps import (answer_confirm, count_pixels, finish, finish_key,
                      launch, press, rebind, stage, tap, wait_until)

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)


def _project(sim, name):
    d = Path(sim.sd_card_path) / "data" / "picoforge" / "projects" / name
    d.mkdir(parents=True)
    (d / "project.json").write_text(json.dumps({"name": name}))
    (d / "main.lua").write_text("-- " + name + "\n")
    return d


def test_picoforge_browser_on_the_pad(simulator):
    sim = simulator
    alpha, beta = _project(sim, "alpha"), _project(sim, "beta")
    launch(sim, "picoforge", settle=1.5)
    press(sim, "down")              # alpha -> beta
    press(sim, "x")                 # delete beta...
    answer_confirm(sim, "b")        # ...no
    time.sleep(0.5)
    assert (beta / "project.json").exists()
    press(sim, "x")                 # delete beta...
    answer_confirm(sim, "a")        # ...yes
    wait_until(lambda: not (beta / "project.json").exists())
    assert (alpha / "project.json").exists()
    finish(sim)


def test_picoforge_empty_browser_quits_on_b(simulator):
    sim = simulator
    launch(sim, "picoforge", settle=1.5)
    press(sim, "a")                 # no project to open: nothing happens
    finish(sim)


def test_picoforge_browser_keys_still_work(simulator):
    """D still deletes (Enter answers the confirm) and Esc still quits."""
    sim = simulator
    alpha = _project(sim, "alpha")
    launch(sim, "picoforge", settle=1.5)
    tap(sim, "d")
    time.sleep(0.7)
    tap(sim, "enter")
    wait_until(lambda: not (alpha / "project.json").exists())
    finish_key(sim, "esc")


def test_picoforge_d_bound_to_a_only_deletes(simulator):
    """D deletes. Bound to A (a player's choice), typing D must still only
    delete, not also open the project: the browser reads its letters
    first."""
    sim = simulator
    alpha = _project(sim, "alpha")
    stage(sim, "picoforge")
    rebind(sim, "picoforge", {"a": ["D"]})
    launch(sim, "picoforge", settle=1.5)
    tap(sim, "d")                   # delete alpha... (and A)
    answer_confirm(sim, "a")        # ...yes, from the pad
    wait_until(lambda: not (alpha / "project.json").exists())
    finish(sim)                     # still the browser: B quits


def _highlight(sim):
    """Pixels of the selected row's highlight (0x4A69) on the screen."""
    return count_pixels(
        sim, (0, 22, 320, 290),
        lambda p: abs(p[0] - 74) < 8 and abs(p[1] - 77) < 8
        and abs(p[2] - 74) < 8)


def test_picoforge_wrapping_scrolls_the_selection_into_view(simulator):
    """With more projects than rows, Up from the first wraps to the last and
    Down from the last wraps to the first; the highlight stays on screen."""
    sim = simulator
    for i in range(20):             # 18 rows fit
        _project(sim, "p%02d" % i)
    launch(sim, "picoforge", settle=1.5)
    assert _highlight(sim) > 500
    press(sim, "up")                # wraps to the last
    time.sleep(0.3)
    assert _highlight(sim) > 500
    press(sim, "down")              # wraps to the first
    time.sleep(0.3)
    assert _highlight(sim) > 500
    finish(sim)
