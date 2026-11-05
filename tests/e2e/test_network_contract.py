"""The documented network behaviour, on the default (libcurl) simulator too.

tests/e2e/net_fw holds the cases; test_network_firmware.py runs them all on
the firmware's own network stack (make simulator-net). These are the ones that
state what an app may rely on, so they must hold here as well: a developer
trying the docs on the plain simulator should get what the device does.

Plus what a Lua case cannot do by itself: an exit while a blocking wait runs,
and a download onto a card that stops taking writes.
"""

from __future__ import annotations

import stat
import time

import pytest

from helpers import new_simulator, run_lua_app, stage_lua_app
from test_network_firmware import APP, servers, stage  # noqa: F401 (fixture)

CASES = [
    "http_get_ok",
    "http_get_big",
    "http_post_binary",
    "http_keepalive_reuse",
    "http_chunked_body",
    "http_close_delimited_body",
    "http_close_delimited_small_ring",
    "http_request_while_busy_refused",
    "http_read_timeout_hang",
    "http_read_timeout_mid_body",
    "http_connect_timeout",
    "http_range_next_request",
    "http_range_open_ended",
    "http_read_timeout_fraction",
    "http_read_timeout_zero_is_off",
    "http_connect_timeout_fraction",
    "http_connect_timeout_zero_is_off",
    "http_body_cut_short_fails",
    "http_big_body_default_ring",
    "http_headers_too_long",
    "tcp_echo",
    "tcp_read_timeout",
    "tcp_connect_timeout",
    "tcp_connect_twice_refused",
    "tcp_read_timeout_fraction",
    "tcp_waits_run_callbacks",
    "tcp_wait_survives_close_in_callback",
    "tcp_wait_data_with_read_callback",
    "tcp_write_retry_pattern",
    "tcp_connect_timeout_zero_is_off",
    "download_lib_big",
    "download_replaces_existing",
    "download_keeps_existing_on_failure",
    "download_failure_leaves_no_file",
    "download_relative_redirects",
    "download_reports_the_real_error",
    "download_progress_totals",
]


@pytest.mark.parametrize("case", CASES)
def test_case(servers, simulator, case):  # noqa: F811
    stage(simulator, servers, case)
    run = run_lua_app(simulator, APP, timeout=40.0)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()


WAIT_APP = """
local s = picocalc.tcp.new("127.0.0.1", {port}, false)
s:setConnectTimeout(30)
assert(s:connect())
picocalc.sys.log("WAITING")
local got = s:{call}
picocalc.sys.log("RETURNED " .. tostring(got))
"""


@pytest.mark.parametrize("call", ["waitConnected(20)", "waitConnected()"])
def test_exit_ends_a_wait_for_a_connection(servers, simulator, call):  # noqa: F811
    """The wait runs the service pass: the exit request is honoured at once
    (and, on the device, the same pass feeds the watchdog: a 20 s wait used to
    outlast its 10 s)."""
    port = servers["blackhole"].port
    stage_lua_app(simulator.sd_card_path, "wait_exit",
                  WAIT_APP.format(port=port, call=call), requirements=["http"])
    simulator.launch_app("wait_exit")
    simulator.wait_for_log("WAITING", timeout=10)
    t0 = time.monotonic()
    simulator.exit_app()
    out = simulator.wait_for_exit(timeout=5)
    assert time.monotonic() - t0 < 3, "the exit waited for the wait"
    assert out.get("result") in ("returned", "exit_sentinel"), out


def test_exit_ends_a_wait_for_data(servers, simulator):  # noqa: F811
    port = servers["echo"].port
    stage_lua_app(simulator.sd_card_path, "wait_exit",
                  WAIT_APP.format(port=port, call="waitConnected(5)")
                  + 's:waitData(20)\npicocalc.sys.log("OVER")\n',
                  requirements=["http"])
    simulator.launch_app("wait_exit")
    simulator.wait_for_log("RETURNED", timeout=10)
    t0 = time.monotonic()
    simulator.exit_app()
    out = simulator.wait_for_exit(timeout=5)
    assert time.monotonic() - t0 < 3, "the exit waited for the wait"
    assert out.get("result") in ("returned", "exit_sentinel"), out


REFUSED_APP = """
local T = picocalc.sys.loadlib("picotest")
local dl = picocalc.sys.loadlib("download")
T.case("download_refused_request_returns_at_once", function()
    -- The firmware-net build's real WiFi layer takes no injection: nothing refuses.
    if picocalc.wifi.isAvailable() then return end
    local t0 = picocalc.sys.getTimeMs()
    local ok, err = dl.toFile("http://127.0.0.1:{port}/ok", picocalc.fs.appPath("x.bin"),
                              {{ timeoutMs = 20000 }})
    T.eq(ok, false, "returned true")
    T.ok(err and err:find("WiFi", 1, true), "reason: " .. tostring(err))
    T.ok(picocalc.sys.getTimeMs() - t0 < 5000, "waited out the timeout")
end)
T.done()
"""


def test_download_refused_request_returns_at_once(servers, simulator):  # noqa: F811
    stage_lua_app(simulator.sd_card_path, "dl_refused",
                  REFUSED_APP.format(port=servers["http"].port),
                  requirements=["http"])
    simulator.call("set_wifi_state", {"status": "not_available"})
    run = run_lua_app(simulator, "dl_refused", timeout=40)
    run.check_case("download_refused_request_returns_at_once")


FULL_APP = """
local T = picocalc.sys.loadlib("picotest")
local fs = picocalc.fs
local dl = picocalc.sys.loadlib("download")
T.case("download_on_a_full_card", function()
    local dest = fs.appPath("big.bin")
    local f = T.ok(fs.open(dest, "w"), "open")
    f:write("precious")
    f:close()
    local ok, err = dl.toFile("http://127.0.0.1:{port}/big", dest, {{ timeoutMs = 20000 }})
    T.eq(ok, false, "toFile returned true on a card that stopped taking writes")
    T.ok(err and err:find("write", 1, true), "reason: " .. tostring(err))
    T.eq(fs.readFile(dest), "precious", "the old file is kept")
    T.eq(fs.exists(dest .. ".part"), false, "no .part left")
end)
T.done()
"""


def test_download_short_write_fails(request, servers, simulator_binary,  # noqa: F811
                                    test_sd_card, tmp_path):
    """A card that stops taking writes: fs.write returns short (no error), and
    the file must not be reported complete. The host's file-size limit stands
    in for a full card (ulimit -f is in KiB; SIGXFSZ ignored, so writes return
    short, as FatFS does on a full volume)."""
    wrapper = tmp_path / "limited_sim.sh"
    wrapper.write_text("#!/bin/bash\ntrap '' XFSZ\nulimit -f 200\n"
                       f'exec "{simulator_binary}" "$@"\n')
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
    sim = new_simulator(request.config, wrapper, test_sd_card,
                        tmp_path / "crash.log")
    try:
        stage_lua_app(sim.sd_card_path, "dl_full",
                      FULL_APP.format(port=servers["http"].port),
                      requirements=["http"])
        run = run_lua_app(sim, "dl_full", timeout=40)
        run.check_case("download_on_a_full_card")
    finally:
        sim.stop()
