"""The documented network behaviour, on the default (libcurl) simulator too.

tests/e2e/net_fw holds the cases; test_network_firmware.py runs them all on
the firmware's own network stack (make simulator-net). These are the ones that
state what an app may rely on, so they must hold here as well: a developer
trying the docs on the plain simulator should get what the device does.

Plus what a Lua case cannot do by itself: an exit while a blocking wait runs.
"""

from __future__ import annotations

import time


import pytest

from helpers import run_lua_app, stage_lua_app
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
