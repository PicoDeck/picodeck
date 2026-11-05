"""The documented network behaviour, on the default (libcurl) simulator too.

tests/e2e/net_fw holds the cases; test_network_firmware.py runs them all on
the firmware's own network stack (make simulator-net). These are the ones that
state what an app may rely on, so they must hold here as well: a developer
trying the docs on the plain simulator should get what the device does.
"""

from __future__ import annotations


import pytest

from helpers import run_lua_app
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
    "tcp_write_retry_pattern",
    "tcp_connect_timeout_zero_is_off",
]


@pytest.mark.parametrize("case", CASES)
def test_case(servers, simulator, case):  # noqa: F811
    stage(simulator, servers, case)
    run = run_lua_app(simulator, APP, timeout=40.0)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()
