"""WiFi across an app's own clock (issue #71).

An app with system_clock_khz and no "http" runs with WiFi down: the launcher
disconnects before the clock change. When the app exits, WiFi goes back to the
state it was in before the launch, unless someone asked for a change meanwhile
(wifi_leave_for_clock / wifi_rejoin_after_clock, wifi.h; the decision is
src/drivers/wifi_rejoin.h, tests/unit/test_wifi_rejoin.c).

What each build can show:

- The `both` tests run on the simulator and on a device (--target hw:<port>,
  WiFi configured). Only the device exercises the radio: the real join's time,
  the CYW43 across the clock change, a launcher that keeps working meanwhile.
- The default simulator's WiFi (sim_wifi.c) is a mock: instantly online, no
  SNTP, no auto-disconnect. It shows the launcher's and the apps' side.
- The firmware_net tests run wifi.c itself on a stand-in link
  (make simulator-net): the Core 0 / Core 1 request path, the real status, and
  the boot auto-connect's time-sync disconnect, against a local SNTP server.
  The link still comes up at once and there is no chip.
"""

from __future__ import annotations

import json
import time

import pytest

from helpers import stage_lua_app
from net_servers import SntpServer

CLOCK_KHZ = 300000
UP = ("connected", "online")
JOIN_TIMEOUT = {"hw": 45.0, "sim": 10.0}  # a real join, with room for a retry

# The launch's disconnect holds the launcher up to WIFI_HW_DISCONNECT_WAIT_MS,
# and the mock never reports idle, so give every exit room for that.
EXIT_TIMEOUT = 30.0

pytestmark = pytest.mark.sd(fixtures=[])


def manifest(name: str, requirements=(), clocked: bool = True) -> str:
    doc = {"id": f"com.test.{name}", "name": name, "version": "1.0",
           "description": "E2E: WiFi across an app's own clock",
           "author": "PicoDeck E2E", "requirements": list(requirements)}
    if clocked:
        doc["system_clock_khz"] = CLOCK_KHZ
    return json.dumps(doc)


def stage_app(target, name: str, lua: str, requirements=(), clocked: bool = True):
    target.stage_lua_app(name, lua, requirements=requirements,
                         id=f"com.test.{name}",
                         files={"app.json": manifest(name, requirements, clocked)})


def wifi_status(target) -> str:
    return target.status().get("wifi")


def wait_wifi(target, want, timeout: float = 0.0) -> str:
    """Poll until the status is one of `want`; returns the last one seen."""
    deadline = time.monotonic() + (timeout or JOIN_TIMEOUT[target.kind])
    st = wifi_status(target)
    while st not in want and time.monotonic() < deadline:
        time.sleep(0.05 if target.kind == "sim" else 0.5)
        st = wifi_status(target)
    return st


def stays(target, want, secs: float = 1.5) -> str:
    """Poll for `secs`; returns the first status outside `want`, or the last."""
    deadline = time.monotonic() + secs
    st = wifi_status(target)
    while st in want and time.monotonic() < deadline:
        time.sleep(0.05 if target.kind == "sim" else 0.5)
        st = wifi_status(target)
    return st


# An "http" app: the launcher joins the saved network for it, and that
# connection outlives the app (no auto-disconnect after its time sync).
JOIN_LUA = """
local T = picocalc.sys.loadlib("picotest")
local wifi, sys = picocalc.wifi, picocalc.sys
T.case("joined", function()
  local t_end = sys.getTimeMs() + 30000
  local st = wifi.getStatus()
  while st ~= wifi.STATUS_CONNECTED and st ~= wifi.STATUS_ONLINE
        and sys.getTimeMs() < t_end do
    sys.sleep(50)
    st = wifi.getStatus()
  end
  T.ok(st == wifi.STATUS_CONNECTED or st == wifi.STATUS_ONLINE, "status " .. st)
end)
T.done()
"""


def run_cases(target, name: str, timeout: float = 60.0):
    run = target.run_lua_app(name, timeout=timeout, poll_s=1.0)
    run.assert_all_passed(list(run.cases))
    assert run.cases, run.describe()
    return run


