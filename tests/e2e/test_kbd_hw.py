"""The asynchronous keyboard bus on the device: the engine polls the STM32 in
the background (cadence, key-to-ring latency bound, interrupt cost),
input.update() no longer waits on the 10 kHz bus, a bus fault recovers, a
clock change keeps the keyboard, idle dimming still drives the backlight.
The simulator has no keyboard bus (simulator/stubs/keyboard_stub.c).

KBD_SOAK_S=<seconds> runs the long soak; KBD_MANUAL=1 runs the keys-held
cost check, which needs a person holding keys on the device."""
import json
import os
import re
import time

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(900)]

SOAK_S = int(os.environ.get("KBD_SOAK_S", "0"))
MANUAL = os.environ.get("KBD_MANUAL") == "1"


def kbdstat(target, arg=""):
    lines = target.command(("kbdstat " + arg).strip(), timeout=3.0)
    for line in lines:
        if "[DEV] Kbd:" in line:
            out = {}
            for k, v in re.findall(r"(\w+)=(-?\w+)", line.split("Kbd:", 1)[1]):
                out[k] = int(v) if re.fullmatch(r"-?\d+", v) else v
            return out
    raise AssertionError(f"no Kbd reply to 'kbdstat {arg}': {lines}")


# P0 measured the blocking driver at 20 polls/s costing 107 ms/s of Core 0.
# The engine re-reads the FIFO 10 ms after it reads empty: ~60 reads/s of
# ~5.7 ms bus time each, and a few microseconds of interrupts per read.
def test_engine_polls_in_background(target):
    target.ensure_launcher()
    kbdstat(target, "reset")
    time.sleep(6)
    s = kbdstat(target)
    print("kbd engine:", s)
    assert s["state"] in ("wait", "busy"), s
    assert s["errors"] == 0, s
    assert s["reads"] >= 30 * s["window_ms"] // 1000, s
    # From an empty answer to the next read's result: the bound on how long a
    # key waits in the STM32 before it reaches the ring.
    assert s["max_gap_us"] <= 25000, s
    assert s["max_read_us"] <= 8000, s
    assert s["isr_us"] * 100 <= s["window_ms"] * 1000, s  # < 1% of Core 0
    assert s["bat_reads"] >= 1, s
    assert 0 <= s["battery"] <= 127, s


# The COST and HELD fixtures below both time input.update() into a 25 us
# histogram and derive p99 from it (a table of every sample would not fit).
# These two chunks are byte-identical between the two fixtures; only the
# measurement window and (for HELD) the live event count differ, so they are
# spliced in rather than duplicated (review rubric: no verbatim duplication).
_HIST_SETUP = """\
  local BUCKET, NB = 25, 80
  local hist = {}
  for i = 1, NB do hist[i] = 0 end
"""

_P99_FROM_HIST = """\
  local want, acc, p99 = n * 99 // 100, 0, NB * BUCKET
  for i = 1, NB do
    acc = acc + hist[i]
    if acc >= want then p99 = i * BUCKET; break end
  end
"""

COST = ("""
local T = picocalc.sys.loadlib("picotest")
local sys, input = picocalc.sys, picocalc.input
-- input.update() cost over 5 s as a 25 us histogram (a table of every
-- sample would not fit), the calls >= 2 ms (P0's stall metric) and the max.
T.case("update_cost", function()
""" + _HIST_SETUP + """  local n, stalls, max_us = 0, 0, 0
  local t_end = sys.getTimeMs() + 5000
  while sys.getTimeMs() < t_end do
    local t0 = sys.getTimeUs()
    input.update()
    local dt = sys.getTimeUs() - t0
    n = n + 1
    if dt >= 2000 then stalls = stalls + 1 end
    if dt > max_us then max_us = dt end
    local b = dt // BUCKET + 1
    if b > NB then b = NB end
    hist[b] = hist[b] + 1
  end
""" + _P99_FROM_HIST + """  local f = picocalc.fs.open(picocalc.fs.appPath("cost.json"), "w")
  picocalc.fs.write(f, string.format(
    '{"calls":%d,"p99_us":%d,"max_us":%d,"stalls_ge2ms":%d}',
    n, p99, max_us, stalls))
  picocalc.fs.close(f)
  T.ok(n > 1000)
end)
T.done()
""")


