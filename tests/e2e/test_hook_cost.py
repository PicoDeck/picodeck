"""The Lua count hook and its service pass (review: Lua Medium "The debug hook
fires every 256 opcodes ... The sound poll pcalls from the hook and never pops
the error").

- The hook's count adapts (128-4096 instructions, about one call per ms of
  wall time) and the expensive part of the service pass runs only when work
  is flagged pending or 5 ms have passed; input.update() runs the same gated
  pass. The latency tests below pin that apps which spend their time in C
  calls (a few instructions per frame) are served no worse than with the old
  fixed 256: dev commands (the MCP keypress path on hardware, exit_app) and
  injected keys still arrive promptly.
- A failing sound callback's error is popped. lua_bridge_service also runs
  from sys.sleep, whose C frame lives for the whole sleep: before the fix
  every error stayed on that stack until lua_rawgeti wrote past it (ASan:
  heap-buffer-overflow in finishrawget after ~1000 callbacks).
"""

import statistics
import time
from pathlib import Path

from helpers import app_rel_dir, stage_lua_app, write_wav

# A frame loop that runs a handful of instructions per 60 Hz frame: nearly
# all its time is in display.clear/flush.
DRAW_LOOP = """
local pc = picocalc
local d, input = pc.display, pc.input
pc.sys.log("LOW_READY")
while true do
    %s
    d.clear(d.BLACK)
    d.flush()
end
"""
POLL_INPUT = """input.update()
    local c = input.getChar()
    if c then pc.sys.log("GOT " .. c) end"""


def _start(sim, name, code):
    stage_lua_app(sim.sd_card_path, name, code)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(name)
    sim.wait_for_log(r"^LOW_READY$", timeout=10, since_seq=seq)
    time.sleep(0.3)


def _ping_ms(sim, n=10):
    """Round trips of the dev `ping` command, which only a running app's
    service pass (hook, sys.sleep, input.update) executes."""
    times = []
    for _ in range(n):
        t = time.time()
        r = sim.call("dev_command", {"cmd": "ping", "timeout_ms": 5000})
        times.append((time.time() - t) * 1000)
        assert r.get("ok"), r
        time.sleep(0.02)
    return statistics.median(times)


def test_dev_commands_reach_input_polling_draw_loop(simulator):
    """input.update() runs the service pass, so an app that polls input once
    a frame runs dev commands every frame (old fixed hook: ~200 ms here)."""
    _start(simulator, "hook_poll_loop", DRAW_LOOP % POLL_INPUT)
    ms = _ping_ms(simulator)
    assert ms < 120, f"dev command round trip {ms:.0f} ms"


def test_dev_commands_reach_non_polling_draw_loop(simulator):
    """No input polling at all: only the count hook serves it. The adaptive
    count keeps this no slower than the old fixed 256 (~350 ms here); a
    fixed 1024 took ~1.5 s."""
    _start(simulator, "hook_draw_loop", DRAW_LOOP % "")
    ms = _ping_ms(simulator)
    assert ms < 700, f"dev command round trip {ms:.0f} ms"
    t = time.time()
    simulator.exit_app()
    outcome = simulator.wait_for_exit(timeout=10)
    assert outcome.get("result") == "exit_sentinel", outcome
    assert time.time() - t < 1.0, "exit_app took over a second"


def test_exit_after_compute_burst_then_non_polling_draw_loop(simulator):
    """Review finding: a compute phase drives the hook count to its maximum
    (4096), and a following draw loop runs a few instructions per frame, so
    the next hook call was seconds away (past the device's 10 s watchdog).
    display.flush runs the gated service pass every frame."""
    code = """
local pc = picocalc
local d = pc.display
local acc = 0
for i = 1, 3000000 do acc = acc + i % 7 end   -- count climbs to the max
pc.sys.log("LOW_READY")
while true do
    d.clear(d.BLACK)
    d.flush()
end
"""
    stage_lua_app(simulator.sd_card_path, "hook_burst_draw", code)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("hook_burst_draw")
    simulator.wait_for_log(r"^LOW_READY$", timeout=10, since_seq=seq)
    time.sleep(0.3)
    t = time.time()
    simulator.exit_app()
    outcome = simulator.wait_for_exit(timeout=15)
    elapsed = time.time() - t
    assert outcome.get("result") == "exit_sentinel", outcome
    assert elapsed < 1.0, f"exit_app took {elapsed:.1f} s after a compute burst"


