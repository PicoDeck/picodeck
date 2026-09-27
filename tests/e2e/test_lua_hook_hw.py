"""Hardware-only checks for the timer-armed Lua hook: the VM runs without the
per-instruction hook tax, and the watchdog is fed through a long compute loop
inside a coroutine (the simulator has neither the tax nor a watchdog)."""
import time

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(300)]

SPEED = """
local T = picocalc.sys.loadlib("picotest")
T.case("empty_loop_ns", function()
  local n = 2000000
  local best_ms = nil
  for _ = 1, 3 do
    local t0 = picocalc.sys.getTimeMs()
    for _ = 1, n do end
    local ms = picocalc.sys.getTimeMs() - t0
    if best_ms == nil or ms < best_ms then best_ms = ms end
  end
  local f = picocalc.fs.open(picocalc.fs.appPath("ns.txt"), "w")
  picocalc.fs.write(f, string.format("%.1f", best_ms * 1e6 / n))
  picocalc.fs.close(f)
end)
T.done()
"""

WATCHDOG = """
local T = picocalc.sys.loadlib("picotest")
T.case("coroutine_compute_15s", function()
  local co = coroutine.wrap(function()
    local t_end = picocalc.sys.getTimeMs() + 15000
    local x = 0
    while picocalc.sys.getTimeMs() < t_end do x = x + 1 end
    return x
  end)
  T.ok(co() > 0)
end)
T.done()
"""

# This guard's job is catching the per-instruction hook tax coming back:
# P0 (specs/2026-09-27-p0-lua-perf-findings.md) measured 491 ns/iter with the
# permanent count hook. Hook-free measures ~170-181 ns/iter (-O2) and drifts
# a few % with firmware code layout/XIP cache effects, so the guard sits well
# above that — nowhere near 491, where a regression would put it back. -O2
# itself is pinned separately by tests/unit/test_build_flags.py. The fixture
# times the loop 3x and keeps the fastest (best-of-3): harness status polling
# during a run only ever adds time, never subtracts it, so the minimum is the
# least-noisy estimate of the true per-iteration cost.
EMPTY_LOOP_NS_MAX = 250.0


def test_vm_runs_without_per_instruction_hook_cost(target):
    target.stage_lua_app("hook_speed", SPEED, id="com.test.hook_speed")
    # A stale ns.txt from an earlier run would otherwise be read as a pass.
    target.delete_file("/data/com.test.hook_speed/ns.txt")
    run = target.run_lua_app("hook_speed", timeout=60)
    run.assert_clean_exit()
    run.assert_all_passed(["empty_loop_ns"])
    ns = float(target.read_file("/data/com.test.hook_speed/ns.txt"))
    assert ns < EMPTY_LOOP_NS_MAX, f"empty loop {ns} ns/iter"


def test_watchdog_fed_through_long_coroutine_compute(target):
    # Core 1 relays watchdog kicks for 60 s after boot (g_core0_heartbeat_ms
    # starts at 0), which would mask a starved hook: start after that window.
    up = target.status()["uptime_ms"]
    if up < 70000:
        time.sleep((70000 - up) / 1000)
    target.stage_lua_app("hook_wdt", WATCHDOG, id="com.test.hook_wdt")
    run = target.run_lua_app("hook_wdt", timeout=90)
    run.assert_clean_exit()  # a watchdog reset reports device_rebooted
    run.assert_all_passed(["coroutine_compute_15s"])