def bring_wifi_up(target):
    """WiFi up through the launcher, whatever the card's settings."""
    stage_app(target, "wc_join", JOIN_LUA, requirements=["http"], clocked=False)
    run = target.run_lua_app("wc_join", timeout=60.0, poll_s=1.0)
    if run.cases.get("joined", {}).get("status") != "PASS":
        if target.kind == "hw":
            pytest.skip("device WiFi did not join: configure wifi_ssid/wifi_pass")
        pytest.fail(run.describe())


@pytest.fixture
def wifi_up(target):
    """The device online before the test; on hardware, back online after it
    (a simulator is per test)."""
    bring_wifi_up(target)
    assert wifi_status(target) in UP
    yield target
    if target.kind == "hw":
        target.ensure_launcher()
        bring_wifi_up(target)


# ── The launch takes WiFi down, the exit gives it back ───────────────────────

# Reports what the app sees, then leaves the way the test names.
OFF_LUA = """
local T = picocalc.sys.loadlib("picotest")
local wifi, sys = picocalc.wifi, picocalc.sys
T.case("wifi_is_off_in_the_app", function()
  T.eq(wifi.getStatus(), wifi.STATUS_DISCONNECTED)
  T.eq(wifi.getSSID(), nil)
end)
T.done()
@LEAVE@
"""

LEAVE = {
    "return": "return",
    "sys_exit": "sys.exit()",
    "error": 'error("wifi_clock: the app fails")',
    "dev_exit": "while true do sys.sleep(20) end",
}


@pytest.mark.both
@pytest.mark.parametrize("how", sorted(LEAVE))
def test_wifi_comes_back_however_the_app_exits(wifi_up, how):
    target = wifi_up
    name = f"wc_{how}"
    stage_app(target, name, OFF_LUA.replace("@LEAVE@", LEAVE[how]))
    target.launch_app(name)
    doc = target.wait_for_results(f"com.test.{name}", timeout=EXIT_TIMEOUT)
    assert doc["fail"] == 0, doc
    if how == "dev_exit":
        target.exit_app()
    # A Lua error's screen waits for Esc on the device (--test-mode: none).
    deadline = time.monotonic() + EXIT_TIMEOUT
    while True:
        try:
            out = target.wait_for_exit(timeout=3.0, poll_s=0.5)
            break
        except TimeoutError:
            assert time.monotonic() < deadline, f"{name} did not exit"
            if how == "error":
                target.keypress("esc")
    # (The device's status polling cannot tell sys.exit() from a return.)
    assert out["result"] in {"return": ("returned",), "error": ("error",),
                             "dev_exit": ("exit_sentinel",),
                             "sys_exit": ("exit_sentinel", "returned")}[how], out
    # The launcher is back whatever the join is doing.
    assert target.status()["app"] == "launcher"
    st = wait_wifi(target, UP)
    assert st in UP, f"WiFi is {st!r} after {how}, it was up before the launch"


@pytest.mark.both
def test_wifi_comes_back_every_time(wifi_up):
    """Launch after launch, including one straight after the last exit's
    rejoin started."""
    target = wifi_up
    stage_app(target, "wc_again", OFF_LUA.replace("@LEAVE@", "return"))
    for _ in range(3):
        target.launch_app("wc_again")
        target.wait_for_exit(timeout=EXIT_TIMEOUT, poll_s=0.5)
    st = wait_wifi(target, UP)
    assert st in UP, st


# ── Whoever decided otherwise stays decided ──────────────────────────────────

@pytest.mark.both
def test_wifi_turned_off_before_the_launch_stays_off(wifi_up):
    target = wifi_up
    stage_app(target, "wc_userdisc", "picocalc.wifi.disconnect()", clocked=False)
    target.launch_app("wc_userdisc")
    target.wait_for_exit(timeout=EXIT_TIMEOUT, poll_s=0.5)
    assert wait_wifi(target, ("disconnected",), timeout=10) == "disconnected"

    stage_app(target, "wc_clocked", OFF_LUA.replace("@LEAVE@", "return"))
    target.launch_app("wc_clocked")
    assert target.wait_for_exit(timeout=EXIT_TIMEOUT,
                                poll_s=0.5)["result"] == "returned"
    st = stays(target, ("disconnected",), secs=3.0)
    assert st == "disconnected", f"nobody asked for WiFi, but it is {st!r}"


