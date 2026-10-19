"""Local network servers for the firmware-network E2E suite
(test_network_firmware.py, specs/test-audit-2026-09-24.md §3.6).

Everything listens on 127.0.0.1 with an OS-assigned port, runs in daemon
threads, and records what it saw so a test can assert on the server side too
(bodies received, connections still open).

- HttpTestServer: HTTP/1.1 (keep-alive capable) with fixed endpoints:
    /ok        200, "hello picodeck", Content-Length
    /big       200, 1 MiB of a known pattern (big_body()), Content-Length
    /chunked   200, Transfer-Encoding: chunked, body "alphabetagamma"
    /close     200, no Content-Length, body then close (close-delimited)
    /closelater  like /close, but the server closes 300 ms after the body
    /drip      200, Content-Length 400, one byte every 50 ms
    /hang      reads the request, never answers; records when the client
               hangs up in .hang_closed (monotonic seconds since the request)
    /stall     headers (Content-Length 1000) and 10 body bytes, then silence
    /reset     headers (Content-Length 1000), 10 body bytes, then RST
    /echo      POST: 200 with the request body echoed back; the raw body is
               recorded in .posts
- TcpEchoServer: echoes every byte. A line "FLOOD\\n" switches the
  connection to a steady stream (1 KiB every 5 ms) until the peer goes;
  "BURST\\n" sends BURST_BODY (20000 bytes) once, then nothing.
  "BURSTCLOSE\\n" the same 20000 bytes, then the server closes.
  .open_count() is the number of connections the server still has open.
- BlackholeServer: a listener whose accept queue is kept full, so a new
  connect() is never answered (SYNs are dropped): a connect timeout target
  that needs no outside network.
"""

from __future__ import annotations

import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OK_BODY = b"hello picodeck"
CHUNKS = [b"alpha", b"beta", b"gamma"]
BIG_SIZE = 1 << 20
DRIP_SIZE = 400
DRIP_INTERVAL_S = 0.05


def big_body() -> bytes:
    """The /big payload: bytes (i * 7 + i // 256) & 0xFF, so a dropped,
    repeated or reordered block changes the checksum."""
    return bytes(((i * 7) + (i >> 8)) & 0xFF for i in range(BIG_SIZE))


_BIG = None


def _big() -> bytes:
    global _BIG
    if _BIG is None:
        _BIG = big_body()
    return _BIG


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive unless the client says close

    def log_message(self, fmt, *args):  # quiet: pytest captures stderr
        pass

    def _send(self, body: bytes, status: int = 200, extra=()):
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        srv = self.server
        srv.hits.append(self.path)
        path = self.path.split("?", 1)[0]
        try:
            if path == "/ok":
                self._send(OK_BODY)
            elif path == "/big":
                self._send(_big())
            elif path == "/chunked":
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for c in CHUNKS:
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
                    self.wfile.flush()
                    time.sleep(0.02)
                self.wfile.write(b"0\r\n\r\n")
            elif path == "/close":
                self.send_response(200)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(OK_BODY)
                self.wfile.flush()
                self.close_connection = True
            elif path == "/closelater":
                # Close-delimited: the body, then the server closes 300 ms
                # later (the client is still in BODY when the FIN arrives).
                self.send_response(200)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(OK_BODY)
                self.wfile.flush()
                time.sleep(0.3)
                self.close_connection = True
            elif path == "/drip":
                self.send_response(200)
                self.send_header("Content-Length", str(DRIP_SIZE))
                self.end_headers()
                self.wfile.flush()
                for i in range(DRIP_SIZE):
                    if srv.stopping.is_set():
                        break
                    self.wfile.write(bytes([0x41 + i % 26]))
                    self.wfile.flush()
                    time.sleep(DRIP_INTERVAL_S)
            elif path == "/hang":
                # Never answer; notice the client hanging up (EOF/RST).
                t0 = time.monotonic()
                self.connection.settimeout(0.05)
                while not srv.stopping.is_set():
                    try:
                        if self.connection.recv(1) == b"":
                            srv.hang_closed.append(time.monotonic() - t0)
                            break
                    except socket.timeout:
                        continue
                    except OSError:
                        srv.hang_closed.append(time.monotonic() - t0)
                        break
                self.close_connection = True
            elif path == "/stall":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.end_headers()
                self.wfile.write(b"0123456789")
                self.wfile.flush()
                srv.stopping.wait()
                self.close_connection = True
            elif path == "/reset":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.end_headers()
                self.wfile.write(b"0123456789")
                self.wfile.flush()
                time.sleep(0.05)
                # SO_LINGER {on, 0}: close() sends RST instead of FIN.
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                           struct.pack("ii", 1, 0))
                self.close_connection = True
            else:
                self._send(b"not found", status=404)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True

    def do_POST(self):
        srv = self.server
        srv.hits.append(self.path)
        n = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(n) if n else b""
        srv.posts.append(body)
        try:
            if self.path.split("?", 1)[0] == "/echo":
                self._send(body)
            else:
                self._send(b"not found", status=404)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class HttpTestServer:
    def __init__(self, host: str = "127.0.0.1"):
        self._srv = _Server((host, 0), _Handler)
        self._srv.hits = []
        self._srv.posts = []
        self._srv.hang_closed = []
        self._srv.stopping = threading.Event()
        self.port = self._srv.server_address[1]
        self._thread = threading.Thread(target=self._srv.serve_forever,
                                        kwargs={"poll_interval": 0.05},
                                        daemon=True)

    @property
    def hits(self) -> list:
        return self._srv.hits

    @property
    def posts(self) -> list:
        return self._srv.posts

    @property
    def hang_closed(self) -> list:
        """Seconds from each /hang request to the client hanging up."""
        return self._srv.hang_closed

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._srv.stopping.set()
        self._srv.shutdown()
        self._srv.server_close()


