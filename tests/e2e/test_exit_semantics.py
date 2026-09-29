"""App exit is sticky: a pcall/xpcall/coroutine that swallows the exit
sentinel cannot keep the app alive (review: Lua High "Exit is an ordinary
lua_error with a sentinel ...").

sys.exit(), the system menu's Exit App and the dev `exit` command (exit_app)
all go through lua_bridge_raise_exit, which records the request in a flag the
Lua runner owns and makes the count hook raise again before every
instruction. Each case stages an app whose main loop would swallow the
sentinel, asks it to exit, and requires the "exit_sentinel" outcome within a
few seconds (the old behaviour: the app never exits, wait_for_exit times out).
"""

import time

import pytest

from helpers import stage_lua_app
from net_servers import HttpTestServer

EXIT_TIMEOUT = 5.0

# Burns instructions forever inside one protected call; only an error that
# escapes every pcall/xpcall/resume ends it.
BUSY = """
local function busy()
    local x = 0
    while true do
        for i = 1, 100 do x = x + i end
    end
end
"""


def _run_until_ready(sim, name, code, requirements=()):
    stage_lua_app(sim.sd_card_path, name, code, requirements=requirements)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(name)
    sim.wait_for_log(r"^EXIT_READY$", timeout=10, since_seq=seq)


def _exit_and_wait(sim):
    sim.exit_app()
    return sim.wait_for_exit(timeout=EXIT_TIMEOUT)


def _assert_exited(outcome):
    assert outcome.get("result") == "exit_sentinel", outcome


@pytest.mark.parametrize("wrapper", [
    # pcall around the loop, retried forever
    "while true do pcall(busy) end",
    # xpcall with a message handler that swallows everything
    "while true do xpcall(busy, function(e) return e end) end",
    # nested pcalls
    "while true do pcall(function() while true do pcall(busy) end end) end",
    # the loop in a coroutine; resume catches errors like pcall
    "while true do coroutine.resume(coroutine.create(busy)) end",
    # the app treats every error as "retry"
    "while true do local ok, e = pcall(busy); if not ok then x_err = e end end",
])
def test_exit_app_escapes_swallowing_loop(simulator, wrapper):
    code = BUSY + 'picocalc.sys.log("EXIT_READY")\n' + wrapper + "\n"
    _run_until_ready(simulator, "exit_swallow", code)
    _assert_exited(_exit_and_wait(simulator))


def test_sys_exit_inside_pcall_exits(simulator):
    """sys.exit() whose sentinel the app catches still ends the app."""
    code = BUSY + """
picocalc.sys.log("EXIT_READY")
local ok = pcall(picocalc.sys.exit)
picocalc.sys.log("SWALLOWED " .. tostring(ok))
while true do pcall(busy) end
"""
    stage_lua_app(simulator.sd_card_path, "exit_syspcall", code)
    simulator.launch_app("exit_syspcall")
    _assert_exited(simulator.wait_for_exit(timeout=EXIT_TIMEOUT))


def test_exit_rethrown_as_other_error_shows_no_error_screen(simulator):
    """An app that turns the caught sentinel into its own error still exits
    cleanly: the exit request wins, no error screen / error outcome."""
    code = BUSY + """
picocalc.sys.log("EXIT_READY")
local ok, e = pcall(busy)
error("loop failed: " .. tostring(e))
"""
    _run_until_ready(simulator, "exit_rethrow", code)
    _assert_exited(_exit_and_wait(simulator))


def test_exit_during_protected_sleep(simulator):
    code = """
picocalc.sys.log("EXIT_READY")
while true do pcall(picocalc.sys.sleep, 1000) end
"""
    _run_until_ready(simulator, "exit_sleep", code)
    _assert_exited(_exit_and_wait(simulator))


@pytest.mark.sd(fixtures=[], reserve=2)  # stages exit_first and exit_second
def test_next_app_runs_normally_after_swallowed_exit(simulator):
    """The request is per app: the runner clears it, so the next app is not
    killed by the previous app's exit."""
    code = BUSY + 'picocalc.sys.log("EXIT_READY")\nwhile true do pcall(busy) end\n'
    _run_until_ready(simulator, "exit_first", code)
    _assert_exited(_exit_and_wait(simulator))

    stage_lua_app(simulator.sd_card_path, "exit_second", """
local x = 0
for i = 1, 200000 do x = x + i end   -- many hook intervals
picocalc.sys.sleep(50)
picocalc.sys.log("SECOND_DONE " .. x)
""")
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("exit_second")
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome.get("result") == "returned", outcome
    simulator.wait_for_log(r"^SECOND_DONE ", timeout=5, since_seq=seq)


@pytest.fixture
def http_server():
    srv = HttpTestServer()
    srv.start()
    yield srv
    srv.stop()