@pytest.mark.both
def test_the_apps_own_disconnect_stands(wifi_up):
    target = wifi_up
    stage_app(target, "wc_appdisc", "picocalc.wifi.disconnect()")
    target.launch_app("wc_appdisc")
    target.wait_for_exit(timeout=EXIT_TIMEOUT, poll_s=0.5)
    st = stays(target, ("disconnected",), secs=3.0)
    assert st == "disconnected", st


# Stored credentials on the device; a network of its own in the simulator,
# whose default card has none.
CONNECT_LUA = """
local T = picocalc.sys.loadlib("picotest")
local wifi, sys = picocalc.wifi, picocalc.sys
local function joined()
  local st = wifi.getStatus()
  return st == wifi.STATUS_CONNECTED or st == wifi.STATUS_ONLINE
end
T.case("the_app_connects", function()
  picocalc.network.setEnabled(true)
  if wifi.getStatus() == wifi.STATUS_DISCONNECTED then
    wifi.connect("AppNet", "apppass")
  end
  local t_end = sys.getTimeMs() + 30000
  while not joined() and sys.getTimeMs() < t_end do sys.sleep(50) end
  T.ok(joined(), "status " .. wifi.getStatus())
end)
@THEN@
T.done()
"""


@pytest.mark.both
def test_the_apps_own_connect_stands(wifi_up):
    """It is up at exit: not joined again, so whatever it talks to stays up."""
    target = wifi_up
    stage_app(target, "wc_appconn", CONNECT_LUA.replace("@THEN@", ""))
    run_cases(target, "wc_appconn")
    st = stays(target, UP, secs=3.0)
    assert st in UP, st


@pytest.mark.both
def test_the_apps_connect_then_disconnect_stands(wifi_up):
    """The last request wins: nothing here may rejoin behind the app's back."""
    target = wifi_up
    stage_app(target, "wc_appboth", CONNECT_LUA.replace(
        "@THEN@", 'T.case("the_app_disconnects", function() wifi.disconnect() end)'))
    run_cases(target, "wc_appboth")
    st = stays(target, ("disconnected",), secs=3.0)
    assert st == "disconnected", st


def test_the_apps_network_is_not_replaced(simulator):
    """Connected to its own network at exit: the one from before the launch
    does not come back over it."""
    code = """
picocalc.wifi.connect("AppNet", "apppass")
local t_end = picocalc.sys.getTimeMs() + 10000
while picocalc.wifi.getSSID() ~= "AppNet" and picocalc.sys.getTimeMs() < t_end do
  picocalc.sys.sleep(20)
end
"""
    sd = simulator.sd_card_path
    stage_lua_app(sd, "wc_appnet", code, files={"app.json": manifest("wc_appnet")})
    simulator.launch_app("wc_appnet")
    simulator.wait_for_exit(timeout=EXIT_TIMEOUT)
    assert simulator.call("get_wifi_state")["ssid"] == "AppNet"


# ── What the launch does not touch ───────────────────────────────────────────

@pytest.mark.both
def test_a_clocked_http_app_keeps_the_link(wifi_up):
    target = wifi_up
    lua = """
local T = picocalc.sys.loadlib("picotest")
local wifi = picocalc.wifi
T.case("still_up", function()
  local st = wifi.getStatus()
  T.ok(st == wifi.STATUS_CONNECTED or st == wifi.STATUS_ONLINE, "status " .. st)
  T.ok(wifi.getSSID() ~= nil, "no SSID")
end)
T.done()
"""
    stage_app(target, "wc_http", lua, requirements=["http"])
    run_cases(target, "wc_http")
    st = stays(target, UP, secs=3.0)
    assert st in UP, st


@pytest.mark.both
def test_an_app_without_its_own_clock_keeps_the_link(wifi_up):
    target = wifi_up
    lua = """
local T = picocalc.sys.loadlib("picotest")
local wifi = picocalc.wifi
T.case("still_up", function()
  local st = wifi.getStatus()
  T.ok(st == wifi.STATUS_CONNECTED or st == wifi.STATUS_ONLINE, "status " .. st)
end)
T.done()
"""
    stage_app(target, "wc_plain", lua, clocked=False)
    run_cases(target, "wc_plain")
    st = stays(target, UP, secs=3.0)
    assert st in UP, st


