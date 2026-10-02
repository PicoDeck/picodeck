"""Bluetooth gamepads on a PicoCalc (issue #26; src/drivers/bt_pad.c).

What a test can check without a pad in pairing mode: the controller comes
up and goes down on request, a search runs to its end, WiFi still moves
data with Bluetooth on and while it searches, Core 1's tick and the main
stack keep their margins, and a 300 MHz clock change with Bluetooth on
neither stalls the CYW43 nor loses the controller. Pairing a real pad is
the hand check in docs/API-Gamepad.md ("Bluetooth controllers").

    PICODECK_HW_HOST_IP=<this host's LAN address> pytest tests/e2e/test_bt_hw.py \\
        --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00 -s

The throughput test needs WiFi configured on a network that reaches this
host (as test_hw_http_close.py); it prints KB/s per mode. Every test leaves
Bluetooth as it found it.
"""

from __future__ import annotations

import json
import os
import re
import socket
import statistics
import time

import pytest

from net_servers import HttpTestServer

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(600)]

TPUT_APP = "bt_tput"
TPUT_ID = "com.test.bt_tput"
CLOCK_APP = "bt_clock"
CLOCK_ID = "com.test.bt_clock"

# GET /big (1 MiB) `n` times, timing each from the request to its last byte;
# results to /data/<id>/tput.json.
TPUT_LUA = r'''
local pc = picocalc
local dir = "/data/" .. APP_ID
local cfg = pc.json.decode(pc.fs.readFile(dir .. "/cfg.json"))
pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "bt_tput " .. cfg.mode, pc.display.WHITE)
pc.display.flush()
-- The launcher connects an "http" app's WiFi if the boot sync left it off.
local w = pc.wifi
local t_wait = pc.sys.getTimeMs()
while w.getStatus() ~= w.STATUS_CONNECTED and w.getStatus() ~= w.STATUS_ONLINE
      and pc.sys.getTimeMs() - t_wait < 20000 do
    pc.sys.sleep(50)
end
local runs = {}
for i = 1, cfg.n do
    local conn = pc.network.http.new(cfg.host, cfg.port, false, "bt")
    conn:setReadBufferSize(16384)
    local got, done, err = 0, false, nil
    local function drain()
        while true do
            local s = conn:read(8192)
            if not s then break end
            got = got + #s
        end
    end
    conn:setRequestCallback(drain)
    conn:setRequestCompleteCallback(function() drain(); done = true end)
    conn:setConnectionClosedCallback(function()
        done = true
        err = conn:getError()
    end)
    local t0 = pc.sys.getTimeMs()
    local ok = conn:get("/big")
    while ok and not done and pc.sys.getTimeMs() - t0 < 30000 do
        pc.sys.sleep(2)
    end
    runs[#runs + 1] = { ms = pc.sys.getTimeMs() - t0, bytes = got,
                        err = err and tostring(err) or "" }
    conn:close()
end
local f = pc.fs.open(dir .. "/tput.json", "w")
pc.fs.write(f, pc.json.encode({ runs = runs }))
pc.fs.close(f)
'''

# Sits at its own clock (app.json system_clock_khz) for `secs` seconds.
CLOCK_LUA = r'''
local pc = picocalc
local t0 = pc.sys.getTimeMs()
pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "bt_clock", pc.display.WHITE)
pc.display.flush()
while pc.sys.getTimeMs() - t0 < 6000 do
    pc.input.update()
    pc.sys.sleep(20)
end
'''


def bt(target, args: str = "") -> dict:
    """The `bt` dev command's reply as a dict (status: key=value pairs)."""
    lines = target.command(("bt " + args).strip(), timeout=5.0)
    for ln in lines:
        m = re.search(r"\[DEV\] (BT[^:]*|Error): ?(.*)", ln)
        if not m:
            continue
        if m.group(1) == "Error":
            raise AssertionError(f"bt {args}: {m.group(2)}")
        out = {"_line": m.group(2)}
        note = re.search(r'note="(.*)"', m.group(2))
        if note:
            out["note"] = note.group(1)
        for k, v in re.findall(r'(\w+)=("[^"]*"|\S+)', m.group(2)):
            out.setdefault(k, v.strip('"'))
        return out
    raise AssertionError(f"no reply to `bt {args}`: {lines}")