class TcpEchoServer:
    FLOOD_CHUNK = b"F" * 1024
    FLOOD_INTERVAL_S = 0.005
    # More than the firmware's TCP ring (TCP_RECV_BUF_DEFAULT, 8 KiB)
    BURST_BODY = bytes((i * 7) & 0xFF for i in range(20000))

    def __init__(self, host: str = "127.0.0.1"):
        self._lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._lsock.bind((host, 0))
        self._lsock.listen(16)
        self._lsock.settimeout(0.1)
        self.port = self._lsock.getsockname()[1]
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._open = 0
        self.accepted = 0
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def open_count(self) -> int:
        with self._lock:
            return self._open

    def wait_all_closed(self, timeout: float) -> int:
        """Wait until the peer has closed every connection; returns how many
        are still open at the deadline (0 = all closed)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.open_count() == 0:
                return 0
            time.sleep(0.05)
        return self.open_count()

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._lsock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._lock:
                self._open += 1
                self.accepted += 1
            threading.Thread(target=self._serve, args=(conn,),
                             daemon=True).start()

    def _serve(self, conn: socket.socket):
        conn.settimeout(0.1)
        flooding = False
        pending = b""
        try:
            while not self._stop.is_set():
                if flooding:
                    try:
                        conn.sendall(self.FLOOD_CHUNK)
                    except OSError:
                        break
                    # Notice the peer closing while we flood.
                    try:
                        conn.settimeout(self.FLOOD_INTERVAL_S)
                        if conn.recv(4096) == b"":
                            break
                    except socket.timeout:
                        pass
                    except OSError:
                        break
                    continue
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                pending += data
                if b"FLOOD\n" in pending:
                    flooding = True
                    continue
                if b"BURSTCLOSE\n" in pending:
                    # One burst, then the server closes at once.
                    conn.sendall(self.BURST_BODY)
                    break
                if b"BURST\n" in pending:
                    # One burst, then silence (the connection stays open).
                    conn.sendall(self.BURST_BODY)
                    pending = b""
                    continue
                conn.sendall(data)
                if len(pending) > 64:
                    pending = pending[-8:]
        finally:
            try:
                conn.close()
            except OSError:
                pass
            with self._lock:
                self._open -= 1

    def stop(self):
        self._stop.set()
        try:
            self._lsock.close()
        except OSError:
            pass


class BlackholeServer:
    """A port that never completes a TCP handshake: listen(0) and one
    connection parked in the accept queue fill it, so Linux drops further
    SYNs (net.ipv4.tcp_abort_on_overflow=0, the default) and connect()
    hangs until the client gives up."""

    def __init__(self, host: str = "127.0.0.1"):
        self._lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._lsock.bind((host, 0))
        self._lsock.listen(0)
        self.port = self._lsock.getsockname()[1]
        self._fillers = []
        # Fill the accept queue (backlog 0 admits one, sometimes two).
        for _ in range(3):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setblocking(False)
            try:
                s.connect(("127.0.0.1", self.port))
            except BlockingIOError:
                pass
            self._fillers.append(s)
        time.sleep(0.05)

    def is_black(self, timeout: float = 0.5) -> bool:
        """True if a fresh connect() does not complete within `timeout`."""
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(("127.0.0.1", self.port))
            return False
        except (socket.timeout, TimeoutError):
            return True
        except OSError:
            return False
        finally:
            s.close()

    def start(self):
        return self

    def stop(self):
        for s in self._fillers:
            s.close()
        self._lsock.close()


def _adler(data: bytes) -> int:
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b * 65536 + a


if __name__ == "__main__":
    # Hardware runs: serve the net_fw cases to a PicoCalc on the LAN.
    #   python3 tests/e2e/net_servers.py --advertise <this host's LAN IP> \
    #       --case tcp_close_while_receiving --out /tmp/servers.json
    # then put /tmp/servers.json at /data/com.test.net_fw/servers.json on the
    # device and launch net_fw (see the Task 13 report's hardware checklist).
    # The black-hole port is only black on this host's own stack; over the
    # LAN a connect to it may succeed, so skip the *_connect_timeout cases.
    import argparse
    import json
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--advertise", required=True,
                    help="address the device uses to reach this host")
    ap.add_argument("--case", required=True, help="net_fw case to run")
    ap.add_argument("--out", default="servers.json")
    args = ap.parse_args()
    http = HttpTestServer(args.bind).start()
    echo = TcpEchoServer(args.bind).start()
    hole = BlackholeServer(args.bind).start()
    cfg = {"host": args.advertise, "http": http.port, "echo": echo.port,
           "blackhole": hole.port, "big_sum": _adler(big_body()),
           "case": args.case}
    with open(args.out, "w") as f:
        json.dump(cfg, f)
    print(f"serving on {args.bind}: {json.dumps(cfg)} -> {args.out}")
    print("Ctrl-C to stop")
    try:
        while True:
            time.sleep(1)
            print(f"tcp open={echo.open_count()} accepted={echo.accepted} "
                  f"http hits={len(http.hits)}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        http.stop()
        echo.stop()
        hole.stop()
