"""Lua apps' hot path on the device: the interpreter, Lua calling C, and a
chargrid frame (hw_apps/lua_bench), whose speed hangs on where the code sits
in the XIP cache. src/hot_text.ld pins that code; this guards it.

Where Lua's data sits in PSRAM, which shares that cache, moves it as much or
more: the C calls alone took 32.7 to 77.4 ms on one build, with nothing moved
but Lua's data (a block allocated ahead of it). A single launch says nothing about
the code, and on 2026-11-06 that read as a slowdown from 38.6 ms: the same
commit rebuilt took 55 ms in this test and 33 ms in the right place. So this runs
the app from several places, by padding main.lua with a comment (the source's
block comes before Lua's), and judges each workload by its best: only code that
is in the wrong place is slow in all of them. The first launch after a boot runs
slower, so the reboot comes first and that launch doesn't count."""
import json

import pytest

from hw_target import HW_APPS_DIR

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(600)]

BENCH, BENCH_ID = "lua_bench", "com.test.lua_bench"
# Best medians in us at 300 MHz over PADDINGS. See the module docstring.
MAX_US = {"interpreter": 39000, "C calls": 39500, "fire 64x40": 26500}
PADDINGS = [0, 700, 1400, 2800, 4200, 5600, 7000, 8400]


def run_bench(target):
    target.delete_file(f"/data/{BENCH_ID}/lua_bench.json")
    target.delete_file(f"/data/{BENCH_ID}/test_results.json")
    target.launch_app(BENCH)
    target.wait_for_results(BENCH_ID, timeout=300)
    return {r["name"]: r for r in json.loads(target.read_file(f"/data/{BENCH_ID}/lua_bench.json"))}


def test_lua_hot_path_cost(target):
    source = (HW_APPS_DIR / BENCH / "main.lua").read_bytes()
    target.push_app(HW_APPS_DIR / BENCH, BENCH)
    target.reboot()  # a fresh PSRAM heap, whatever ran before
    run_bench(target)
    best = {}
    for pad in PADDINGS:
        comment = b"--" + b"x" * pad + b"\n" if pad else b""
        target.write_file(f"/apps/.test/{BENCH}/main.lua", comment + source)
        for name, r in run_bench(target).items():
            print(f"lua pad {pad:5d} {name:>12}: median {r['us_median']:6d} us, max {r['us_max']:6d} us, xip {r['xip']}%")
            if name not in best or r["us_median"] < best[name]["us_median"]:
                best[name] = r
    for name, limit in MAX_US.items():
        print(f"lua best {name:>12}: {best[name]['us_median']} us, limit {limit}")
        assert best[name]["us_median"] <= limit, best[name]
