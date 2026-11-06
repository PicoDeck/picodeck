"""The firmware network stack's close, timeout and abort paths
(specs/test-audit-2026-09-24.md §3.6 and R13).

Runs only against a simulator built with SIM_FIRMWARE_NET=ON:

    make simulator-net            # build_sim_net/ (or simulator-net-asan/-tsan)
    PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator \\
        SDL_VIDEODRIVER=dummy pytest tests/e2e/test_network_firmware.py -v

That build runs the firmware's src/drivers/wifi.c, http.c and tcp.c on
Mongoose/POSIX with Core 0 and Core 1 as two host threads (simulator/net/),
so the Core 0/Core 1 lifetime bugs of the code review reproduce here. Against
any other build every test skips (allow-listed through the firmware_net
marker).

Each case runs in its own simulator: the app tests/e2e/net_fw/main.lua runs
the one case named in /data/<APP_ID>/servers.json, next to the ports of the
local servers (net_servers.py). A case that crashes the simulator therefore
fails alone, with the crash or sanitizer report as its evidence.

Cases that fail because of a known bug are strict xfails (KNOWN_BUGS): the
fix turns them into XPASS, which fails the run until the marker is removed.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from helpers import E2E_DIR, case_params, lua_case_names, new_simulator, \
    run_lua_app, stage_lua_app
from net_servers import (BlackholeServer, HttpTestServer, TcpEchoServer,
                         TcpSinkServer, big_body)

pytestmark = pytest.mark.firmware_net

APP = "net_fw"
APP_ID = "com.test.net_fw"
APP_SRC = E2E_DIR / "net_fw" / "main.lua"

# The Lua-side cases, one pytest id each. Two are halves of a Python-driven
# scenario (test_http_inflight_across_app_exit) and one is the app's own
# "unknown case" guard; tcp_leave_open is driven by
# test_tcp_app_exit_closes_sockets, http_read_timeout_hang by
# test_read_timeout_fires_before_headers, pool_churn_then_fill by
# test_pool_churn_releases_every_slot, and the two sink cases by the tests named
# after them, which read what arrived.
_DRIVEN = {"http_leave_inflight", "http_after_inflight_exit",
           "tcp_leave_open", "http_read_timeout_hang", "pool_churn_then_fill",
           "tcp_write_then_close", "tcp_writer_is_held_back", "unknown_case"}
CASES = [n for n in lua_case_names(APP_SRC) if n not in _DRIVEN]

# Cases that fail today because of a known, unfixed bug: {case: reason}
# (strict xfail).  Task 13 fixed every one this suite started with — the
# tcp_free and HTTP timeout-path use-after-frees, the per-object spinlocks,
# the read-timeout arming, callback re-entrancy, keep-alive reuse, chunked
# and close-delimited bodies, binary POST, TCP timeouts and the tcp_connect
# strncpy overlap — so it is empty.
KNOWN_BUGS: dict[str, str] = {}


@pytest.fixture
def servers():
    http = HttpTestServer().start()
    echo = TcpEchoServer().start()
    hole = BlackholeServer().start()
    assert hole.is_black(), "black-hole port completed a handshake"
    yield {"http": http, "echo": echo, "blackhole": hole}
    http.stop()
    echo.stop()
    hole.stop()


_BIG_SUM = None


def _adler(data: bytes) -> int:
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b * 65536 + a


def _big_sum() -> int:
    global _BIG_SUM
    if _BIG_SUM is None:
        _BIG_SUM = _adler(big_body())
    return _BIG_SUM


def stage(sim, servers, case: str):
    """Stage the app and point it at `case` and the servers."""
    sd = Path(sim.sd_card_path)
    stage_lua_app(sd, APP, APP_SRC.read_text(), requirements=["http"],
                  id=APP_ID)
    data = sd / "data" / APP_ID
    data.mkdir(parents=True, exist_ok=True)
    (data / "servers.json").write_text(json.dumps({
        "http": servers["http"].port,
        "echo": servers["echo"].port,
        "blackhole": servers["blackhole"].port,
        "sink": servers["sink"].port if "sink" in servers else 0,
        "big_sum": _big_sum(),
        "case": case,
    }))


def run_case(sim, servers, case: str, timeout: float = 40.0):
    stage(sim, servers, case)
    return run_lua_app(sim, APP, timeout=timeout)


def test_firmware_stack_is_up(simulator):
    """The firmware stack came up: wifi.c joined the stand-in network and
    its connectivity check (to the shim's loopback listener) went ONLINE."""
    deadline = time.monotonic() + 10
    state = simulator.call("get_wifi_state")
    while state["status"] != "online" and time.monotonic() < deadline:
        time.sleep(0.1)
        state = simulator.call("get_wifi_state")
    assert state["status"] == "online", state
    assert state["ip"] == "127.0.0.1", state
    assert state["ssid"] == "SimulatorWiFi", state


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_case(servers, simulator, case):
    run = run_case(simulator, servers, case)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()


def test_post_binary_server_side(servers, simulator):
    """The bytes the server received for the binary POST (the Lua case
    checks the echo; this pins what went on the wire)."""
    run = run_case(simulator, servers, "http_post_binary")
    payload = b"a\0b\0c" + b"\0\1\2\3" * 64 + b"end"
    assert servers["http"].posts, "no POST reached the server\n" + run.describe()
    got = servers["http"].posts[0]
    assert got == payload, (
        f"server got {len(got)} of {len(payload)} bytes: {got[:16]!r}")


def test_read_timeout_fires_before_headers(request, servers, simulator_binary,
                                           test_sd_card, tmp_path):
    """setReadTimeout(1) on a server that accepts and never answers: the
    firmware must give up and hang up within ~1 s (the read timeout is armed
    once the request is sent, state HEADERS). Judged on the server side only
    (/hang records when the client closes), on a simulator this test starts
    itself, so the verdict does not depend on the teardown (the timeout
    path's own safety is http_read_timeout_mid_body's job)."""
    sim = new_simulator(request.config, simulator_binary, test_sd_card,
                        tmp_path / "crash_unwatched.log")
    try:
        stage(sim, servers, "http_read_timeout_hang")
        sim.launch_app(APP)
        deadline = time.monotonic() + 6
        while not servers["http"].hang_closed and time.monotonic() < deadline:
            time.sleep(0.05)
    finally:
        sim.stop()
    closed = servers["http"].hang_closed
    assert closed and closed[0] < 2.5, (
        f"client did not hang up on /hang within 2.5 s of setReadTimeout(1) "
        f"(server saw {closed})")


def test_tcp_close_releases_server_side(servers, simulator):
    """sock:close() must close the connection: the echo server sees EOF."""
    run = run_case(simulator, servers, "tcp_echo")
    run.check_case("tcp_echo")
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, (
        f"{still_open} TCP connection(s) still open 2 s after sock:close() "
        f"(accepted {servers['echo'].accepted})")


def test_write_then_close_delivers(servers, simulator):
    """close() right after a write: all of it arrives, then the FIN. It used to
    drop whatever Core 1 had not sent yet."""
    sink = TcpSinkServer().start()
    try:
        run = run_case(simulator, {**servers, "sink": sink}, "tcp_write_then_close")
        run.check_case("tcp_write_then_close")
        assert sink.wait_closed(5.0), f"no FIN; {len(sink.data)} bytes arrived"
        want = b"".join(b"%09d\n" % i for i in range(1, 6001))
        assert bytes(sink.data) == want, f"{len(sink.data)} of {len(want)} bytes"
    finally:
        sink.stop()


def test_writer_is_held_back(servers, simulator):
    """A peer not reading: tcp_write refuses past TCP_SEND_BACKLOG, the socket
    stays up, and once the peer reads every line arrives, in order."""
    sink = TcpSinkServer(hold_s=2.0, rcvbuf=4096).start()
    try:
        run = run_case(simulator, {**servers, "sink": sink}, "tcp_writer_is_held_back",
                       timeout=60.0)
        run.check_case("tcp_writer_is_held_back")
        assert sink.wait_closed(15.0), f"no FIN; {len(sink.data)} bytes arrived"
        want = b"".join(b"%011d\n" % i + b"x" * 8180 for i in range(1, 401))
        assert bytes(sink.data) == want, f"{len(sink.data)} of {len(want)} bytes"
    finally:
        sink.stop()


def test_tcp_app_exit_closes_sockets(servers, simulator):
    """An app that exits with sockets open must not leave them open:
    teardown GCs the sockets (tcp_free)."""
    run = run_case(simulator, servers, "tcp_leave_open")
    run.check_case("tcp_leave_open")
    assert servers["echo"].accepted == 3
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, (
        f"{still_open} of 3 TCP connection(s) still open 2 s after the app "
        f"exited")


def test_http_inflight_across_app_exit(servers, simulator):
    """§3.6 cross-app: exit with requests in flight, relaunch, and new
    requests work (the next launch's lua_bridge_register runs
    http_close_all; the exit itself GCs the connections)."""
    first = run_case(simulator, servers, "http_leave_inflight")
    first.check_case("http_leave_inflight")
    second = run_case(simulator, servers, "http_after_inflight_exit")
    second.check_case("http_after_inflight_exit")


def test_pool_churn_releases_every_slot(servers, simulator):
    """20 HTTP and 10 TCP open/use/close cycles, then both pools fill to
    exactly their size (pool_churn_then_fill): no slot leaked. And every TCP
    connection of the churn was really closed (the server saw each EOF)."""
    run = run_case(simulator, servers, "pool_churn_then_fill")
    run.check_case("pool_churn_then_fill")
    assert servers["echo"].accepted == 10, servers["echo"].accepted
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, f"{still_open} of 10 churned TCP connections still open"


def test_open_close_churn_leaves_heap_flat(servers, simulator):
    """20 request lifecycles on recycled slots: no crash, and the second
    run of the loop costs no more heap than the first (umm/Lua bytes)."""
    run_case(simulator, servers, "http_open_close_loop").check_case(
        "http_open_close_loop")
    free_1 = simulator.call("get_heap_info")["lua_heap_free_kb"]
    run_case(simulator, servers, "http_open_close_loop").check_case(
        "http_open_close_loop")
    free_2 = simulator.call("get_heap_info")["lua_heap_free_kb"]
    assert free_1 - free_2 < 64, (free_1, free_2)
