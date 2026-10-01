"""E2E tests for the Lua VM configuration.

CMake passed LUA_32BITS=1 / LUAI_MAXSTACK=500 for years, but upstream
luaconf.h hard-coded both, so the VM silently ran with 64-bit integers,
double floats and a million-slot stack. cmake/picodeck_lua.cmake now patches
luaconf.h to honour the shared config (LUAI_MAXSTACK raised to 1000 — 500
overflowed the minesweeper flood fill); these tests pin the result down
through the lua_runtime fixture app, which logs one "LR <NAME> <value>" line
per probe.
"""
import json
import time

import pytest

from hw_target import HW_APPS_DIR


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _run_fixture(sim, timeout=30.0):
    sim.launch_app("lua_runtime")
    # The fixture logs "LR DONE" after its last probe.
    try:
        sim.wait_for_log(r"LR DONE", timeout=timeout)
    except TimeoutError:
        pytest.fail("LR DONE not seen within %.0fs:\n%s"
                    % (timeout, "\n".join(_lines(sim)[-40:])))
    results = {}
    for l in _lines(sim):
        idx = l.find("LR ")
        if idx < 0:
            continue
        name, _, value = l[idx + 3:].partition(" ")
        results[name] = value
    return results


BINARY_REJECTED = "nil attempt to load a binary chunk (mode is 't')"


def test_lua_runtime_config(simulator):
    r = _run_fixture(simulator)

    # 32-bit integers that wrap, single-precision floats
    assert r["MAXINT"] == "2147483647"
    assert r["MININT"] == "-2147483648"
    assert r["WRAP"] == "true"
    assert r["FLOATTYPE"] == "float"
    assert r["HUGE"] == "true"                  # 1e39 overflows float32
    assert r["FMT01"] == "0.1000000015"         # float32 nearest to 0.1
    assert r["HEX"] == "050c5d1f"               # FNV step wraps mod 2^32
    assert r["JSONF"] == "[0.1,0.33333334] rt=true"

    # coroutine and utf8 are open; the OS-facing libs stay closed
    assert r["CORO"] == "6"
    assert r["UTF8"] == "4:2"
    assert r["SANDBOX"] == "nil,nil,nil,nil"

    # load accepts source text only
    assert r["LOADTEXT"] == "42"
    assert r["LOADBC"] == BINARY_REJECTED
    assert r["LOADBC_MODE_B"] == BINARY_REJECTED
    assert r["LOADBC_READER"] == BINARY_REJECTED
    assert r["LOADBC_HEADER"] == BINARY_REJECTED

    # LUAI_MAXSTACK=1000: runaway recursion is a catchable "stack overflow"
    # (a trivial frame costs ~1 slot, so the depth lands just under the cap)
    assert r["RECURSE"] == "ok=false overflow=true"
    assert 900 < int(r["DEPTH"]) < 1000

    # LUAI_MAXCCALLS=60: Lua -> C -> Lua recursion (nested string.gsub
    # callbacks, the pattern that overflowed the old 4 KB C stack on device)
    # runs 40 deep and then stops with a catchable "C stack overflow" before
    # the 64 KB Lua VM stack is exhausted. Upstream's 200 would let it run
    # ~197 deep, about 170 KB of C stack on hardware.
    assert r["GSUB40"] == "x"
    assert r["GSUBDEEP"] == "ok=false cstack=true"
    assert 40 < int(r["GSUBDEPTH"]) < 60


PROBE_ID = "com.test.hw_probe"