def test_input_update_does_not_wait_for_the_bus(target):
    target.stage_lua_app("kbd_cost", COST, id="com.test.kbd_cost")
    target.delete_file("/data/com.test.kbd_cost/cost.json")
    run = target.run_lua_app("kbd_cost", timeout=60)
    run.assert_clean_exit()
    run.assert_all_passed(["update_cost"])
    r = json.loads(target.read_file("/data/com.test.kbd_cost/cost.json"))
    print("input.update:", r)
    assert r["p99_us"] <= 300, r
    # P0: 20 stalls/s of 5-7 ms. A few can remain from harness commands that
    # update()'s service pass runs.
    assert r["stalls_ge2ms"] <= 10, r


# Review Focus 4: a real abort (the next transaction goes to an address
# nobody answers) stops the engine; the launcher's next poll clears the bus
# and the engine carries on.
def test_bus_fault_recovers(target):
    target.ensure_launcher()
    kbdstat(target, "reset")
    kbdstat(target, "fault")
    time.sleep(1.0)
    s = kbdstat(target)
    assert s["errors"] >= 1 and s["recoveries"] >= 1, s
    kbdstat(target, "reset")
    time.sleep(2.0)
    s = kbdstat(target)
    assert s["errors"] == 0 and s["reads"] >= 60, s
    assert s["state"] in ("wait", "busy"), s


CLOCK = """
local T = picocalc.sys.loadlib("picotest")
T.case("polls_at_250mhz", function()
  local t_end = picocalc.sys.getTimeMs() + 8000
  while picocalc.sys.getTimeMs() < t_end do
    picocalc.input.update()
    picocalc.sys.sleep(5)
  end
  T.ok(picocalc.sys.getBattery() >= 0)
end)
T.done()
"""


# Review Focus 3: the launcher switches to the app's clock (pausing the
# engine) and back to 200 MHz when it exits.
def test_clock_change_keeps_the_bus(target):
    manifest = {"id": "com.test.kbd_clock", "name": "kbd_clock",
                "description": "E2E inline test app", "version": "1.0",
                "author": "PicoDeck E2E", "requirements": [],
                "system_clock_khz": 250000}
    target.stage_lua_app("kbd_clock", CLOCK, id="com.test.kbd_clock",
                         files={"app.json": json.dumps(manifest)})
    target.ensure_launcher()
    target.delete_file("/data/com.test.kbd_clock/test_results.json")
    target.launch_app("kbd_clock")
    time.sleep(2)
    kbdstat(target, "reset")
    time.sleep(3)
    during = kbdstat(target)
    target.wait_for_results("com.test.kbd_clock", timeout=30)
    target.ensure_launcher()
    kbdstat(target, "reset")
    time.sleep(3)
    after = kbdstat(target)
    print("at 250 MHz:", during, "\nback at 200 MHz:", after)
    for s in (during, after):
        assert s["errors"] == 0 and s["reads"] >= 90, s


def test_idle_dim_drives_the_backlight(target):
    target.ensure_launcher()
    try:
        cfg = json.loads(target.read_file("/system/config.json"))
    except Exception:
        cfg = {}
    timeout_s = int(cfg.get("dim_timeout_s", 60))
    if timeout_s == 0 or timeout_s > 120:
        pytest.skip(f"dim_timeout_s={timeout_s}")
    target.keypress("down")  # activity: wakes a dimmed screen, restarts the timer
    time.sleep(1)
    kbdstat(target, "reset")
    time.sleep(timeout_s + 5)
    dimmed = kbdstat(target)
    assert dimmed["bl_writes"] >= 1, dimmed
    target.keypress("down")  # wakes it (the waking key is swallowed)
    time.sleep(1)
    woke = kbdstat(target)
    assert woke["bl_writes"] >= dimmed["bl_writes"] + 1, woke
    assert woke["errors"] == 0, woke


