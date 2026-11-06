"""TCP resend and socket close on a PicoCalc (src/drivers/CLAUDE.md, "Closing"):
what the host tests cannot show, on the real radio, heap and Core 1. The stack
itself is host-tested (tests/unit/test_mg_tcp_resend.c, test_mg_tls_resend.c,
test_mg_linux_peer.py); test_hw_http_close.py is the device test for the FIN.

    PICODECK_HW_HOST_IP=<this host's LAN address> pytest tests/e2e/test_hw_tcp_resend.py \\
        -s --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

-s shows the figures, which the assertions only bound. test_dead_peer needs this
host to drop the device's frames and skips unless it is given the commands:

    PICODECK_HW_DROP_CMD='sudo iptables -I INPUT -p tcp --dport {port} -j DROP'
    PICODECK_HW_UNDROP_CMD='sudo iptables -D INPUT -p tcp --dport {port} -j DROP'
"""

from __future__ import annotations

import json
import os
import re
import socket
import ssl
import subprocess
import threading
import time

import pytest

from hw_target import HW_APPS_DIR
from net_servers import TcpEchoServer, TcpSinkServer
from test_hw_http_close import _host_ip
from test_tcp_tls import CERT, TlsEchoServer

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(900)]

APP = "tcp_close_probe"
APP_ID = "com.test.tcp_close_probe"
DIR = f"/data/{APP_ID}"

# A closed socket's slack on the Lua heap across a run, once every close has
# finished: fragmentation and Lua's own garbage, not a TLS context (~30 KB each).
HEAP_SLACK = 24 * 1024
POOL_SLACK = 2 * 1024


class TlsSilentServer:
    """Completes each TLS handshake, then reads nothing and closes nothing: the
    worst peer for a closing socket short of a dead one."""

    def __init__(self, host: str = "0.0.0.0"):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(CERT))
        self.sock = socket.create_server((host, 0))
        self.port = self.sock.getsockname()[1]
        self.kept = []

    def start(self):
        threading.Thread(target=self._serve, daemon=True).start()
        return self

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
                self.kept.append(self.ctx.wrap_socket(conn, server_side=True))
            except ssl.SSLError:
                pass
            except OSError:
                return

    def stop(self):
        self.sock.close()
        for s in self.kept:
            s.close()


@pytest.fixture
def probe(target):
    deadline = time.monotonic() + 30
    wifi = target.status().get("wifi")
    while wifi not in ("connected", "online") and time.monotonic() < deadline:
        time.sleep(1)
        wifi = target.status().get("wifi")
    if wifi not in ("connected", "online"):
        pytest.skip(f"device WiFi is {wifi!r}: configure wifi_ssid/wifi_pass")
    target.push_app(HW_APPS_DIR / APP, APP)
    return target


def netstat(target) -> dict:
    for line in target.command("netstat", 5.0):
        m = re.search(r"Net: pool_free=(\d+) pool_largest=(\d+) conns=(\d+) "
                      r"closing=(\d+) resent=(\d+)", line)
        if m:
            return dict(zip(("pool_free", "pool_largest", "conns", "closing",
                             "resent"), map(int, m.groups())))
    raise AssertionError("no answer to netstat: is this firmware from this branch?")


def run_probe(target, timeout, **cfg) -> dict:
    for name in ("probe_out.json", "probe_ready", "probe_go", "test_results.json"):
        target.delete_file(f"{DIR}/{name}")
    target.write_file(f"{DIR}/probe_in.json", json.dumps({"host": _host_ip(), **cfg}).encode())
    target.launch_app(APP)
    target.wait_for_results(APP_ID, timeout=timeout)
    return json.loads(target.read_file(f"{DIR}/probe_out.json"))


def settle(target, seconds=12.0) -> dict:
    """Past NET_CLOSE_LIMIT_MS: nothing should be left closing."""
    deadline = time.monotonic() + seconds
    st = netstat(target)
    while st["closing"] and time.monotonic() < deadline:
        time.sleep(1)
        st = netstat(target)
    return st


def check_cycles(target, out, before, label):
    cycles = out["cycles"]
    bad = [c for c in cycles if not c["ok"]]
    print(f"\n{label}: {len(cycles)} cycles, {len(bad)} failed, "
          f"median {sorted(c['ms'] for c in cycles)[len(cycles) // 2]} ms each")
    print(f"  PSRAM free {cycles[0]['free']} .. {cycles[-1]['free']}, "
          f"lowest {min(c['free'] for c in cycles)}; "
          f"largest block {cycles[0]['largest']} .. {cycles[-1]['largest']}")
    after = settle(target)
    print(f"  pool before {before}\n  pool after  {after}")
    stack = " ".join(target.command("stack", 5.0))
    print(f"  {stack}")
    peak = re.search(r"core1_peak=(\d+) core1_size=(\d+)", stack)
    assert peak and int(peak.group(1)) < int(peak.group(2)) - 256, stack
    assert not bad, bad[:5]
    assert after["closing"] == 0, after
    assert before["pool_free"] - after["pool_free"] <= POOL_SLACK, (before, after)
    assert cycles[0]["free"] - cycles[-1]["free"] <= HEAP_SLACK, cycles[:1] + cycles[-1:]
    assert cycles[0]["largest"] - cycles[-1]["largest"] <= HEAP_SLACK


