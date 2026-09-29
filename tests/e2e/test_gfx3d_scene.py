"""gfx3d golden-image scene: sky bands, a road over a crest, a ship sorted as
one object, fog, background mountains and a sprite in a 320x240 viewport."""
import re

import numpy as np

from helpers import GOLDEN_DIR, compare_golden

GOLDEN = GOLDEN_DIR / "gfx3d" / "scene.png"


def _rgb(c565):
    r, g, b = (c565 >> 11) & 31, (c565 >> 5) & 63, c565 & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def test_gfx3d_scene_golden(simulator, update_baselines):
    simulator.clear_log()
    simulator.launch_app("gfx3d_scene")
    line = simulator.wait_for_log(r"G3S:SHIP -?\d+ -?\d+", timeout=30)
    sx, sy = map(int, re.search(r"G3S:SHIP (-?\d+) (-?\d+)", line).groups())
    simulator.wait_frames(3)
    img = np.array(simulator.screenshot_pil().convert("RGB"))
    compare_golden(img, GOLDEN, update_baselines)

    def at(x, y):
        return tuple(int(v) for v in img[y, x])

    assert at(5, 5) == _rgb(_to565(24, 40, 120)), "top-left should be the top sky band"
    assert at(160, 300) == (0, 0, 0), "HUD strip below the viewport is black"
    assert 0 <= sx < 320 and 0 <= sy < 240, (sx, sy)
    road = {_rgb(_to565(80, 80, 96)), _rgb(_to565(112, 112, 128))}
    sky = {_rgb(_to565(24, 40, 120)), _rgb(_to565(64, 96, 200))}
    assert at(sx, sy) not in road | sky, f"ship missing at {(sx, sy)}: {at(sx, sy)}"
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
