"""What can only be shown on a PicoCalc: the documented network calls on real
WiFi, and blocking calls against the real 10 s watchdog. Not run by the
simulator suite (hardware marker):

    PICODECK_HW_HOST_IP=<this host's LAN address> pytest tests/e2e/test_hw_net_contract.py \\
        --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

The device needs WiFi configured (wifi_ssid / wifi_pass in /system/config.json) on
a network that reaches this host. The watchdog tests wait until 70 s after boot:
Core 1 relays the watchdog for 60 s after boot, which would hide a starved feed.
"""

from __future__ import annotations

import os
import time

import pytest

from test_hw_http_close import lan_servers, net, run_case  # noqa: F401 (fixtures)

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(300)]

CASES = [
    "http_range_next_request",
    "http_range_open_ended",
    "http_read_timeout_fraction",
    "http_read_timeout_zero_is_off",
    "http_body_cut_short_fails",
    "http_big_body_default_ring",
    "tcp_connect_twice_refused",
    "tcp_read_timeout_fraction",
    "tcp_waits_run_callbacks",
    "tcp_wait_data_with_read_callback",
    "tcp_write_retry_pattern",
    "download_replaces_existing",
    "download_keeps_existing_on_failure",
    "download_relative_redirects",
    "download_reports_the_real_error",
    "download_progress_totals",
    # 1 MiB from the LAN to the SD card at 1 MHz (~80 KB/s), several times
    # slower than the server: the receive window has to hold it back
    # (MG_TCPIP_RCVBUF). Without it the transfer failed once the 256 KB ring
    # was full, with "receive buffer overflow (the app is not reading)" here
    # (plain HTTP) and "partial: oom" or "TLS recv error" over HTTPS: the
    # store's large installs (#78).
    "download_lib_big",
]


@pytest.mark.parametrize("case", CASES)
def test_case(target, net, case):  # noqa: F811
    run = run_case(target, net, case)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()


def _past_the_relay(target):
    up = target.status()["uptime_ms"]
    if up < 70000:
        time.sleep((70000 - up) / 1000)


# A connect that is never answered, for longer than the watchdog's 10 s. Any
# address the network drops silently will do: one that gets an ICMP unreachable
# ends the wait early and fails the test (set PICODECK_HW_BLACKHOLE_IP). The
# device's stack gives up on it after 15 s, whatever setConnectTimeout asks.
WAIT_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("wait_connected_outlasts_the_watchdog", function()
    local c = picocalc.tcp.new("BLACKHOLE_IP", 80, false)
    c:setConnectTimeout(30)
    T.ok(c:connect(), "connect")
    local t0 = picocalc.sys.getTimeMs()
    T.eq(c:waitConnected(20), false, "connected to a black hole")
    T.ok(picocalc.sys.getTimeMs() - t0 >= 14000, "the wait ended early")
    c:close()
end)
T.done()
"""


def test_wait_connected_outlasts_the_watchdog(target, net):  # noqa: F811
    _past_the_relay(target)
    ip = os.environ.get("PICODECK_HW_BLACKHOLE_IP", "10.255.255.1")
    target.stage_lua_app("hw_wait", WAIT_APP.replace("BLACKHOLE_IP", ip),
                         requirements=["http"], id="com.test.hw_wait")
    run = target.run_lua_app("hw_wait", timeout=60)
    run.assert_clean_exit()  # a watchdog reset reports device_rebooted
    run.assert_all_passed(["wait_connected_outlasts_the_watchdog"])


# 2 MB through each single C call under slow mode (about 100 KB/s), so each runs
# well past 10 s.
BIG_FILE_APP = """
local T = picocalc.sys.loadlib("picotest")
local fs = picocalc.fs
local MB = 1024 * 1024
local src, dst = fs.appPath("big.bin"), fs.appPath("big2.bin")
T.case("large_write_slow_mode", function()
    fs.setSlowMode(true)
    local f = T.ok(fs.open(src, "w"), "open")
    local s = string.rep("x", 2 * MB)
    T.eq(f:write(s), #s, "write")
    f:close()
    fs.setSlowMode(false)
    T.eq(fs.size(src), 2 * MB, "size")
end)
T.case("large_read_slow_mode", function()
    fs.setSlowMode(true)
    local f = T.ok(fs.open(src, "r"), "open")
    local s = f:read(2 * MB)
    f:close()
    fs.setSlowMode(false)
    T.eq(s and #s, 2 * MB, "read")
end)
T.case("large_copy_no_progress_slow_mode", function()
    fs.setSlowMode(true)
    T.ok(fs.copy(src, dst), "copy")
    fs.setSlowMode(false)
    T.eq(fs.size(dst), 2 * MB, "size")
end)
fs.delete(src)
fs.delete(dst)
T.done()
"""


def test_large_sd_calls_outlast_the_watchdog(target):
    _past_the_relay(target)
    target.stage_lua_app("hw_bigfile", BIG_FILE_APP, id="com.test.hw_bigfile")
    run = target.run_lua_app("hw_bigfile", timeout=240)
    run.assert_clean_exit()  # a watchdog reset reports device_rebooted
    run.assert_all_passed(["large_write_slow_mode", "large_read_slow_mode",
                           "large_copy_no_progress_slow_mode"])
