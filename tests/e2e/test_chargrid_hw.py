"""chargrid on the device: full-window draw cost at six configs, and the
golden scene rendered on the device matches the simulator's (an integer-only
renderer; only the byte order differs, so a tight tolerance)."""
import io
import json
import time

import numpy as np
import pytest
from PIL import Image

from helpers import GOLDEN_DIR
from hw_target import HW_APPS_DIR

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(600)]

BENCH, BENCH_ID = "chargrid_bench", "com.test.chargrid_bench"
SCENE_DIR = GOLDEN_DIR / "chargrid_scene"
# P0 (specs/2026-10-09-chargrid-p0-findings.md): the register-only spike blit
# took 3.05 ms at 6x8 and 5.16 ms at 80x80@4x4. Regression guards on the
# median; the P1 target for 80x80@4x4 (<= 4 ms, the spec's damage-tracking
# threshold) is reported, not asserted.
MAX_US = {"53x40@6x8": 3500, "80x80@4x4": 5500}
TARGET_US_80X80 = 4000


def test_chargrid_draw_cost(target):
    target.push_app(HW_APPS_DIR / BENCH, BENCH)
    target.delete_file(f"/data/{BENCH_ID}/bench.json")
    target.delete_file(f"/data/{BENCH_ID}/test_results.json")
    target.launch_app(BENCH)
    target.wait_for_results(BENCH_ID, timeout=300)
    rows = {r["name"]: r for r in json.loads(target.read_file(f"/data/{BENCH_ID}/bench.json"))}
    for r in rows.values():
        print(f"chargrid {r['name']:>11}: median {r['us_median']:5d} us, "
              f"max {r['us_max']:5d} us, {r['cells']} cells, xip {r['xip']}%")
    met = rows["80x80@4x4"]["us_median"] <= TARGET_US_80X80
    print(f"80x80@4x4 P1 target {TARGET_US_80X80} us: {'met' if met else 'MISSED'}")
    for name, limit in MAX_US.items():
        assert rows[name]["us_median"] <= limit, rows[name]


def test_scene_matches_simulator_golden(target):
    target.push_app(SCENE_DIR, "chargrid_scene")
    target.launch_app("chargrid_scene")
    time.sleep(4)
    got = np.array(Image.open(io.BytesIO(target.screenshot())).convert("RGB")).astype(int)
    want = np.array(Image.open(GOLDEN_DIR / "chargrid" / "scene.png").convert("RGB")).astype(int)
    target.exit_app()
    target.wait_for_exit(timeout=15)
    differs = (np.abs(got - want).max(axis=2) > 0).mean()
    assert differs < 0.01, f"{differs:.2%} of pixels differ from the simulator golden"