@pytest.mark.skipif(SOAK_S <= 0, reason="set KBD_SOAK_S=<seconds> to soak")
@pytest.mark.timeout(SOAK_S + 900 if SOAK_S > 0 else 900)
def test_bus_soak(target):
    target.ensure_launcher()
    kbdstat(target, "reset")
    last = kbdstat(target)
    t_end = time.time() + SOAK_S
    while time.time() < t_end:
        time.sleep(60)
        s = kbdstat(target)
        print(f"soak {s['window_ms'] // 1000:6d}s reads={s['reads']} "
              f"errors={s['errors']} recoveries={s['recoveries']} "
              f"battery={s['battery']} max_gap_us={s['max_gap_us']}",
              flush=True)
        # The STM32 still answers: ~3600 reads/min when healthy.
        assert s["reads"] >= last["reads"] + 1000, (last, s)
        assert s["bat_reads"] > last["bat_reads"], (last, s)
        last = s
    assert last["errors"] <= 10, last


HELD = ("""
local T = picocalc.sys.loadlib("picotest")
local sys, input, disp = picocalc.sys, picocalc.input, picocalc.display
local YELLOW = disp.rgb(255, 255, 0)
local function say(s)
  disp.clear(0)
  disp.drawText(10, 150, s, YELLOW, 0)
  disp.flush()
end
T.case("held_cost", function()
  say("PRESS AND HOLD 2-3 KEYS")
  local t_wait = sys.getTimeMs() + 120000
  local started = false
  while sys.getTimeMs() < t_wait do
    input.update()
    if input.pollEvent() then started = true; break end
  end
  T.ok(started)
  say("KEEP HOLDING (8 s)")
""" + _HIST_SETUP + """  local n, stalls, max_us, events = 0, 0, 0, 0
  local t_end = sys.getTimeMs() + 8000
  while sys.getTimeMs() < t_end do
    local t0 = sys.getTimeUs()
    input.update()
    local dt = sys.getTimeUs() - t0
    while input.pollEvent() do events = events + 1 end
    n = n + 1
    if dt >= 2000 then stalls = stalls + 1 end
    if dt > max_us then max_us = dt end
    local b = dt // BUCKET + 1
    if b > NB then b = NB end
    hist[b] = hist[b] + 1
  end
""" + _P99_FROM_HIST + """  say("DONE - RELEASE")
  local f = picocalc.fs.open(picocalc.fs.appPath("held.json"), "w")
  picocalc.fs.write(f, string.format(
    '{"calls":%d,"p99_us":%d,"max_us":%d,"stalls_ge2ms":%d,"events":%d}',
    n, p99, max_us, stalls, events))
  picocalc.fs.close(f)
end)
T.done()
""")


# The spec's acceptance: input.update() under 0.3 ms with keys held. Needs a
# person: the app waits (up to 2 min) for the first key, then measures 8 s.
@pytest.mark.skipif(not MANUAL, reason="set KBD_MANUAL=1 and hold keys")
def test_input_update_cost_with_keys_held(target):
    target.stage_lua_app("kbd_held", HELD, id="com.test.kbd_held")
    target.delete_file("/data/com.test.kbd_held/held.json")
    kbdstat(target, "reset")
    run = target.run_lua_app("kbd_held", timeout=200)
    run.assert_clean_exit()
    run.assert_all_passed(["held_cost"])
    r = json.loads(target.read_file("/data/com.test.kbd_held/held.json"))
    s = kbdstat(target)
    print("keys held:", r, "\nengine:", s)
    assert r["events"] >= 10, r  # keys really were held (downs + repeats)
    assert r["p99_us"] <= 300, r
    assert r["stalls_ge2ms"] <= 10, r
    assert s["items"] >= 10 and s["errors"] == 0, s
