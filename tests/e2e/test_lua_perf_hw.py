"""Lua apps' hot path on the device: the interpreter, Lua calling C, and a
chargrid frame (hw_apps/lua_bench), whose speed hangs on where the code sits
in the XIP cache. src/hot_text.ld pins that code; this guards it.

Where Lua's data sits in PSRAM, which shares that cache, moves it as much:
on one build, after test_chargrid_hw.py's apps had run, the fire took
31-35 ms and the C calls 45-66 ms, against 23.8 and 38.6 from a fresh boot.
So the test reboots first, and judges each workload by the better of two
launches: the first launch after a boot ran the fire at 30.8 ms, the second
at 23.8, every time."""
import json

import pytest

from hw_target import HW_APPS_DIR

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(600)]

BENCH, BENCH_ID = "lua_bench", "com.test.lua_bench"
# Medians in us at 300 MHz (2026-10-25, the same commit linked four ways):
#                       interpreter  C calls  fire 64x40
#   .text (develop)           37.7     56.3        23.7
#   .text + 452 B of VM       36.1     39.9        31.0  (ascii-scene fire 43 -> 25 fps)
#   that + a 7.6 KB pad       37.7     56.1        28.1
#   .hot_text                 35.7     38.6        23.8
# The guards sit ~20% above .hot_text: the second build fails on fire, the
# first on C calls.
MAX_US = {"interpreter": 43000, "C calls": 46000, "fire 64x40": 28500}
LAUNCHES = 2  # after a reboot; each workload keeps its better median


def run_bench(target):
    target.delete_file(f"/data/{BENCH_ID}/lua_bench.json")
    target.delete_file(f"/data/{BENCH_ID}/test_results.json")
    target.launch_app(BENCH)
    target.wait_for_results(BENCH_ID, timeout=300)
    return {r["name"]: r for r in json.loads(target.read_file(f"/data/{BENCH_ID}/lua_bench.json"))}


def test_lua_hot_path_cost(target):
    target.push_app(HW_APPS_DIR / BENCH, BENCH)
    target.reboot()  # a fresh PSRAM heap, whatever ran before
    best = {}
    for _ in range(LAUNCHES):
        for name, r in run_bench(target).items():
            print(f"lua {name:>12}: median {r['us_median']:6d} us, max {r['us_max']:6d} us, xip {r['xip']}%")
            if name not in best or r["us_median"] < best[name]["us_median"]:
                best[name] = r
    for name, limit in MAX_US.items():
        assert best[name]["us_median"] <= limit, best[name]
