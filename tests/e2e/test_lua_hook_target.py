"""The Lua service hook reaches code that never calls into the SDK: tight
loops on the main thread, in coroutines, nested coroutines and in __close
handlers run by coroutine.close. Runs on the simulator (synchronous count
hook) and on hardware (timer-armed hook, src/os/lua_bridge.c)."""
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
CASES = {"tight": TIGHT, "coro": CORO, "nested": NESTED,
         "after_switches": AFTER_SWITCHES, "close": CLOSE}


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
    assert elapsed < 3.0, f"{name}: exit took {elapsed:.2f}s"


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