def test_small_writes_never_stall(probe):
    """The stall this exists to fix: 25 of 90 runs before it, on the perf branch."""
    echo = TcpEchoServer("0.0.0.0").start()
    try:
        before = netstat(probe)
        out = run_probe(probe, 800, mode="small_writes", port=echo.port, runs=90)
        after = netstat(probe)
    finally:
        echo.stop()
    print(f"\nsmall writes: {out}; TCP resent {after['resent'] - before['resent']} segments")
    assert not out.get("connect_failed"), out
    assert out["stalled"] == 0, out


def test_tls_open_close_gives_everything_back(probe):
    """The worry that kept the FIN out: TLS sockets opened and closed in a row."""
    tls = TlsEchoServer(CERT, "0.0.0.0").start()
    try:
        before = settle(probe)
        out = run_probe(probe, 600, mode="tls_cycles", port=tls.port, cycles=40, echo=True)
    finally:
        tls.stop()
    check_cycles(probe, out, before, "TLS, echoing server")


def test_tls_close_to_a_silent_peer_gives_everything_back(probe):
    """A peer that never answers the FIN: each close waits a second, holding a
    few hundred bytes, and none of them may stop the next connection."""
    silent = TlsSilentServer().start()
    try:
        before = settle(probe)
        lows = []
        stop = threading.Event()

        def sample():
            while not stop.wait(2.0):
                try:
                    lows.append(netstat(probe))
                except Exception:  # the probe's own commands take the port too
                    pass

        t = threading.Thread(target=sample, daemon=True)
        t.start()
        out = run_probe(probe, 600, mode="tls_cycles", port=silent.port, cycles=30)
        stop.set()
        t.join(5)
    finally:
        silent.stop()
    if lows:
        print(f"\nduring the run: pool low {min(s['pool_free'] for s in lows)}, "
              f"largest low {min(s['pool_largest'] for s in lows)}, "
              f"most closing {max(s['closing'] for s in lows)}")
    check_cycles(probe, out, before, "TLS, silent server")


def test_write_then_close_delivers(probe):
    """A close right after a write: all of it arrives, then the FIN, at once."""
    sink = TcpSinkServer("0.0.0.0").start()
    try:
        t0 = time.monotonic()
        out = run_probe(probe, 60, mode="write_close", port=sink.port, bytes=60000)
        assert sink.wait_closed(15.0), f"no FIN; {len(sink.data)} bytes arrived"
        print(f"\nwrite then close: FIN {sink.closed_at - t0:.2f} s from launch")
    finally:
        sink.stop()
    assert bytes(sink.data) == b"".join(b"%09d\n" % i for i in range(1, out["sent"] // 10 + 1))


def test_upload_rate_and_core1_tick(probe):
    """What the in-flight cap costs: compare the figure with a build before this
    one (no cap; 1 MiB to a LAN host). It should keep up with the radio."""
    sink = TcpSinkServer("0.0.0.0").start()
    try:
        before = netstat(probe)
        probe.command("audiostat reset", 5.0)
        t0 = time.monotonic()
        out = run_probe(probe, 300, mode="upload", port=sink.port, bytes=1 << 20)
        assert sink.wait_closed(30.0), f"no FIN; {len(sink.data)} bytes arrived"
        secs = sink.closed_at - t0
        tick = [ln for ln in probe.command("audiostat", 5.0) if "tick_max_us" in ln]
        after = netstat(probe)
    finally:
        sink.stop()
    print(f"\nupload: {out}\n  {len(sink.data)} bytes arrived, about "
          f"{len(sink.data) / 1024 / secs:.0f} KB/s from launch to FIN; resent "
          f"{after['resent'] - before['resent']}\n  {tick}")
    assert not out.get("err"), out
    assert len(sink.data) == out["sent"], out


def test_dead_peer(probe):
    """A peer that stops answering ends with an error about 3 minutes after its
    last frame: the keep-alive's fourth miss."""
    drop, undrop = (os.environ.get("PICODECK_HW_DROP_CMD"),
                    os.environ.get("PICODECK_HW_UNDROP_CMD"))
    if not (drop and undrop):
        pytest.skip("set PICODECK_HW_DROP_CMD and PICODECK_HW_UNDROP_CMD")
    sink = TcpSinkServer("0.0.0.0").start()
    fmt = {"port": sink.port}
    dropped = False
    try:
        for name in ("probe_out.json", "probe_ready", "probe_go", "test_results.json"):
            probe.delete_file(f"{DIR}/{name}")
        probe.write_file(f"{DIR}/probe_in.json", json.dumps({
            "host": _host_ip(), "mode": "dead_peer", "port": sink.port,
            "limit_s": 240}).encode())
        probe.launch_app(APP)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                probe.read_file(f"{DIR}/probe_ready")
                break
            except Exception:
                time.sleep(0.5)
        subprocess.run(drop.format(**fmt), shell=True, check=True)
        dropped = True
        probe.write_file(f"{DIR}/probe_go", b"1")
        probe.wait_for_results(APP_ID, timeout=300)
        out = json.loads(probe.read_file(f"{DIR}/probe_out.json"))
    finally:
        if dropped:
            subprocess.run(undrop.format(**fmt), shell=True, check=True)
        sink.stop()
    print(f"\ndead peer: {out}")
    assert not out["connected_at_end"], out
    assert "keepalive" in (out["err"] or ""), out
    assert 120_000 <= out["after_ms"] <= 200_000, out
