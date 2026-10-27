"""chargrid shader golden image: an aurora-class sky shader over a land
layer, and a torch shader on the land, at a fixed time. The shader maths is
libm-free, so the simulator's frame is exactly reproducible (and the device
matches it: test_chargrid_hw.py)."""
import shutil
from pathlib import Path

import numpy as np

from helpers import GOLDEN_DIR, app_rel_dir, compare_golden

GOLDEN = GOLDEN_DIR / "chargrid" / "shader_scene.png"
# Staged hidden (apps/.test/chargrid_shader_scene): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / "chargrid_shader_scene"


def test_chargrid_shader_scene_golden(simulator, update_baselines):
    shutil.copytree(FIXTURE, Path(simulator.sd_card_path) / app_rel_dir("chargrid_shader_scene"))
    simulator.clear_log()
    simulator.launch_app("chargrid_shader_scene")
    simulator.wait_for_log(r"CGSH:READY", timeout=30)
    simulator.wait_frames(3)
    img = np.array(simulator.screenshot_pil().convert("RGB"))
    compare_golden(img, GOLDEN, update_baselines)
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
