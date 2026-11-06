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
    /range     RANGE_BODY (1000 bytes); honours a Range header with a 206 and
               the slice. The header seen by each request is in .ranges
    /slowok    200, OK_BODY, after 1 s of silence
    /short     headers (Content-Length 1000), 3 body bytes, then a clean close
    /empty     200 with an empty body
    /notfound  404 with a body
    /redir-abs, /redir-root, /redir-rel, /redir-rel-up, /redir-scheme,
    /sub/dir/redir-rel-up, /sub/dir/redir-rel: 302 to /ok by an absolute URL
               and each relative form a Location can take
    /redir-none 302 with no Location
    /redir-loop 302 to itself
- TcpEchoServer: echoes every byte. A line "FLOOD\\n" switches the
  connection to a steady stream (1 KiB every 5 ms) until the peer goes;
  "BURST\\n" sends BURST_BODY (20000 bytes) once, then nothing.
  "BURSTCLOSE\\n" the same 20000 bytes, then the server closes.
  .open_count() is the number of connections the server still has open.
- BlackholeServer: a listener whose accept queue is kept full, so a new
  connect() is never answered (SYNs are dropped): a connect timeout target
  that needs no outside network.
- SntpServer: answers SNTP queries while .answering is true (silent otherwise,
  as a network that blocks NTP), for PICODECK_SIM_NET_SNTP_URL. .queries and
  .answers count what it saw.
"""

from __future__ import annotations

import re
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
RANGE_SIZE = 1000


def range_body() -> bytes:
    """The /range payload: 1000 bytes that differ at every offset of interest."""
    return bytes((i * 3 + (i >> 4)) & 0xFF for i in range(RANGE_SIZE))


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


REDIRECTS = {"/redir-abs", "/redir-root", "/redir-rel", "/redir-rel-up",
             "/redir-scheme", "/redir-none", "/redir-loop"}


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

    def _range(self):
        """Serve range_body(), honouring `Range: bytes=from-[to]`."""
        body = range_body()
        rng = self.headers.get("Range")
        self.server.ranges.append(rng)
        m = re.fullmatch(r"bytes=(\d+)-(\d*)", rng or "")
        if not m:
            self._send(body)
            return
        first = int(m.group(1))
        last = int(m.group(2)) if m.group(2) else len(body) - 1
        last = min(last, len(body) - 1)
        self._send(body[first:last + 1], status=206,
                   extra=[("Content-Range",
                           f"bytes {first}-{last}/{len(body)}")])

    def _redirect(self, path):
        host = self.headers.get("Host", "")
        location = {
            "/redir-abs": f"http://{host}/ok",
            "/redir-root": "/ok",
            "/redir-rel": "ok",
            "/redir-rel-up": "../ok",
            "/redir-scheme": f"//{host}/ok",
            "/redir-loop": path,
            "/sub/dir/redir-rel": "./target",
            "/sub/dir/redir-rel-up": "../../ok",
        }.get(path)
        self.send_response(302)
        if location is not None:
            self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        srv = self.server
        srv.hits.append(self.path)
        path = self.path.split("?", 1)[0]
        try:
            if path == "/ok" or path == "/sub/dir/target":
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
            elif path == "/range":
                self._range()
            elif path == "/slowok":
                time.sleep(1.0)
                self._send(OK_BODY)
            elif path == "/short":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(b"abc")
                self.wfile.flush()
                self.close_connection = True
            elif path == "/empty":
                self._send(b"")
            elif path == "/notfound":
                self._send(b"nope", status=404)
            elif path in REDIRECTS or path.startswith("/sub/dir/redir-"):
                self._redirect(path)
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
        self._srv.ranges = []
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
    def ranges(self) -> list:
        """The Range header of each /range request (None if it had none)."""
        return self._srv.ranges

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


class SntpServer:
    """SNTP over UDP on 127.0.0.1: what wifi.c's time sync talks to in the
    firmware-network simulator (PICODECK_SIM_NET_SNTP_URL = .url)."""

    NTP_EPOCH_OFFSET = 2208988800  # seconds from 1900 to 1970

    def __init__(self, answering: bool = True):
        self.answering = answering
        self.queries = 0
        self.answers = 0
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.settimeout(0.05)
        self.url = f"udp://127.0.0.1:{self._sock.getsockname()[1]}"
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.is_set():
            try:
                query, peer = self._sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return
            self.queries += 1
            if not self.answering or len(query) < 48:
                continue
            now = struct.pack(">II", int(time.time()) + self.NTP_EPOCH_OFFSET, 0)
            # Version 4, mode 4 (server), stratum 1; the query's transmit
            # time comes back as the origin time.
            reply = (bytes([0x24, 1]) + bytes(22) + query[40:48] + now + now)
            self._sock.sendto(reply, peer)
            self.answers += 1

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=2)
        self._sock.close()


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