def test_coroutine_adapting_does_not_pin_main_thread_count(simulator):
    """Review finding: hook counts are per thread. A coroutine that adapted
    its own count down (a slow phase) must not stop the main thread's count
    from coming down when main enters a slow phase of its own. The slow
    phases are table.sort calls (C, a few instructions each, ~1-2 ms here)
    that never serve the system, so only the count hook runs dev commands."""
    code = """
local pc = picocalc
local t = {}
for i = 1, 5000 do t[i] = (i * 7919) % 5003 end   -- count climbs to the max
local function slow_for(ms)
    local t0 = pc.sys.getTimeMs()
    while pc.sys.getTimeMs() - t0 < ms do table.sort(t) end
end
-- The coroutine inherits the maximum and adapts down to the minimum.
local co = coroutine.create(function() slow_for(1500) end)
coroutine.resume(co)
pc.sys.log("LOW_READY")
while true do table.sort(t) end               -- main: slow phase
"""
    stage_lua_app(simulator.sd_card_path, "hook_coroutine", code)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("hook_coroutine")
    simulator.wait_for_log(r"^LOW_READY$", timeout=15, since_seq=seq)
    time.sleep(3.0)   # past main's first (long) hook gap of the slow phase
    ms = _ping_ms(simulator, n=8)
    assert ms < 250, f"dev command round trip {ms:.0f} ms: main thread count stuck"
    simulator.exit_app()
    assert simulator.wait_for_exit(timeout=15).get("result") == "exit_sentinel"


def test_injected_keys_reach_draw_loop(simulator):
    _start(simulator, "hook_key_loop", DRAW_LOOP % POLL_INPUT)
    for ch in "abcde":
        seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
        t = time.time()
        simulator.keypress(ch)
        simulator.wait_for_log(r"^GOT " + ch + "$", timeout=2, since_seq=seq)
        assert time.time() - t < 0.5, f"key {ch!r} took {time.time() - t:.2f} s"
        time.sleep(0.05)


def test_sound_callback_fires_promptly_in_compute_loop(simulator):
    """Core 1's trampoline flags the service pass pending, so a finish
    callback runs within a hook call or two of the file ending, not at the
    next 5 ms period."""
    code = """
local pc = picocalc
local fired_at
local fp = pc.sound.fileplayer()
assert(fp:load(APP_DIR .. "/tick.wav"))
fp:setFinishCallback(function() fired_at = pc.sys.getTimeMs() end)
fp:play()
local t0 = pc.sys.getTimeMs()
local x = 0
while not fired_at and pc.sys.getTimeMs() - t0 < 3000 do
    for i = 1, 1000 do x = x + i end
end
pc.sys.log("FINISH_AFTER " .. tostring(fired_at and (fired_at - t0)))
"""
    stage_lua_app(simulator.sd_card_path, "hook_sound_cb", code)
    write_wav(Path(simulator.sd_card_path) / app_rel_dir("hook_sound_cb") / "tick.wav",
              seconds=0.05)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("hook_sound_cb")
    assert simulator.wait_for_exit(timeout=15).get("result") == "returned"
    line = simulator.wait_for_log(r"^FINISH_AFTER ", timeout=5, since_seq=seq)
    assert line != "FINISH_AFTER nil", "finish callback never fired"
    assert int(line.split()[1]) < 1000, line


def test_failing_sound_callbacks_do_not_pile_up_during_sleep(simulator):
    """A looping fileplayer whose loop callback always raises fires ~100
    callbacks a second during one long sys.sleep; more than LUAI_MAXSTACK
    (1000) of them must neither overflow the stack nor stop firing."""
    code = """
local pc = picocalc
local n = 0
local fp = pc.sound.fileplayer()
assert(fp:load(APP_DIR .. "/tick.wav"), "load tick.wav")
fp:setLoopRange(0, 0)
fp:setLoopCallback(function() n = n + 1; error("loop callback failed") end)
fp:play()
pc.sys.sleep(12500)
local m = n
pc.sys.sleep(1000)
fp:stop()
pc.sys.log("SOUND_CALLS " .. m .. " " .. (n - m))
"""
    stage_lua_app(simulator.sd_card_path, "hook_sound_err", code)
    write_wav(Path(simulator.sd_card_path) / app_rel_dir("hook_sound_err") / "tick.wav",
              seconds=0.01)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("hook_sound_err")
    outcome = simulator.wait_for_exit(timeout=40)
    assert outcome.get("result") == "returned", outcome
    line = simulator.wait_for_log(r"^SOUND_CALLS ", timeout=5, since_seq=seq)
    total, last = (int(v) for v in line.split()[1:3])
    assert total > 1000, f"only {total} loop callbacks in 12.5 s: {line}"
    assert last > 0, f"loop callbacks stopped firing: {line}"
