"""The Lua service hook reaches code that never calls into the SDK: tight
loops on the main thread, in coroutines, nested coroutines, in __close
handlers run by coroutine.close, and 900 frames deep. Runs on the simulator
(synchronous count hook) and on hardware (timer-armed hook,
src/os/lua_bridge.c)."""
import time

import pytest

pytestmark = [pytest.mark.both, pytest.mark.timeout(300)]

TIGHT = "local x = 0\nwhile true do x = x + 1 end\n"
CORO = """
local co = coroutine.wrap(function() local x = 0 while true do x = x + 1 end end)
co()
"""
NESTED = """
local inner = coroutine.create(function() local x = 0 while true do x = x + 1 end end)
local outer = coroutine.wrap(function() coroutine.resume(inner) end)
outer()
"""
AFTER_SWITCHES = """
local y = coroutine.wrap(function() coroutine.yield(1) end)
y()
local done = coroutine.wrap(function() return 2 end)
done()
local x = 0
while true do x = x + 1 end
"""
CLOSE = """
local co = coroutine.create(function()
  local t <close> = setmetatable({}, { __close = function()
    local x = 0 while true do x = x + 1 end
  end })
  coroutine.yield()
end)
coroutine.resume(co)
coroutine.close(co)
"""
# A failed coroutine.wrap runs its __close handlers (here a tight loop)
# inside the coroutine, on the way out through the error (Minor #4).
WRAP_CLOSE = """
local f = coroutine.wrap(function()
  local t <close> = setmetatable({}, { __close = function()
    local x = 0 while true do x = x + 1 end
  end })
  error("boom")
end)
f()
"""
CASES = {"tight": TIGHT, "coro": CORO, "nested": NESTED,
         "after_switches": AFTER_SWITCHES, "close": CLOSE,
         "wrap_close": WRAP_CLOSE}

# A tight loop 900 Lua frames deep: the smallest frames (one stack slot
# each) near the LUAI_MAXSTACK limit, as unbounded recursion reaches before
# its "stack overflow" (hw_probe's lua_recursion case). "deep" is recorded
# at the bottom, before the loop.
DEEP_DEPTH = 900
DEEP = f"""
local T = picocalc.sys.loadlib("picotest")
local depth = 0
local function spin() local x = 0 while true do x = x + 1 end end
local function dive()
  depth = depth + 1
  if depth >= {DEEP_DEPTH} then
    T.case("deep", function() T.eq(depth, {DEEP_DEPTH}) end)
    spin()
  end
  return 1 + dive()
end
dive()
"""


@pytest.mark.parametrize("name", sorted(CASES))
def test_exit_reaches_hook_free_loop(target, name):
    app = f"hook_{name}"
    target.stage_lua_app(app, CASES[name], id=f"com.test.{app}")
    target.launch_app(app)
    time.sleep(1.5)
    t0 = time.monotonic()
    r = target.exit_app()
    assert r["ok"], r
    out = target.wait_for_exit(timeout=15)
    elapsed = time.monotonic() - t0
    assert out["result"] == "exit_sentinel", out
    # A hook that never reaches the loop hangs until the watchdog resets the
    # device (out["result"] would be "device_rebooted", not "exit_sentinel"),
    # so this bound only needs to separate "exited" from "hung" — it is not a
    # hook-latency budget. On hardware, wait_for_exit's elapsed time includes
    # the launcher's post-exit app rescan and the error.log download, which
    # scale with device state (app count, log size), not the hook.
    assert elapsed < 10.0, f"{name}: exit took {elapsed:.2f}s"


def test_status_answers_during_tight_loop(target):
    target.stage_lua_app("hook_status", TIGHT, id="com.test.hook_status")
    target.launch_app("hook_status")
    time.sleep(1.0)
    worst = 0.0
    for _ in range(5):
        t0 = time.monotonic()
        st = target.status()
        worst = max(worst, time.monotonic() - t0)
        assert st["app"] != "launcher", st
    target.exit_app()
    target.wait_for_exit(timeout=15)
    assert worst < 1.0, f"status round trip {worst:.2f}s during a tight loop"


@pytest.mark.sd(fixtures=[], reserve=1)
def test_hook_reaches_deep_recursion(target):
    """The service hook keeps running with the VM 900 frames deep (issue
    #21). On the device the 1 ms timer that arms the hook used lua_sethook,
    which walks every frame of the call chain: this deep, that walk of QMI
    PSRAM took longer than the timer period, the timer IRQ re-fired forever,
    the VM never ran again and the watchdog reset the device. Here `status`
    must be answered from inside the hook and `exit` must end the app."""
    app, app_id = "hook_deep", "com.test.hook_deep"
    target.stage_lua_app(app, DEEP, id=app_id)
    target.delete_file(f"/data/{app_id}/test_results.json")
    target.launch_app(app)
    doc = target.wait_for_results(
        app_id, timeout=20,
        until=lambda d: any(c["name"] == "deep" for c in d.get("cases", [])))
    assert all(c["status"] == "PASS" for c in doc["cases"]), doc
    time.sleep(1.0)  # spinning at the bottom
    st = target.status()
    assert st["app"] != "launcher", st
    r = target.exit_app()
    assert r["ok"], r
    out = target.wait_for_exit(timeout=15)
    assert out["result"] == "exit_sentinel", out