@pytest.mark.sd(fixtures=[], reserve=1)
def test_cstack_probe_cases(target):
    """The hardware probe's cstack mode (tests/e2e/hw_apps/hw_probe, run on
    the device by test_hw_device.py::test_c_stack_depth_on_device) in the
    simulator: nested gsub callbacks, a 250-deep __index chain and unbounded
    recursion each end in a catchable error, and the whole run takes well
    under a second. Issue #21 was its last case freezing the device's
    timer-armed service hook ~1000 frames deep; the simulator's synchronous
    hook cannot show that (tests/unit/test_lua_hook_arm.c and
    test_lua_hook_target.py::test_hook_reaches_deep_recursion do), so this
    pins the cases' Lua outcomes and timing."""
    target.push_app(HW_APPS_DIR / "hw_probe", "hw_probe")
    target.write_file(f"/data/{PROBE_ID}/mode.json",
                      json.dumps({"mode": "cstack"}).encode())
    t0 = time.monotonic()
    target.launch_app("hw_probe")
    doc = target.wait_for_results(PROBE_ID, timeout=20)
    elapsed = time.monotonic() - t0
    cases = {c["name"]: c for c in doc["cases"]}
    assert list(cases) == [
        "mode_known", "gsub_40_deep", "gsub_runaway_is_c_stack_overflow",
        "index_chain_40", "index_chain_250_is_c_stack_overflow",
        "lua_recursion_is_stack_overflow"], doc
    failed = {n: c for n, c in cases.items() if c["status"] != "PASS"}
    assert not failed, failed
    assert elapsed < 10.0, f"cstack cases took {elapsed:.1f}s"
    target.exit_app()
    out = target.wait_for_exit(timeout=15)
    assert out["result"] == "exit_sentinel", out


# Number formatting must not depend on the C library: the firmware links the
# Pico SDK's pico_printf, whose %g keeps trailing zeros (tostring(51.0) was
# "51.00000" on device), whose %f loses digits past precision 9 or 1e9, and
# which has no %a at all; the simulator uses glibc. Lua's l_sprintf now routes
# float conversions through src/os/lua_numfmt.c on both targets. Expected
# values are glibc's output for the same float32 values, except that NaN
# prints as "nan" whatever its sign bit (x86 makes 0/0 negative, ARM does
# not, and glibc shows the sign). One known hardware difference remains and
# is outside the formatter: subnormals (TSMIN) print "0.0" on the device,
# because the RP2350 flushes them to zero when a float is promoted to double
# for C varargs (see the fixture).
FORMAT_EXPECT = {
    "TS51": "51.0",
    "TS01": "0.1",
    "TS1EM5": "1e-05",
    "TS123456": "123456.7",
    "TSTHIRD": "0.3333333",
    "TS1E6": "1000000.0",
    "TS1E7": "1e+07",
    "TS2P30": "1.073742e+09",
    "TSNEG": "-2.5",
    "TSNEGZ": "-0.0",
    "TSINF": "inf -inf",
    "TSNAN": "nan",
    "TSMIN": "9.999999e-39 1.401298e-45",
    "TSMAX": "3.402823e+38",
    "CONCAT": "v=2.5,7.0",
    "SFG": "2.5",
    "SF3F": "0.333",
    "SF51F": "  2.2",
    "SFF": "1.500000",
    "SFE": "1.234568e+04",
    "SF12F": "0.100000001490",
    "SFBIGF": "10000000000.0",
    "SFG3": "1.23e+04",
    "SFGHASH": "1.00000",
    "SFGRANGE": "0.0001 1e-05 100000 1e+06 0",
    "SFE0": "2e+01 4e+01",
    "SFF0": "0 2 2 -0",
    "SFFLAGS": "3.14    |-0002.50| 1.0e+02|000.000123|+5.0E+00 |",
    "SFG99": "0.10000000149011611938",
    "SFINF": "inf -inf nan   inf -inf  |",
    "SFA": "0x1p+0 0X1.99999AP-4 0x1.55p-2",
    "SFQ": "0x1.99999ap-4",
    "SFINT": "  007|0xff|+3|2   |A|10",
    "JSON": "[51.0,100.0,1e-07,2.5,0.33333334]",  # whole floats keep ".0"
}


def test_number_formatting(simulator):
    r = _run_fixture(simulator)
    got = {}
    for name in FORMAT_EXPECT:
        raw = r.get("F_" + name, "<missing>")
        got[name] = raw[1:-1] if raw.startswith("|") and raw.endswith("|") else raw
    wrong = {n: (got[n], want) for n, want in FORMAT_EXPECT.items()
             if got[n] != want}
    assert not wrong, "got != want:\n" + "\n".join(
        "  %-9s %r != %r" % (n, g, w) for n, (g, w) in wrong.items())
