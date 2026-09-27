"""gfx3d on the device: frame cost at 200-1600 triangles, and the golden
scene rendered on hardware matches the simulator's (shared rasteriser;
edges may differ by float rounding, so a small tolerance)."""
import io
import json
import time

import numpy as np
import pytest
from PIL import Image

from helpers import GOLDEN_DIR
from hw_target import HW_APPS_DIR

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(900)]

BENCH, BENCH_ID = "gfx3d_bench", "com.test.gfx3d_bench"
SCENE_DIR = GOLDEN_DIR.parent / "apps" / "gfx3d_scene"
# P0: the same 804-triangle strip took 41 800 us of CPU in pure Lua with the
# fastest runtime. gfx3d measured ~17.4 ms avg on this branch (2.4x); this is
# a regression guard on the median (robust to harness-polling spikes), not
# the design target — see the gfx3d Perf/Follow-ups note in src/os/CLAUDE.md
# to optimise geometry/raster.
CPU_US_MEDIAN_MAX_800 = 20000


def test_gfx3d_frame_cost(target):
    target.push_app(HW_APPS_DIR / BENCH, BENCH)
    target.delete_file(f"/data/{BENCH_ID}/bench.json")
    target.delete_file(f"/data/{BENCH_ID}/test_results.json")
    target.launch_app(BENCH)
    target.wait_for_results(BENCH_ID, timeout=600)
    rows = json.loads(target.read_file(f"/data/{BENCH_ID}/bench.json"))
    for r in rows:
        print(f"gfx3d {r['tris']:5d} tris: cpu {r['cpu_us_avg']:6d} us avg "
              f"(median {r['cpu_us_median']}, max {r['cpu_us_max']}) geom "
              f"{r['geom_us_avg']} raster {r['raster_us_avg']} drawn "
              f"{r['drawn_avg']} culled {r['culled_avg']}")
    r800 = next(r for r in rows if 800 <= r["tris"] < 1000)
    assert r800["cpu_us_median"] < CPU_US_MEDIAN_MAX_800, r800


def test_scene_matches_simulator_golden(target):
    target.push_app(SCENE_DIR, "gfx3d_scene")
    target.launch_app("gfx3d_scene")
    time.sleep(4)
    got = np.array(Image.open(io.BytesIO(target.screenshot())).convert("RGB")).astype(int)
    want = np.array(Image.open(GOLDEN_DIR / "gfx3d" / "scene.png").convert("RGB")).astype(int)
    target.exit_app()
    target.wait_for_exit(timeout=15)
    differs = (np.abs(got - want).max(axis=2) > 24).mean()
    assert differs < 0.01, f"{differs:.2%} of pixels differ from the simulator golden"
