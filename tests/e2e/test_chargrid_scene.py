"""chargrid golden image: a 53x40 RPG-style screen (text, boxes, every
generated shape, palette swatches, a dot ramp, stamped art). The grid redraws
every cell each frame, so a system-menu round trip must leave the screen
identical to the golden."""
import shutil
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import GOLDEN_DIR, app_rel_dir, compare_golden

GOLDEN = GOLDEN_DIR / "chargrid" / "scene.png"
# Staged hidden (apps/.test/chargrid_scene): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / "chargrid_scene"


def _shot(sim):
    return np.array(sim.screenshot_pil().convert("RGB"))


def _wait(sim, pred, what, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        img = _shot(sim)
        if pred(img):
            return img
        time.sleep(0.1)
    pytest.fail(what)


def test_chargrid_scene_golden(simulator, update_baselines):
    shutil.copytree(FIXTURE, Path(simulator.sd_card_path) / app_rel_dir("chargrid_scene"))
    simulator.clear_log()
    simulator.launch_app("chargrid_scene")
    simulator.wait_for_log(r"CGS:READY", timeout=30)
    simulator.wait_frames(3)
    img = _shot(simulator)
    compare_golden(img, GOLDEN, update_baselines)

    simulator.keypress("menu")
    _wait(simulator, lambda s: not np.array_equal(s, img), "the system menu did not appear")
    simulator.keypress("esc")  # read by the menu: closes it
    _wait(simulator, lambda s: np.array_equal(s, img),
          "the screen after the menu closed differs from the scene")

    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