def test_wifi_unavailable_stays_unavailable(simulator):
    """No WiFi on the board: nothing to take down, nothing to bring back.
    (The mock's injection; the firmware-network build's wifi.c does not read
    it, so this only asserts what both report.)"""
    simulator.call("set_wifi_state", {"status": "not_available"})
    stage_lua_app(simulator.sd_card_path, "wc_nowifi", "",
                  files={"app.json": manifest("wc_nowifi")})
    simulator.launch_app("wc_nowifi")
    simulator.wait_for_exit(timeout=EXIT_TIMEOUT)
    assert simulator.call("get_wifi_state")["status"] == "not_available"


# ── The network it was on, not just "a" network ──────────────────────────────

def _connect_to(sim, ssid: str):
    code = f'picocalc.wifi.connect("{ssid}", "otherpass")'
    stage_lua_app(sim.sd_card_path, "wc_connect", code,
                  files={"app.json": manifest("wc_connect", clocked=False)})
    sim.launch_app("wc_connect")
    sim.wait_for_exit(timeout=EXIT_TIMEOUT)
    deadline = time.monotonic() + 10
    state = sim.call("get_wifi_state")
    while (state["status"] not in UP or state["ssid"] != ssid) \
            and time.monotonic() < deadline:
        time.sleep(0.05)
        state = sim.call("get_wifi_state")
    assert state["status"] in UP and state["ssid"] == ssid, state


