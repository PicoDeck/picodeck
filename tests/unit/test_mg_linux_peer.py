"""Mongoose's built-in TCP (the device's stack) against this machine's real TCP.

test_mg_tcp_resend.c's peer is a script; this one is Linux, so what the stack's
resends and FINs look like to a real peer is checked, with TCP_INFO as on the
device tests. mg_tap_client.c runs the stack on a TAP device inside a user and
network namespace, so no privileges are needed; where a machine does not allow
those the tests skip. Real time: a run takes seconds.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TAP = "tap9"
MY_IP = "10.9.0.2"
PEER_IP = "10.9.0.1"
PORT = 5599
# struct tcp_info: 64-bit counters from offset 104 (include/uapi/linux/tcp.h).
TCPI_TOTAL_RETRANS = 100
TCPI_RCV_OOOPACK = 224
IDLE_S = 6.0  # no bytes for this long: the stream has stalled


def pattern(n: int) -> bytes:
    return bytes((i * 31 + (i >> 8)) & 0xFF for i in range(n))


def inner(binary: str, mode: str, n: int, drop_every: int, resend: int,
          hold_s: float, keep_open_s: float) -> None:
    """In the namespace: bring up the TAP, serve one connection, run the client."""
    for args in (["ip", "tuntap", "add", TAP, "mode", "tap"],
                 ["ip", "addr", "add", f"{PEER_IP}/24", "dev", TAP],
                 ["ip", "link", "set", TAP, "up"]):
        subprocess.run(args, check=True, capture_output=True)
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((PEER_IP, PORT))
    lsock.listen(1)
    lsock.settimeout(30)
    got = {"data": bytearray(), "eof": False, "reset": False, "ooo": 0, "retrans": 0}

    def serve() -> None:
        conn, _ = lsock.accept()
        conn.settimeout(0.2)
        time.sleep(hold_s)
        idle_until = time.monotonic() + IDLE_S
        try:
            while time.monotonic() < idle_until:
                info = conn.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 256)
                got["ooo"] = struct.unpack_from("I", info, TCPI_RCV_OOOPACK)[0]
                got["retrans"] = struct.unpack_from("I", info, TCPI_TOTAL_RETRANS)[0]
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    got["eof"] = True
                    break
                got["data"] += chunk
                idle_until = time.monotonic() + IDLE_S
        except ConnectionResetError:
            got["reset"] = True
        time.sleep(keep_open_s)
        conn.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    run = subprocess.run([binary, TAP, MY_IP, PEER_IP, str(PORT), mode, str(n),
                          str(drop_every), str(resend)],
                         capture_output=True, text=True, timeout=120)
    t.join(5)
    out = json.loads(run.stdout) if run.returncode == 0 else {"rc": run.returncode}
    out.update(received=len(got["data"]), intact=bytes(got["data"]) == pattern(n),
               eof=got["eof"], reset=got["reset"], ooo=got["ooo"], retrans=got["retrans"])
    print(json.dumps(out))


@pytest.fixture(scope="module")
def client(tmp_path_factory):
    if sys.platform != "linux" or not shutil.which("ip") or not shutil.which("unshare"):
        pytest.skip("needs Linux, iproute2 and unshare")
    if subprocess.run(["unshare", "-Urn", "ip", "tuntap", "add", TAP, "mode", "tap"],
                      capture_output=True).returncode != 0:
        pytest.skip("no user namespaces or TAP devices here")
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    out = tmp_path_factory.mktemp("mg_tap") / "mg_tap_client"
    subprocess.run([cc, "-std=gnu11", "-O1", "-w",
                    "-DMG_ENABLE_TCPIP=1", "-DMG_ENABLE_SOCKET=0", "-DMG_IO_SIZE=8192",
                    "-DMG_TLS=MG_TLS_NONE",
                    f"-I{ROOT}/third_party/mongoose", f"-I{ROOT}/src/drivers",
                    f"{ROOT}/tests/unit/mg_tap_client.c", f"{ROOT}/src/drivers/net_close.c",
                    f"{ROOT}/third_party/mongoose/mongoose.c", "-o", str(out)], check=True)
    return str(out)


def run(client, mode, n, drop_every=0, resend=1, hold_s=0.0, keep_open_s=0.0):
    r = subprocess.run(["unshare", "-Urn", sys.executable, __file__, client, mode, str(n),
                        str(drop_every), str(resend), str(hold_s), str(keep_open_s)],
                       capture_output=True, text=True, timeout=150)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout.strip().splitlines()[-1])


def test_a_clean_close_reaches_the_peer(client):
    r = run(client, "small", 200)
    assert r["connected"] and r["intact"], r
    assert r["eof"] and not r["reset"], r
    assert r["ended"] and r["err"] == "", r
    assert r["resent"] == 0, r
    assert r["close_ms"] < 500, r  # the peer's FIN ended it, not the timer


def test_without_the_resend_the_peer_stalls_as_on_the_device(client):
    """The symptom measured on the device: out-of-order packets held, in-order
    bytes stopped, no retransmission and no error on either side."""
    r = run(client, "small", 200, drop_every=30, resend=0)
    assert r["connected"], r
    assert r["lost"] > 0 and r["resent"] == 0, r
    assert r["received"] < 200, r
    assert r["ooo"] > 0 and r["retrans"] == 0, r
    assert not r["reset"], r


def test_small_writes_survive_lost_frames(client):
    r = run(client, "small", 200, drop_every=30)
    assert r["lost"] > 0 and r["resent"] > 0, r
    assert r["ooo"] > 0, r
    assert r["intact"] and r["eof"] and not r["reset"], r
    assert r["ended"] and r["err"] == "", r


def test_a_stream_survives_lost_frames(client):
    r = run(client, "big", 400_000, drop_every=100)
    assert r["lost"] > 0 and r["resent"] > 0, r
    assert r["intact"] and r["eof"] and not r["reset"], r
    assert r["ended"] and r["err"] == "", r


def test_a_peer_not_reading_shuts_the_window_and_then_gets_everything(client):
    r = run(client, "big", 300_000, hold_s=3.0)
    assert r["intact"] and r["eof"] and not r["reset"], r
    assert r["ended"] and r["err"] == "", r


def test_a_peer_that_keeps_its_side_open_costs_a_second(client):
    r = run(client, "small", 50, keep_open_s=3.0)
    assert r["intact"] and r["eof"], r
    assert r["ended"] and r["err"] == "", r
    assert 900 <= r["close_ms"] <= 1500, r  # the FIN timer, nothing longer


if __name__ == "__main__":
    a = sys.argv
    inner(a[1], a[2], int(a[3]), int(a[4]), int(a[5]), float(a[6]), float(a[7]))
