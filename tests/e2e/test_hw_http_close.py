"""Firmware HTTP/TCP close paths over real WiFi and TLS-capable firmware
(specs/test-audit-2026-09-24.md §5.3 R15, §3.6).

test_network_firmware.py runs these net_fw cases against the firmware's
http.c/tcp.c inside the simulator (make simulator-net). This module runs the
close-related ones on a PicoCalc, where Core 0 and Core 1 are real cores and
the CYW43 link is real:

    PICODECK_HW_HOST_IP=<this host's LAN address> pytest tests/e2e/test_hw_http_close.py \\
        --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

The device must have WiFi configured (wifi_ssid / wifi_pass in
/system/config.json) on a network that reaches this host; the servers
(net_servers.py) bind 0.0.0.0. Without WiFi the tests skip (allow-listed,
hardware marker).
"""

from __future__ import annotations

import json
import os
import socket
import time

import pytest

from helpers import E2E_DIR
from net_servers import BlackholeServer, HttpTestServer, TcpEchoServer, big_body

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(300)]

APP = "net_fw"
APP_ID = "com.test.net_fw"
APP_SRC = E2E_DIR / "net_fw" / "main.lua"

# The close, GC and teardown cases (the *_connect_timeout cases need a port
# that is black over the LAN, which net_servers cannot promise).
CLOSE_CASES = [
    "http_get_ok",
    "http_close_in_request_callback",
    "http_close_during_drip_then_gc",
    "http_drop_reference_then_gc",
    "http_open_close_loop",
    "http_reset_after_headers",
    "http_callbacks_not_reentrant",
    "http_pool_exhaustion",
    "http_close_delimited_body",
    "tcp_close_while_receiving",
    "tcp_close_then_reuse_slot",
    "tcp_read_after_close",
    "tcp_data_survives_the_close",
]


def _host_ip() -> str:
    ip = os.environ.get("PICODECK_HW_HOST_IP")
    if ip:
        return ip
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # UDP connect sends nothing
        return s.getsockname()[0]
    except OSError:
        pytest.skip("no LAN address for this host: set PICODECK_HW_HOST_IP")
    finally:
        s.close()


def _adler(data: bytes) -> int:
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b * 65536 + a


@pytest.fixture(scope="module")
def lan_servers():
    http = HttpTestServer("0.0.0.0").start()
    echo = TcpEchoServer("0.0.0.0").start()
    hole = BlackholeServer("0.0.0.0").start()
    yield {"http": http, "echo": echo, "blackhole": hole,
           "host": _host_ip(), "big_sum": _adler(big_body())}
    http.stop()
    echo.stop()
    hole.stop()


@pytest.fixture
def net(target, lan_servers):
    """net_fw staged on the device (once), WiFi up, servers running."""
    if not getattr(target, "_net_fw_staged", False):
        target.stage_lua_app(APP, APP_SRC.read_text(), requirements=["http"],
                             id=APP_ID)
        target._net_fw_staged = True
    deadline = time.monotonic() + 30
    wifi = target.status().get("wifi")
    while wifi not in ("connected", "online") and time.monotonic() < deadline:
        time.sleep(1)
        wifi = target.status().get("wifi")
    if wifi not in ("connected", "online"):
        pytest.skip(f"device WiFi is {wifi!r}: configure wifi_ssid/wifi_pass")
    return lan_servers


def run_case(target, servers, case: str, timeout: float = 60.0):
    target.write_file(f"/data/{APP_ID}/servers.json", json.dumps({
        "host": servers["host"], "http": servers["http"].port,
        "echo": servers["echo"].port, "blackhole": servers["blackhole"].port,
        "big_sum": servers["big_sum"], "case": case}).encode())
    return target.run_lua_app(APP, timeout=timeout)


@pytest.mark.parametrize("case", CLOSE_CASES)
def test_close_case_on_device(target, net, case):
    run = run_case(target, net, case)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()


def test_tcp_close_reaches_the_server(target, net):
    """sock:close() on the device closes the connection on the wire."""
    run = run_case(target, net, "tcp_echo")
    run.check_case("tcp_echo")
    still_open = net["echo"].wait_all_closed(timeout=5.0)
    assert still_open == 0, f"{still_open} TCP connection(s) left open"


def test_app_exit_closes_open_sockets(target, net):
    """An app that exits with sockets open leaves none open (teardown GC)."""
    accepted = net["echo"].accepted
    run = run_case(target, net, "tcp_leave_open")
    run.check_case("tcp_leave_open")
    assert net["echo"].accepted - accepted == 3
    still_open = net["echo"].wait_all_closed(timeout=5.0)
    assert still_open == 0, f"{still_open} of 3 sockets open after app exit"


def test_http_inflight_across_app_exit(target, net):
    """Exit with requests in flight, relaunch: new requests work."""
    first = run_case(target, net, "http_leave_inflight")
    first.check_case("http_leave_inflight")
    second = run_case(target, net, "http_after_inflight_exit")
    second.check_case("http_after_inflight_exit")