def wait_power(target, want: str, timeout: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout
    st = bt(target)
    while st.get("power") != want and time.monotonic() < deadline:
        time.sleep(0.5)
        st = bt(target)
    assert st.get("power") == want, f"Bluetooth power {st.get('power')!r}, " \
        f"want {want!r} ({st.get('note')})"
    return st


@pytest.fixture
def bt_state(target):
    """Bluetooth available; restores the setting the device had."""
    target.ensure_launcher()
    st = bt(target)
    if st.get("available") != "1":
        pytest.skip("Bluetooth is not available on this build/device")
    was_on = st.get("enabled") == "1"
    yield st
    try:
        target.ensure_launcher()
        bt(target, "on" if was_on else "off")
    except Exception:
        pass


def test_power_cycle_and_search(target, bt_state):
    wifi_before = target.status().get("wifi")
    t0 = time.monotonic()
    bt(target, "on")
    st = wait_power(target, "on")
    up_s = time.monotonic() - t0
    print(f"\nBluetooth on in {up_s:.1f} s: {st.get('note')}")
    assert st["radio_in_use"] == "1"

    bt(target, "scan")
    deadline = time.monotonic() + 45
    while bt(target).get("scanning") == "1" and time.monotonic() < deadline:
        time.sleep(1)
    st = bt(target)
    assert st.get("scanning") == "0", "the search never ended"
    found = bt(target, "found")
    print(f"search: {found['_line']}")

    stack = target.stack() or {}
    print(f"stack after the search: {stack}")
    if stack.get("msp_peak"):
        assert int(stack["msp_peak"]) < 3584, stack  # 0.5 KB of the 4 KB left

    bt(target, "off")
    st = wait_power(target, "off")
    assert st["radio_in_use"] == "0"
    assert st["bus_errors"] == "0", st
    # WiFi was not disturbed by the controller going up and down: a link
    # that was up is still up (the boot's time sync may have left it off
    # on purpose; then it must not have failed either).
    wifi_after = target.status().get("wifi")
    print(f"WiFi before {wifi_before!r}, after {wifi_after!r}")
    if wifi_before in ("connected", "online"):
        assert wifi_after in ("connected", "online"), wifi_after
    else:
        assert wifi_after != "failed", wifi_after


def _host_ip() -> str:
    ip = os.environ.get("PICODECK_HW_HOST_IP")
    if ip:
        return ip
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        pytest.skip("no LAN address for this host: set PICODECK_HW_HOST_IP")
    finally:
        s.close()


def _audiostat(target) -> dict:
    for ln in target.command("audiostat", timeout=3.0):
        if "Audio:" in ln:
            return dict(re.findall(r"(\w+)=(-?\d+)", ln))
    return {}


def _run_tput(target, http, mode: str, n: int = 3) -> tuple:
    target.write_file(f"/data/{TPUT_ID}/cfg.json", json.dumps({
        "host": _host_ip(), "port": http.port, "n": n, "mode": mode}).encode())
    target.delete_file(f"/data/{TPUT_ID}/tput.json")
    target.command("audiostat reset", timeout=3.0)
    target.launch_app(TPUT_APP)
    if mode == "scan":
        bt(target, "scan")
    out = target.wait_for_exit(timeout=180, poll_s=3.0)
    assert out.get("result") == "returned", out
    tick = _audiostat(target)
    runs = json.loads(target.read_file(f"/data/{TPUT_ID}/tput.json"))["runs"]
    for r in runs:
        assert r["bytes"] == 1048576, f"{mode}: {r}"
    kbs = [r["bytes"] / 1024 / (r["ms"] / 1000) for r in runs]
    return statistics.median(kbs), kbs, tick


def test_wifi_throughput_with_bluetooth(target, bt_state):
    deadline = time.monotonic() + 30
    while target.status().get("wifi") not in ("connected", "online"):
        if time.monotonic() > deadline:
            pytest.skip("device WiFi is not up: configure wifi_ssid/wifi_pass")
        time.sleep(1)
    http = HttpTestServer("0.0.0.0").start()
    try:
        # Before any Bluetooth change: pushing a new app reboots the device.
        target.stage_lua_app(TPUT_APP, TPUT_LUA, requirements=["http"],
                             id=TPUT_ID)
        results = {}
        bt(target, "off")
        wait_power(target, "off")
        results["off"] = _run_tput(target, http, "off")
        bt(target, "on")
        wait_power(target, "on")
        results["on"] = _run_tput(target, http, "on")
        results["scan"] = _run_tput(target, http, "scan")
        print()
        for mode, (med, kbs, tick) in results.items():
            print(f"{mode:5s} median {med:6.1f} KB/s  runs "
                  f"{', '.join(f'{k:.0f}' for k in kbs)}  core1 tick "
                  f"max_us={tick.get('tick_max_us')} over={tick.get('tick_over')}"
                  f" missed={tick.get('tick_missed')}")
        off = results["off"][0]
        assert results["on"][0] >= 0.7 * off, results
        assert results["scan"][0] >= 0.25 * off, results
    finally:
        http.stop()
        target.ensure_launcher()
        target.command(f"rm /apps/{TPUT_APP}", timeout=10.0)
        target.command(f"rm /data/{TPUT_ID}", timeout=10.0)


def test_clock_change_keeps_bluetooth(target, bt_state):
    """An app at 300 MHz with Bluetooth on: the radio is not paused (BT
    needs the driver), the CYW43's bus is retuned for the clock, and nothing
    logs the sleep-handshake failure (the ~68 ms Core 0 stall)."""
    # The manifest with system_clock_khz replaces the helper's (staged
    # together). Staged first: pushing a new app reboots the device (the
    # launcher's app list is read at boot), and Bluetooth comes up after.
    manifest = {"id": CLOCK_ID, "name": CLOCK_APP, "version": "1.0",
                "description": "E2E: 300 MHz with Bluetooth on",
                "author": "PicoDeck E2E", "requirements": [],
                "system_clock_khz": 300000}
    try:
        target.stage_lua_app(CLOCK_APP, CLOCK_LUA, id=CLOCK_ID,
                             files={"app.json": json.dumps(manifest)})
        bt(target, "on")
        wait_power(target, "on")
        mark = target.log_cursor()
        target.launch_app(CLOCK_APP)
        out = target.wait_for_exit(timeout=30, poll_s=2.0)
        assert out.get("result") == "returned", out
        log = "\n".join(target.get_log_lines(mark))
        assert "Changing clock: 200 -> 300" in log, log[-2000:]
        assert "kso_set" not in log, log[-3000:]
        assert "state machine not found" not in log, log[-3000:]
        st = bt(target)
        assert st["power"] == "on", st
        assert st["radio_in_use"] == "1", st
        assert st["bus_errors"] == "0", st
        # The controller still answers: a search starts and ends.
        bt(target, "scan")
        deadline = time.monotonic() + 45
        while bt(target).get("scanning") == "1" and time.monotonic() < deadline:
            time.sleep(1)
        assert bt(target).get("scanning") == "0"
    finally:
        target.ensure_launcher()
        target.command(f"rm /apps/{CLOCK_APP}", timeout=10.0)
        target.command(f"rm /data/{CLOCK_ID}", timeout=10.0)
