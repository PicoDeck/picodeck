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
# took 3.05 ms at 6x8 and 5.16 ms at 80x80@4x4. P1 measured 64x40@5x8 at
# 9.7 ms on the per-pixel path; P2 puts odd widths on the pair path (~3.7 ms
# expected). Regression guards on the median; the P1 target for 80x80@4x4
# (<= 4 ms, the spec's damage-tracking threshold) is reported, not asserted.
MAX_US = {"53x40@6x8": 3500, "80x80@4x4": 5500, "64x40@5x8": 4500}
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


SHADER_BENCH, SHADER_BENCH_ID = "chargrid_shader_bench", "com.test.chargrid_shader_bench"
SHADER_SCENE_DIR = GOLDEN_DIR / "chargrid_shader_scene"
# P0's spike ran the aurora-class program at 80x40 in 44-51 ms and the torch
# at 53x40 in 2.2 ms. Regression guards on the median; the P2 target for the
# aurora (<= 25 ms) is reported, not asserted.
SHADER_MAX_US = {"aurora 80x40": 40000, "torch 53x40": 3000}
SHADER_TARGET_US = 25000


def test_chargrid_shader_cost(target):
    target.push_app(HW_APPS_DIR / SHADER_BENCH, SHADER_BENCH)
    target.delete_file(f"/data/{SHADER_BENCH_ID}/shader_bench.json")
    target.delete_file(f"/data/{SHADER_BENCH_ID}/test_results.json")
    target.launch_app(SHADER_BENCH)
    target.wait_for_results(SHADER_BENCH_ID, timeout=300)
    rows = {r["name"]: r for r in json.loads(target.read_file(f"/data/{SHADER_BENCH_ID}/shader_bench.json"))}
    for name, r in rows.items():
        if "ns" in r:
            print(f"chargrid shader {name:>16}: {r['ns']:5d} ns")
        else:
            print(f"chargrid shader {name:>16}: median {r['us_median']:6d} us, max {r['us_max']:6d} us, "
                  f"{r['cells']} cells, {r['instructions']} instructions, xip {r['xip']}%")
    met = rows["aurora 80x40"]["us_median"] <= SHADER_TARGET_US
    print(f"aurora 80x40 P2 target {SHADER_TARGET_US} us: {'met' if met else 'MISSED'}")
    for name, limit in SHADER_MAX_US.items():
        assert rows[name]["us_median"] <= limit, rows[name]


def test_shader_scene_matches_simulator_golden(target):
    target.push_app(SHADER_SCENE_DIR, "chargrid_shader_scene")
    target.launch_app("chargrid_shader_scene")
    time.sleep(4)
    got = np.array(Image.open(io.BytesIO(target.screenshot())).convert("RGB")).astype(int)
    want = np.array(Image.open(GOLDEN_DIR / "chargrid" / "shader_scene.png").convert("RGB")).astype(int)
    target.exit_app()
    target.wait_for_exit(timeout=15)
    differs = (np.abs(got - want).max(axis=2) > 0).mean()
    assert differs < 0.01, f"{differs:.2%} of pixels differ from the simulator golden"
