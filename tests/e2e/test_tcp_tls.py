"""TLS TCP sockets against a local self-signed server: setInsecure,
verification refusing it, and tcp:peerFingerprint() (the SHA-256 of the
server's certificate, for apps that pin certificates). The certificate in
certs/selfsigned.pem is a fixed EC P-256 test certificate (CN=localhost)."""

import hashlib
import json
import socket
import ssl
import threading
from pathlib import Path

from helpers import E2E_DIR, run_lua_app, stage_lua_app
from net_servers import TcpEchoServer

APP = "tcp_tls"
APP_ID = "com.test.tcp_tls"
APP_SRC = E2E_DIR / "tcp_tls" / "main.lua"
CERT = E2E_DIR / "certs" / "selfsigned.pem"
CASES = ["insecure_tls_reports_the_fingerprint", "verified_tls_refuses_a_self_signed_server",
         "plain_sockets_have_no_fingerprint", "no_fingerprint_before_connecting"]


class TlsEchoServer:
    """Accepts TLS connections with `certfile` on 127.0.0.1 and echoes."""

    def __init__(self, certfile: Path):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(certfile))
        self.sock = socket.create_server(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]

    def start(self):
        threading.Thread(target=self._serve, daemon=True).start()
        return self

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        try:
            with self.ctx.wrap_socket(conn, server_side=True) as s:
                s.settimeout(5)
                while data := s.recv(4096):
                    s.sendall(data)
        except (OSError, ssl.SSLError):
            pass

    def stop(self):
        self.sock.close()


def fingerprint(pem_path: Path) -> str:
    """The SHA-256 of the certificate in a PEM file, lowercase hex."""
    pem = pem_path.read_text()
    begin = pem.index("-----BEGIN CERTIFICATE-----")
    end = pem.index("-----END CERTIFICATE-----") + len("-----END CERTIFICATE-----")
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem[begin:end])).hexdigest()


def test_tls_sockets_and_fingerprint(simulator):
    tls = TlsEchoServer(CERT).start()
    plain = TcpEchoServer().start()
    try:
        sd = Path(simulator.sd_card_path)
        stage_lua_app(sd, APP, APP_SRC.read_text(), requirements=["http"], id=APP_ID)
        data = sd / "data" / APP_ID
        data.mkdir(parents=True, exist_ok=True)
        (data / "servers.json").write_text(json.dumps(
            {"tls": tls.port, "plain": plain.port, "fingerprint": fingerprint(CERT)}))
        run = run_lua_app(simulator, APP, timeout=40)
        run.assert_all_passed(CASES)
    finally:
        tls.stop()
        plain.stop()