def test_sys_exit_inside_http_callback_exits(simulator, http_server):
    """The HTTP callback runs inside a lua_pcall (http_fire); sys.exit() there
    used to be printed as a callback error and the app kept running."""
    simulator.call("set_wifi_state", {"status": "connected"})
    code = """
local net = picocalc.network
local conn = net.http.new("127.0.0.1", %d, false, "exit_test")
conn:setRequestCompleteCallback(function()
    picocalc.sys.log("CALLBACK_EXIT")
    picocalc.sys.exit()
end)
conn:get("/ok")
picocalc.sys.log("EXIT_READY")
local t0 = picocalc.sys.getTimeMs()
local x = 0
while picocalc.sys.getTimeMs() - t0 < 20000 do
    for i = 1, 100 do x = x + i end
end
picocalc.sys.log("STILL_RUNNING")
""" % http_server.port
    stage_lua_app(simulator.sd_card_path, "exit_http", code,
                  requirements=["http"])
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("exit_http")
    simulator.wait_for_log(r"^CALLBACK_EXIT$", timeout=15, since_seq=seq)
    _assert_exited(simulator.wait_for_exit(timeout=EXIT_TIMEOUT))


# ── sys.sleep shares the hook's service pass (review: Lua Low "Three service
# loops have drifted: sleep ignores the menu and exit") ──────────────────────

def _wait_white(sim, x=6, y=40, timeout=3.0):
    """The app's white frame on the panel (the log line can come before the
    present): the baseline a menu overlay then changes."""
    deadline = time.time() + timeout
    px = sim.call("get_pixel", {"x": x, "y": y})
    while px["rgb565"] != 0xFFFF and time.time() < deadline:
        time.sleep(0.02)
        px = sim.call("get_pixel", {"x": x, "y": y})
    assert px["rgb565"] == 0xFFFF, px
    return px


def _pixel_changes(sim, x, y, before, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        px = sim.call("get_pixel", {"x": x, "y": y})
        if px["rgb565"] != before["rgb565"]:
            return px
        time.sleep(0.02)
    return None


def test_menu_opens_during_long_sleep(simulator):
    """A Sym press during a long sys.sleep opens the system menu at once
    (it used to wait for the sleep to end); its Exit App ends the app."""
    code = """
local d = picocalc.display
d.clear(d.WHITE)
d.flush()
picocalc.sys.log("EXIT_READY")
picocalc.sys.sleep(30000)
picocalc.sys.log("SLEEP_RETURNED")
"""
    _run_until_ready(simulator, "sleep_menu", code)
    before = _wait_white(simulator)
    simulator.keypress("menu")
    after = _pixel_changes(simulator, 6, 40, before, timeout=3.0)
    assert after is not None, "system menu did not open during sys.sleep(30000)"
    simulator.keypress("esc")
    _assert_exited(_exit_and_wait(simulator))


def test_menu_exit_escapes_pcall_loop(simulator):
    """The system menu's Exit App, chosen while the app's loop runs inside
    pcall, ends the app (the menu raises the sentinel inside that pcall)."""
    code = BUSY + """
local d = picocalc.display
d.clear(d.WHITE)
d.flush()
picocalc.sys.log("EXIT_READY")
while true do pcall(busy) end
"""
    _run_until_ready(simulator, "menu_exit", code)
    before = _wait_white(simulator)
    simulator.keypress("menu")
    assert _pixel_changes(simulator, 6, 40, before, timeout=3.0), \
        "system menu did not open"
    # Up wraps from the first item to the last one, Exit App. (Paced, not
    # wait_input_consumed: the simulator's consumed_seq can stay behind a
    # menu injection, a harness quirk seen with the old hook too.)
    time.sleep(0.2)
    simulator.keypress_sequence(["up", "enter"], delay_ms=200)
    _assert_exited(simulator.wait_for_exit(timeout=EXIT_TIMEOUT))


def test_key_edges_during_sleep_reach_the_app_after_it(simulator):
    """sys.sleep polls the keyboard in the background (so the physical Sym
    key works during it); keys that arrive meanwhile must still reach the
    app's next input.update(): the press edge, the char and the events."""
    code = """
local pc = picocalc
local input = pc.input
input.update()
while input.pollEvent() do end
pc.sys.log("EXIT_READY")
pc.sys.sleep(1500)
input.update()
local pressed = input.getButtonsPressed()
local c = input.getChar()
local downs = 0
while true do
    local ev = input.pollEvent()
    if not ev then break end
    if ev.type == "down" then downs = downs + 1 end
end
pc.sys.log(string.format("AFTER enter=%s char=%s downs=%d",
    tostring(pressed & input.BTN_ENTER ~= 0), tostring(c), downs))
"""
    stage_lua_app(simulator.sd_card_path, "sleep_edges", code)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("sleep_edges")
    simulator.wait_for_log(r"^EXIT_READY$", timeout=10, since_seq=seq)
    time.sleep(0.3)
    simulator.keypress("enter")
    time.sleep(0.2)
    simulator.keypress("q")
    line = simulator.wait_for_log(r"^AFTER ", timeout=10, since_seq=seq)
    assert line.startswith("AFTER enter=true char=q"), line
    assert int(line.rsplit("=", 1)[1]) >= 2, line
