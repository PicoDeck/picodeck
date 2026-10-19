"""TLS TCP sockets on a PicoCalc: test_tcp_tls.py's cases (setInsecure,
verification refusing a self-signed server, tcp:peerFingerprint(), a server
that ends with a TLS close_notify, a reconnect) against servers on this host's
LAN address. On the device Core 1 hashes the certificate at the end of the
handshake (wifi.c) and Mongoose's mg_tls_recv ends the stream at a
close_notify (a PicoDeck patch); no simulator build has the firmware's TLS.

    PICODECK_HW_HOST_IP=<this host's LAN address> pytest tests/e2e/test_hw_tcp_tls.py \\
        --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

The device must have WiFi on a network that reaches this host.
"""

from __future__ import annotations

import json
import time

import pytest

from net_servers import TcpEchoServer
from test_hw_http_close import _host_ip
from test_tcp_tls import (APP, APP_ID, APP_SRC, CASES, CERT, SECOND,
                          TlsClosingServer, TlsEchoServer, fingerprint)

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(300)]


def test_tls_sockets_on_device(target):
    host = _host_ip()
    tls = TlsEchoServer(CERT, "0.0.0.0").start()
    plain = TcpEchoServer("0.0.0.0").start()
    closing = TlsClosingServer([CERT, SECOND], "0.0.0.0").start()
    try:
        deadline = time.monotonic() + 30
        wifi = target.status().get("wifi")
        while wifi not in ("connected", "online") and time.monotonic() < deadline:
            time.sleep(1)
            wifi = target.status().get("wifi")
        if wifi not in ("connected", "online"):
            pytest.skip(f"device WiFi is {wifi!r}: configure wifi_ssid/wifi_pass")
        target.stage_lua_app(APP, APP_SRC.read_text(), requirements=["http"], id=APP_ID)
        target.write_file(f"/data/{APP_ID}/servers.json", json.dumps({
            "host": host, "tls": tls.port, "plain": plain.port, "closing": closing.port,
            "fingerprint": fingerprint(CERT), "second": fingerprint(SECOND)}).encode())
        run = target.run_lua_app(APP, timeout=120)
        run.assert_all_passed(CASES)
    finally:
        tls.stop()
        plain.stop()
        closing.stop()