def _wait_state(sim, want, timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    state = sim.call("get_wifi_state")
    while state["status"] not in want and time.monotonic() < deadline:
        time.sleep(0.05)
        state = sim.call("get_wifi_state")
    return state


def test_it_rejoins_the_network_it_was_on(simulator):
    """Not the saved one (the default card saves none): an app can join any
    network, and the connection outlives it."""
    _connect_to(simulator, "OtherNet")
    stage_lua_app(simulator.sd_card_path, "wc_rejoin",
                  OFF_LUA.replace("@LEAVE@", "return"),
                  files={"app.json": manifest("wc_rejoin")})
    simulator.launch_app("wc_rejoin")
    assert simulator.wait_for_exit(timeout=EXIT_TIMEOUT)["result"] == "returned"
    state = _wait_state(simulator, UP)
    assert state["status"] in UP and state["ssid"] == "OtherNet", state


def test_a_connect_in_flight_is_started_again(simulator):
    """CONNECTING at the launch: the launch stops it, the exit restarts it
    (the mock's status pin; the firmware-network build has no pin, and is
    simply up here)."""
    simulator.call("set_wifi_state", {"status": "connecting"})
    stage_lua_app(simulator.sd_card_path, "wc_connecting", "",
                  files={"app.json": manifest("wc_connecting")})
    simulator.launch_app("wc_connecting")
    simulator.wait_for_exit(timeout=EXIT_TIMEOUT)
    simulator.call("set_wifi_state", {"status": "connected"})  # the real status
    state = _wait_state(simulator, UP)
    assert state["status"] in UP, state


# ── The boot auto-connect (wifi.c itself: firmware_net) ──────────────────────
#
# A boot auto-connect drops the link after its first time sync unless
# wifi_auto_disconnect is off. A rejoin must not undo that policy in either
# direction: a boot connection still short of its sync goes on to the sync and
# the disconnect; any other connection must not fall off when the rejoin's own
# time sync comes in.

@pytest.fixture
def sntp(monkeypatch):
    server = SntpServer().start()
    monkeypatch.setenv("PICODECK_SIM_NET_SNTP_URL", server.url)
    yield server
    server.stop()


def _write_config(sd, **keys):
    cfg = sd / "system" / "config.json"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text(json.dumps(keys))


def _wait_answer(server: SntpServer, answers: int, timeout: float = 10.0):
    deadline = time.monotonic() + timeout
    while server.answers <= answers and time.monotonic() < deadline:
        time.sleep(0.02)
    assert server.answers > answers, "no time sync happened"


def _run_clocked(sim, sd, name="wc_boot"):
    stage_lua_app(sd, name, OFF_LUA.replace("@LEAVE@", "return"),
                  files={"app.json": manifest(name)})
    sim.launch_app(name)
    out = sim.wait_for_exit(timeout=EXIT_TIMEOUT)
    assert out["result"] == "returned", out


@pytest.mark.firmware_net
def test_boot_connection_short_of_its_sync_goes_on_to_it(sim_factory, test_sd_card,
                                                         sntp):
    """Launched before the first time sync: the rejoin gets the sync the app
    interrupted and then disconnects, as the boot would have. (test_mode=False:
    --test-mode pins the clock, so the firmware would see it as already set.)"""
    sntp.answering = False
    _write_config(test_sd_card, wifi_ssid="HomeNet", wifi_pass="homepass")
    sim = sim_factory(test_sd_card, test_mode=False)
    state = _wait_state(sim, UP)
    assert state["status"] in UP and state["ssid"] == "HomeNet", state

    sntp.answering = True
    answers = sntp.answers
    _run_clocked(sim, test_sd_card)
    _wait_answer(sntp, answers)
    state = _wait_state(sim, ("disconnected",))
    assert state["status"] == "disconnected", state


@pytest.mark.firmware_net
def test_boot_connection_already_disconnected_stays_off(sim_factory, test_sd_card,
                                                        sntp):
    """The usual device: time synced, the link dropped, then a game."""
    _write_config(test_sd_card, wifi_ssid="HomeNet", wifi_pass="homepass")
    sim = sim_factory(test_sd_card, test_mode=False)
    _wait_answer(sntp, 0)
    state = _wait_state(sim, ("disconnected",))
    assert state["status"] == "disconnected", state

    queries = sntp.queries
    _run_clocked(sim, test_sd_card)
    target_state = _wait_state(sim, UP, timeout=2.0)
    assert target_state["status"] == "disconnected", target_state
    assert sntp.queries == queries, "the link came back"


@pytest.mark.firmware_net
def test_a_user_connection_survives_the_rejoins_time_sync(sim_factory, test_sd_card,
                                                          sntp):
    """Joined by hand after the boot's disconnect, with auto-disconnect on (the
    default). Rejoined through a boot-style auto-connect it would be up for a
    moment and then fall off at the sync."""
    _write_config(test_sd_card, wifi_ssid="HomeNet", wifi_pass="homepass")
    sim = sim_factory(test_sd_card, test_mode=False)
    _wait_answer(sntp, 0)
    assert _wait_state(sim, ("disconnected",))["status"] == "disconnected"
    answers = sntp.answers
    _connect_to(sim, "HomeNet")
    _wait_answer(sntp, answers)  # that join's own time sync

    answers = sntp.answers
    _run_clocked(sim, test_sd_card)
    _wait_answer(sntp, answers)
    time.sleep(1.0)  # the deferred disconnect would run on the next polls
    state = sim.call("get_wifi_state")
    assert state["status"] in UP and state["ssid"] == "HomeNet", state


@pytest.mark.firmware_net
def test_the_apps_own_connection_is_not_restarted(sim_factory, test_sd_card, sntp):
    """Every join runs a time sync, so a rejoin over the app's connection would
    show as one more query after the exit."""
    sim = sim_factory(test_sd_card)
    _wait_answer(sntp, 0)
    state = _wait_state(sim, UP)
    assert state["status"] in UP, state
    code = """
picocalc.wifi.connect("AppNet", "apppass")
local t_end = picocalc.sys.getTimeMs() + 1000
while picocalc.sys.getTimeMs() < t_end do picocalc.sys.sleep(20) end
"""
    stage_lua_app(test_sd_card, "wc_appsync", code,
                  files={"app.json": manifest("wc_appsync")})
    answers = sntp.answers
    sim.launch_app("wc_appsync")
    sim.wait_for_exit(timeout=EXIT_TIMEOUT)
    _wait_answer(sntp, answers)  # the app's join
    time.sleep(1.0)
    assert sntp.answers == answers + 1, "joined again after the app's exit"
    state = sim.call("get_wifi_state")
    assert state["status"] in UP and state["ssid"] == "AppNet", state


@pytest.mark.firmware_net
def test_auto_disconnect_off_keeps_the_boot_connection(sim_factory, test_sd_card,
                                                       sntp):
    _write_config(test_sd_card, wifi_ssid="HomeNet", wifi_pass="homepass",
                  wifi_auto_disconnect="0")
    sim = sim_factory(test_sd_card, test_mode=False)
    _wait_answer(sntp, 0)
    state = _wait_state(sim, UP)
    assert state["status"] in UP, state

    answers = sntp.answers
    _run_clocked(sim, test_sd_card)
    _wait_answer(sntp, answers)
    time.sleep(1.0)
    state = sim.call("get_wifi_state")
    assert state["status"] in UP and state["ssid"] == "HomeNet", state
