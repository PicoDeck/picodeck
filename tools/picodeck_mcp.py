#!/usr/bin/env python3
"""PicoDeck MCP Server — exposes PicoDeck simulator and hardware to coding agents.

Transports: PicoDeck Simulator (JSON-RPC over TCP) and hardware devices over
serial (USB CDC /dev/ttyACM* or UART adapters /dev/ttyUSB*).  By default the
server auto-selects: simulator when reachable, otherwise a detected serial
device; a per-call device= argument always forces that serial port.

File transfer and screenshots on hardware use the firmware's base64 dev
commands (getb64/putb64/screenshot64) which work over any transport; flash()
uploads firmware via the SD-staged OTA path — no BOOTSEL/USB required.

Usage:
    Registered in .mcp.json as an MCP server.
    Auto mode (default):   python3 tools/picodeck_mcp.py
    Hardware only:         python3 tools/picodeck_mcp.py --hardware
    Simulator only:        python3 tools/picodeck_mcp.py --simulator
"""

import argparse
import asyncio
import base64
import hashlib
import io
import json
import os
import platform
import re
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from collections import deque
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, Optional

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False
if TYPE_CHECKING:
    import serial

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp import Image as MCPImage

mcp = FastMCP("picodeck")

DEFAULT_TIMEOUT = 5
DEFAULT_TCP_PORT = 7878
HARDWARE_MODE = False   # --hardware: hardware only, never simulator
SIMULATOR_ONLY = False  # --simulator: simulator only, never hardware
# Default (neither flag): AUTO — use the simulator when reachable, otherwise
# fall back to a detected serial device.

SCRN_MAGIC = b"SCRN"
SCRN_HEADER_SIZE = 12


# ── JSON-RPC 2.0 Client ─────────────────────────────────────────────────────────

class JRpcError(Exception):
    def __init__(self, code: int, message: str, data: Any = None):
        self.code = code
        self.message = message
        self.data = data
        super().__init__(f"[{code}] {message}")


class SimulatorConnection:
    """Manages a connection to the PicoDeck simulator via JSON-RPC 2.0 over TCP.

    Uses a single reader thread that routes ALL incoming messages:
    - Messages with an "id" → delivered to the corresponding pending call via Event
    - Messages without "id" → appended to notifications list
    This eliminates the race condition between call() and notification reading.
    """

    def __init__(self, tcp_port: int | None = None):
        self.tcp_port = tcp_port or DEFAULT_TCP_PORT
        self._sock: socket.socket | None = None
        self._lock = threading.Lock()  # protects _sock writes
        self._reader_thread: threading.Thread | None = None
        self._reader_done = threading.Event()
        self._notifications: list[dict] = []
        self._notif_lock = threading.Lock()
        self._id_counter = 1
        # Pending responses: id → {"event": Event, "result": dict|None}
        self._pending: dict[int, dict] = {}
        self._pending_lock = threading.Lock()
        self._connected = False

    def connect(self) -> None:
        if self._connected:
            return

        # Retry with exponential backoff
        delays = [0.1, 0.2, 0.4, 0.8, 1.6]
        last_error = None
        for attempt, delay in enumerate(delays):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(DEFAULT_TIMEOUT)
                sock.connect(("127.0.0.1", self.tcp_port))
                self._sock = sock
                self._connected = True
                break
            except (socket.error, ConnectionRefusedError) as e:
                last_error = e
                if sock:
                    sock.close()
                if attempt < len(delays) - 1:
                    time.sleep(delay)

        if not self._connected:
            raise RuntimeError(
                f"Cannot connect to simulator on port {self.tcp_port} "
                f"after {len(delays)} attempts. Is the simulator running? "
                f"Error: {last_error}"
            )

        # Start single reader thread
        self._reader_done.clear()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def close(self) -> None:
        self._connected = False
        self._reader_done.set()
        if self._reader_thread and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=2.0)
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        # Wake any pending calls
        with self._pending_lock:
            for entry in self._pending.values():
                entry["event"].set()
            self._pending.clear()

    def _reader_loop(self) -> None:
        """Single reader thread: routes ALL incoming JSON messages."""
        buf = ""
        while not self._reader_done.is_set():
            if not self._sock:
                break
            try:
                self._sock.settimeout(0.2)
                chunk = self._sock.recv(65536)
                if not chunk:
                    break
                buf += chunk.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    if "id" in obj:
                        # Response to a call — route to pending future
                        msg_id = obj["id"]
                        with self._pending_lock:
                            entry = self._pending.get(msg_id)
                            if entry:
                                entry["result"] = obj
                                entry["event"].set()
                    else:
                        # Notification
                        with self._notif_lock:
                            self._notifications.append(obj)
            except socket.timeout:
                continue
            except OSError:
                break

        self._connected = False

    def _send_raw(self, data: str) -> None:
        with self._lock:
            if not self._sock:
                raise RuntimeError("Not connected to simulator")
            self._sock.sendall((data + "\n").encode("utf-8"))

    def call(self, method: str, params: dict | None = None, timeout: float = DEFAULT_TIMEOUT) -> dict:
        """Send a JSON-RPC 2.0 request and return the result."""
        if not self._connected:
            self.connect()

        jid = self._id_counter
        self._id_counter += 1

        event = threading.Event()
        with self._pending_lock:
            self._pending[jid] = {"event": event, "result": None}

        request = {
            "jsonrpc": "2.0",
            "id": jid,
            "method": method,
            "params": params or {},
        }
        try:
            self._send_raw(json.dumps(request))
        except RuntimeError:
            with self._pending_lock:
                self._pending.pop(jid, None)
            raise

        if not event.wait(timeout):
            with self._pending_lock:
                self._pending.pop(jid, None)
            raise JRpcError(-32000, f"Timeout calling {method} after {timeout}s")

        with self._pending_lock:
            entry = self._pending.pop(jid, None)

        if not entry or not entry["result"]:
            raise JRpcError(-32000, f"Connection lost while calling {method}")

        result = entry["result"]
        if "error" in result:
            err = result["error"]
            raise JRpcError(
                err.get("code", -32603),
                err.get("message", "Unknown error"),
                err.get("data"),
            )
        return result.get("result", {})

    def get_notifications(self) -> list[dict]:
        with self._notif_lock:
            notes = list(self._notifications)
            self._notifications.clear()
        return notes

    def wait_for_notification(self, method: str | None = None, timeout: float = 30.0) -> dict | None:
        """Wait for a specific notification (or any if method=None)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._notif_lock:
                for i, n in enumerate(self._notifications):
                    if method is None or n.get("method") == method:
                        del self._notifications[i]
                        return n
            time.sleep(0.05)
        return None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()


# ── Global connection (lazy) ──────────────────────────────────────────────────────

_conn: SimulatorConnection | None = None
_conn_lock = threading.Lock()
_configured_port = DEFAULT_TCP_PORT


def get_connection() -> SimulatorConnection:
    global _conn
    with _conn_lock:
        if _conn is None:
            _conn = SimulatorConnection(tcp_port=_configured_port)
        # Try to reconnect if connection was lost
        if not _conn._connected:
            try:
                _conn.close()
            except Exception:
                pass
            _conn = SimulatorConnection(tcp_port=_configured_port)
        return _conn


# ── Simulator Lifecycle Management ────────────────────────────────────────────

def _default_project_root() -> Path:
    """Repo root used for simulator builds/launches.

    PICODECK_PROJECT_ROOT overrides the location of this file, so a session
    working in a git worktree can point the server at that tree instead of
    the main checkout.  start_simulator(project_root=...) overrides per call.
    """
    env = os.environ.get("PICODECK_PROJECT_ROOT", "")
    return Path(env).resolve() if env else Path(__file__).resolve().parent.parent


class SimulatorManager:
    """Manages simulator process lifecycle."""

    def __init__(self, project_root: Path | None = None):
        self.process: subprocess.Popen | None = None
        self.port: int = 0
        self.project_root = project_root or _default_project_root()
        self.sd_card_path: str | None = None  # SD dir of the running sim
        # Bounded tails of the child's stdout/stderr. The pipes MUST be
        # drained continuously (see _start_pipe_drains) or the simulator
        # deadlocks once the 64KB kernel pipe buffer fills — the sim's own
        # host-side traces (e.g. "[TRAMP] fs_read" per file read) block in
        # write() while holding the stdio lock, wedging the main thread.
        # Same bug and fix as tests/e2e/picodeck_simulator.py.
        self._stdout_tail: deque = deque(maxlen=2000)
        self._stderr_tail: deque = deque(maxlen=2000)
        self._drain_threads: list[threading.Thread] = []

    def ensure_running(self, port: int = 0, sd_card_path: str | None = None,
                       headless: bool = False) -> int:
        """Ensure a simulator is running. Returns the TCP port.

        If port=0, auto-assigns a port.
        """
        # Check if already running on requested port
        if port > 0 and self._probe_port(port):
            self.port = port
            return port

        # Check if our managed process is still alive
        if self.process and self.process.poll() is None and self.port > 0:
            if self._probe_port(self.port):
                return self.port

        # Build if needed (missing binary, or sources newer than the binary —
        # a stale simulator silently running old source cost real debugging time)
        binary = self.project_root / "build_sim" / "picodeck_simulator"
        if not binary.exists():
            self._build()
        elif self._sources_newer_than(binary):
            print("[picodeck] simulator binary is stale (source changed) — rebuilding…")
            self._build()

        # Start simulator
        cmd = [str(binary)]
        if port >= 0:
            cmd += ["--port", str(port)]
        if sd_card_path:
            cmd += ["--sd-card", sd_card_path]
        else:
            cmd += ["--sd-card", str(self.project_root)]
        self.sd_card_path = sd_card_path or str(self.project_root)

        env = os.environ.copy()
        if headless:
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIODRIVER"] = "dummy"

        self.process = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
            text=True,
        )
        with _tracked_pids_lock:
            _tracked_pids.add(self.process.pid)

        # Read stdout to find the assigned port
        actual_port = port if port > 0 else 0
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                stdout = self.process.stdout.read() if self.process.stdout else ""
                stderr = self.process.stderr.read() if self.process.stderr else ""
                raise RuntimeError(
                    f"Simulator exited early (code {self.process.returncode})\n"
                    f"stdout: {stdout}\nstderr: {stderr}"
                )
            line = self.process.stdout.readline() if self.process.stdout else ""
            if "[Socket] TCP port:" in line:
                actual_port = int(line.split(":")[-1].strip())
                break
            if actual_port > 0 and self._probe_port(actual_port):
                break
            time.sleep(0.1)

        if actual_port <= 0:
            raise RuntimeError("Failed to determine simulator TCP port")

        # From here on nothing else reads the child's pipes, so they must be
        # drained continuously or the simulator eventually deadlocks in
        # write() — see __init__ for the mechanism.
        self._start_pipe_drains()

        # Wait for port to be connectable
        start = time.monotonic()
        while time.monotonic() - start < 5.0:
            if self._probe_port(actual_port):
                break
            time.sleep(0.1)

        self.port = actual_port
        return actual_port

    def _start_pipe_drains(self):
        """Continuously drain the child's stdout/stderr into bounded tails.

        The startup loop above reads stdout only until it finds the port
        line; without these threads the sim wedges once either pipe fills
        (64KB), which a single read-heavy app launch (C-Dogs asset load,
        one "[TRAMP] fs_read" stderr line per read) achieves in seconds.
        """
        def drain(stream, tail):
            try:
                for line in iter(stream.readline, ""):
                    tail.append(line.rstrip("\n"))
            except (ValueError, OSError):
                pass  # stream closed during shutdown
            finally:
                try:
                    stream.close()
                except Exception:
                    pass

        for stream, tail in ((self.process.stdout, self._stdout_tail),
                             (self.process.stderr, self._stderr_tail)):
            if stream is None:
                continue
            t = threading.Thread(target=drain, args=(stream, tail), daemon=True)
            t.start()
            self._drain_threads.append(t)

    def stop(self):
        """Stop managed simulator."""
        if self.process:
            # Try graceful shutdown via RPC
            if self.port > 0:
                try:
                    conn = SimulatorConnection(tcp_port=self.port)
                    conn.connect()
                    conn.call("shutdown", timeout=2)
                    conn.close()
                except Exception:
                    pass

            # Wait briefly, then terminate
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            with _tracked_pids_lock:
                _tracked_pids.discard(self.process.pid)
            self.process = None
            self.port = 0

    def _sources_newer_than(self, binary: Path) -> bool:
        """True if any simulator/OS/driver source is newer than the binary."""
        try:
            bin_mtime = binary.stat().st_mtime
        except OSError:
            return True
        roots = (self.project_root / "simulator", self.project_root / "src")
        exts = {".c", ".h", ".cpp", ".pio", ".txt"}
        for root in roots:
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = [d for d in dirnames if d not in ("build", ".git")]
                for name in filenames:
                    if os.path.splitext(name)[1] not in exts:
                        continue
                    try:
                        if os.path.getmtime(os.path.join(dirpath, name)) > bin_mtime:
                            return True
                    except OSError:
                        continue
        return False

    def _build(self):
        """Build the simulator."""
        build_dir = self.project_root / "build_sim"
        sim_dir = self.project_root / "simulator"
        subprocess.run(
            ["cmake", "-B", str(build_dir), str(sim_dir)],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["make", "-C", str(build_dir), "-j4"],
            check=True, capture_output=True,
        )

    @staticmethod
    def _probe_port(port: int) -> bool:
        """Check if a TCP port is accepting connections."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(("127.0.0.1", port))
            s.close()
            return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            return False


_sim_manager: SimulatorManager | None = None
_tracked_pids: set[int] = set()
_tracked_pids_lock = threading.Lock()


# ── Hardware connection helpers ─────────────────────────────────────────────────

def find_usb_device() -> str | None:
    import glob
    # CDC ACM (USB data port) first, then plain USB-serial adapters (UART).
    for pattern in ["/dev/ttyACM*", "/dev/tty.usbmodem*",
                    "/dev/ttyUSB*", "/dev/tty.usbserial*"]:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_serial(port: str, timeout: float = DEFAULT_TIMEOUT):
    if not HAS_SERIAL:
        raise RuntimeError("pyserial not installed. Run: pip install pyserial")
    ser = serial.Serial(port, baudrate=115200, timeout=timeout)
    # UART adapters (CH340 etc.) garble the first bytes after open while the
    # control lines settle — flush a newline through and discard everything
    # before sending the real command.
    time.sleep(0.35)
    ser.reset_input_buffer()
    ser.write(b"\n")
    ser.flush()
    time.sleep(0.15)
    ser.reset_input_buffer()
    return ser


def _fnv1a(data: bytes, h: int = 2166136261) -> int:
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


class LineReader:
    """Line-splitter over a serial port that survives across wait calls —
    a partial line buffered between reads must not be dropped when the
    caller switches from streaming to marker-waiting (or per-chunk ACKs)."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = b""

    def lines(self, idle_timeout: float):
        """Yield stripped lines; return when no data arrives for idle_timeout."""
        last_data = time.monotonic()
        while time.monotonic() - last_data < idle_timeout:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                last_data = time.monotonic()
                self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield line.strip()

    def wait_marker(self, ok_markers, err_markers, timeout: float) -> bytes:
        """Wait for a line containing any ok marker; raise on error markers."""
        for line in self.lines(timeout):
            for m in err_markers:
                if m in line:
                    raise RuntimeError(f"device: {line.decode(errors='replace')}")
            for m in ok_markers:
                if m in line:
                    return line
        raise TimeoutError(f"no response matching {ok_markers} within {timeout}s")


# ── Hardware log monitor (persistent serial session) ──────────────────────────
#
# The per-command open→write→read→close pattern loses everything the device
# prints between commands ([APP] logs especially) — nothing holds the port.
# A HardwareMonitor keeps the port open and drains output into a ring
# buffer; line commands route through it and bulk transfers park it for
# exclusive port access, so the two never fight over bytes.

class HardwareMonitor:
    """Holds a serial port open, draining device output into a ring buffer."""

    def __init__(self, port: str):
        self.port = port
        self.buffer: deque = deque(maxlen=5000)
        self._ser = open_serial(port, timeout=0.2)
        self._cmd_lock = threading.Lock()  # one command/suspend at a time
        self._cmd_lines: deque | None = None
        self._stop = threading.Event()
        self._suspend_req = threading.Event()
        self._parked = threading.Event()
        self._linebuf = b""
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            if self._suspend_req.is_set():
                self._parked.set()
                time.sleep(0.05)
                continue
            self._parked.clear()
            try:
                chunk = self._ser.read(max(1, self._ser.in_waiting))
            except Exception:
                time.sleep(0.05)  # port closed for suspend, or unplugged
                continue
            if not chunk:
                continue
            self._linebuf += chunk
            while b"\n" in self._linebuf:
                raw, self._linebuf = self._linebuf.split(b"\n", 1)
                line = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                self.buffer.append(line)
                if self._cmd_lines is not None:
                    self._cmd_lines.append(line)

    def command(self, cmd: str, timeout: float = DEFAULT_TIMEOUT,
                idle: float | None = 1.0) -> list[str]:
        """Send a line command; collect response lines until an end marker,
        or `idle` seconds of quiet after some output. idle=None waits for
        the marker (or the timeout): for commands that go quiet mid-reply,
        such as unzip inflating a large file."""
        with self._cmd_lock:
            self._cmd_lines = deque()
            try:
                self._ser.write(f"{cmd}\n".encode())
                self._ser.flush()
                lines: list[str] = []
                deadline = time.monotonic() + timeout
                last_data = time.monotonic()
                while time.monotonic() < deadline:
                    try:
                        line = self._cmd_lines.popleft()
                    except IndexError:
                        if (idle is not None and lines
                                and time.monotonic() - last_data > idle):
                            break
                        time.sleep(0.02)
                        continue
                    last_data = time.monotonic()
                    lines.append(line)
                    if _is_cmd_end(line):
                        break
                return lines
            finally:
                self._cmd_lines = None

    @contextmanager
    def suspend(self):
        """Park the reader and release the port for exclusive use."""
        with self._cmd_lock:
            self._suspend_req.set()
            self._parked.wait(timeout=2.0)
            try:
                self._ser.close()
            except Exception:
                pass
            try:
                yield
            finally:
                self._ser = open_serial(self.port, timeout=0.2)
                self._suspend_req.clear()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self._ser.close()
        except Exception:
            pass


# Response lines that terminate a dev command exchange.
_CMD_END_MARKERS = ["pong", "Total:", "Error:", "Launching", "Rebooting",
                    "Unknown", "Status:", "Created:", "Unzipped", "Deleted:",
                    "Pad:", "Usage: pad"]


def _is_cmd_end(line: str) -> bool:
    """True for a reply line that ends a command exchange. The firmware's
    own `[DEV] Command: <cmd>` echo never does: its arguments can hold a
    marker word (an app named `pong`)."""
    return (line.startswith("[DEV] ")
            and not line.startswith("[DEV] Command:")
            and any(kw in line for kw in _CMD_END_MARKERS))

_hw_monitors: dict[str, HardwareMonitor] = {}
_hw_monitors_lock = threading.Lock()


def _monitor_for(port: str) -> HardwareMonitor | None:
    with _hw_monitors_lock:
        return _hw_monitors.get(port)


def _get_or_start_monitor(port: str) -> tuple[HardwareMonitor, bool]:
    """Return (monitor, started_now) for a port, creating one if needed."""
    with _hw_monitors_lock:
        mon = _hw_monitors.get(port)
        if mon:
            return mon, False
        mon = HardwareMonitor(port)
        _hw_monitors[port] = mon
        return mon, True


@contextmanager
def _exclusive_serial(port: str):
    """Give callers that open the port directly (bulk transfers, binary
    screenshot, key sequences) exclusive access: parks any active monitor
    for the duration.  NOT re-entrant — never nest."""
    mon = _monitor_for(port)
    if mon:
        with mon.suspend():
            yield
    else:
        yield


def _retry_transfer(fn, attempts: int = 3):
    """Retry a serial bulk transfer on integrity/corruption failures.

    A device log line interleaving mid-transfer surfaces as an fnv1a
    mismatch or a corrupted base64 line; both are transient (the firmware
    also mutes its log sink during transfers now), so a retry nearly
    always succeeds.  Non-transient errors re-raise immediately.
    """
    for attempt in range(attempts):
        try:
            return fn()
        except (RuntimeError, TimeoutError) as e:
            msg = str(e)
            transient = ("integrity" in msg or "corrupted" in msg
                         or "mismatch" in msg or "stalled" in msg)
            if not transient or attempt == attempts - 1:
                raise
            time.sleep(0.5)


# ── Base64 file transfer (works on UART and CDC — stdio-based) ────────────────

def do_get_file_b64(port: str, remote_path: str) -> bytes:
    """Fetch a file via getb64: integrity-checked, with retry on corruption."""
    def attempt() -> bytes:
        with _exclusive_serial(port):
            return _do_get_file_b64_once(port, remote_path)
    return _retry_transfer(attempt)


def _do_get_file_b64_once(port: str, remote_path: str) -> bytes:
    ser = open_serial(port, timeout=0.2)
    try:
        ser.write(f"getb64 {remote_path}\n".encode())
        ser.flush()
        raw = bytearray()
        size = None
        for line in LineReader(ser).lines(idle_timeout=10.0):
            if line.startswith(b"~"):
                try:
                    raw += base64.b64decode(line[1:], validate=True)
                except Exception:
                    raise RuntimeError(
                        "corrupted base64 line (a device log line may have "
                        "interleaved mid-transfer) — retry the transfer")
            elif b"B64 size=" in line:
                size = int(line.split(b"size=")[1].split()[0])
            elif b"Failed to open" in line:
                raise FileNotFoundError(f"device: no such file: {remote_path}")
            elif b"Unknown command" in line:
                raise RuntimeError(
                    "device firmware lacks getb64 — flash the current build")
            elif b"B64_END" in line:
                fnv = int(line.split(b"fnv1a=")[1].split()[0], 16)
                if size is not None and size >= 0 and len(raw) != size:
                    raise RuntimeError(
                        f"size mismatch: got {len(raw)}, expected {size}")
                if _fnv1a(bytes(raw)) != fnv:
                    raise RuntimeError("integrity check failed (fnv1a mismatch)")
                return bytes(raw)
        raise TimeoutError(f"getb64 stalled after {len(raw)} bytes")
    finally:
        ser.close()


def do_put_file_b64(port: str, data: bytes, remote_path: str,
                    progress=None) -> str:
    """Send bytes via the firmware's putb64 command (chunk + ACK pacing).

    progress: optional callback(raw_bytes_sent, raw_bytes_total), invoked
    after each ACKed chunk (used by tools/ota_flash.py)."""
    with _exclusive_serial(port):
        return _do_put_file_b64_once(port, data, remote_path, progress)


def _do_put_file_b64_once(port: str, data: bytes, remote_path: str,
                          progress=None) -> str:
    b64 = base64.b64encode(data)
    CHUNK = 512  # b64 chars per line → 384 raw bytes ≤ device write buffer
    ser = open_serial(port, timeout=0.2)
    try:
        reader = LineReader(ser)
        # Create the parent directory first (firmware mkdir is recursive).
        # Older firmware answers "Unknown command" — treat the whole step as
        # best-effort and let putb64 surface any real failure.
        parent = str((PurePosixPath("/") / remote_path.lstrip("/")).parent)
        if parent not in ("/", ""):
            ser.write(f"mkdir {parent}\n".encode())
            ser.flush()
            try:
                reader.wait_marker([b"Created:", b"Unknown command"],
                                   [b"mkdir failed"], 5.0)
            except TimeoutError:
                pass
        ser.write(f"putb64 {remote_path} {len(data)}\n".encode())
        ser.flush()
        reader.wait_marker([b"Ready B64"],
                           [b"Failed to open", b"Usage:", b"Unknown command"], 5.0)
        final = None
        for off in range(0, len(b64), CHUNK):
            ser.write(b64[off:off + CHUNK] + b"\n")
            ser.flush()
            line = reader.wait_marker([b"ACK ", b"File received"],
                                      [b"Error"], 15.0)
            if progress:
                progress(min(off + CHUNK, len(b64)) * 3 // 4, len(data))
            if b"File received" in line:
                final = line
        if final is None:
            final = reader.wait_marker([b"File received"], [b"Error"], 15.0)
        fnv = int(final.split(b"fnv1a=")[1].split()[0], 16)
        if _fnv1a(data) != fnv:
            raise RuntimeError("integrity check failed after upload "
                               "(device wrote different bytes) — retry")
        return f"Uploaded {len(data)} bytes to {remote_path} (fnv1a verified)"
    finally:
        ser.close()


def do_screenshot_b64(port: str) -> bytes:
    """Capture the framebuffer via screenshot64 (slow on UART: ~25s)."""
    def attempt() -> bytes:
        with _exclusive_serial(port):
            return _do_screenshot_b64_once(port)
    return _retry_transfer(attempt)


def _do_screenshot_b64_once(port: str) -> bytes:
    ser = open_serial(port, timeout=0.2)
    try:
        ser.write(b"screenshot64\n")
        ser.flush()
        raw = bytearray()
        w = h = 320
        for line in LineReader(ser).lines(idle_timeout=10.0):
            if line.startswith(b"~"):
                try:
                    raw += base64.b64decode(line[1:], validate=True)
                except Exception:
                    raise RuntimeError(
                        "corrupted base64 line (a device log line may have "
                        "interleaved mid-transfer) — retry the transfer")
            elif b"SCRN64 " in line:
                for tok in line.split():
                    if tok.startswith(b"w="):
                        w = int(tok[2:])
                    elif tok.startswith(b"h="):
                        h = int(tok[2:])
            elif b"Unknown command" in line:
                raise RuntimeError(
                    "device firmware lacks screenshot64 — flash the current build")
            elif b"SCRN64_END" in line:
                fnv = int(line.split(b"fnv1a=")[1].split()[0], 16)
                if _fnv1a(bytes(raw)) != fnv:
                    raise RuntimeError("integrity check failed (fnv1a mismatch)")
                return rgb565be_to_png(bytes(raw), w, h)
        raise TimeoutError(f"screenshot64 stalled after {len(raw)} bytes")
    finally:
        ser.close()


# ── Simulator helpers ────────────────────────────────────────────────────────────

def do_screenshot_simulator(timeout: float = DEFAULT_TIMEOUT) -> bytes:
    conn = get_connection()
    result = conn.call("screenshot", {"format": "png"}, timeout=timeout)
    png_b64 = result.get("data", "")
    return base64.b64decode(png_b64)


def rgb565be_to_png(data: bytes, width: int, height: int) -> bytes:
    import io
    from PIL import Image
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        hi = data[i * 2]
        lo = data[i * 2 + 1]
        pixel = (hi << 8) | lo
        r = ((pixel >> 11) & 0x1F) * 255 // 31
        g = ((pixel >> 5) & 0x3F) * 255 // 63
        b = (pixel & 0x1F) * 255 // 31
        pixels[i * 3] = r
        pixels[i * 3 + 1] = g
        pixels[i * 3 + 2] = b
    img = Image.frombytes("RGB", (width, height), bytes(pixels))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


# ── Hardware helpers ────────────────────────────────────────────────────────────

def do_command_hardware(cmd: str, port: str, timeout: float = DEFAULT_TIMEOUT,
                        idle: float | None = 1.0) -> list[str]:
    """Run a dev command; see HardwareMonitor.command for `idle` (without a
    monitor the port's read timeout is `timeout`, so there is no idle end)."""
    mon = _monitor_for(port)
    if mon:
        return mon.command(cmd, timeout, idle)
    ser = open_serial(port, timeout)
    try:
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        lines = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            lines.append(line)
            if _is_cmd_end(line):
                break
        return lines
    finally:
        ser.close()


def do_screenshot_hardware(port: str, timeout: float = DEFAULT_TIMEOUT) -> bytes:
    with _exclusive_serial(port):
        return _do_screenshot_hardware_once(port, timeout)


def _do_screenshot_hardware_once(port: str, timeout: float = DEFAULT_TIMEOUT) -> bytes:
    ser = open_serial(port, timeout)
    try:
        ser.write(b"screenshot\n")
        ser.flush()
        buf = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            buf += chunk
            idx = buf.find(SCRN_MAGIC)
            if idx >= 0:
                buf = buf[idx:]
                break
        else:
            raise TimeoutError("Timed out waiting for SCRN header")
        while len(buf) < SCRN_HEADER_SIZE:
            chunk = ser.read(SCRN_HEADER_SIZE - len(buf))
            if not chunk:
                raise TimeoutError("Timed out reading header")
            buf += chunk
        width, height, fmt = struct.unpack_from("<HHH", buf, 4)
        pixel_bytes = width * height * 2
        data = buf[SCRN_HEADER_SIZE:]
        while len(data) < pixel_bytes:
            remaining = pixel_bytes - len(data)
            chunk = ser.read(min(remaining, 4096))
            if not chunk:
                raise TimeoutError(f"Timed out reading pixels ({len(data)}/{pixel_bytes})")
            data += chunk
        return rgb565be_to_png(data[:pixel_bytes], width, height)
    finally:
        ser.close()


# ── Unified dispatch ───────────────────────────────────────────────────────────

_sim_probe_cache = {"t": 0.0, "ok": False}


def _sim_reachable() -> bool:
    now = time.monotonic()
    if now - _sim_probe_cache["t"] < 3.0:
        return _sim_probe_cache["ok"]
    ok = SimulatorManager._probe_port(_configured_port)
    _sim_probe_cache["t"] = now
    _sim_probe_cache["ok"] = ok
    return ok


def resolve_port(device: str | None = None) -> str | None:
    """Pick the transport: a serial port path for hardware, None for simulator.

    An explicit device argument always selects hardware.  Otherwise:
    --hardware → detected serial device (error if none); --simulator → always
    simulator; default (auto) → simulator when reachable, else a detected
    serial device, else simulator (whose error path reports the connect
    failure)."""
    if device:
        return device
    if HARDWARE_MODE:
        port = find_usb_device()
        if not port:
            raise RuntimeError(
                "No PicoDeck hardware device found. Connect via USB or use --simulator flag."
            )
        return port
    if SIMULATOR_ONLY:
        return None
    if _sim_reachable():
        return None
    return find_usb_device()  # may be None → simulator error path


# ── MCP Tools ──────────────────────────────────────────────────────────────────


@mcp.tool()
async def ping(device: str | None = None) -> str:
    """Ping the PicoDeck device or simulator. Returns 'pong' on success."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "ping", port)
            for line in lines:
                if "pong" in line.lower():
                    return "pong"
            return f"No pong. Response: {lines}"
        except Exception as e:
            return str(e)
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "ping", timeout=5)
            return f"pong (uptime: {result.get('uptime_ms', '?')}ms)"
        except Exception as e:
            return f"Cannot connect: {e}"


@mcp.tool()
async def screenshot(device: str | None = None, save_path: str = "") -> list:
    """Take a screenshot of the PicoDeck display. Returns a PNG image block.

    save_path: optional host filesystem path — when given, the PNG is also
    written there (useful for byte-diffing successive screenshots).

    Works on simulator and hardware (UART hardware uses a base64 transfer,
    ~25 seconds at 115200 baud)."""
    port = resolve_port(device)
    if port:
        try:
            # The binary SCRN path needs a CDC host; base64 works everywhere.
            if "ttyACM" in port or "usbmodem" in port:
                png_bytes = await asyncio.to_thread(do_screenshot_hardware, port)
            else:
                png_bytes = await asyncio.to_thread(do_screenshot_b64, port)
        except ImportError:
            return ["Pillow not installed: pip install Pillow"]
        except Exception as e:
            return [f"Error: {e}"]
    else:
        try:
            png_bytes = await asyncio.to_thread(do_screenshot_simulator)
        except Exception as e:
            return [f"Error: {e}"]

    # MCPImage produces a real ImageContent block; returning raw dicts here
    # made FastMCP JSON-dump them into unusable TextContent.
    note = "Screenshot (320x320 PNG)"
    if save_path:
        try:
            Path(save_path).write_bytes(png_bytes)
            note += f", saved to {save_path}"
        except OSError as e:
            note += f" (save to {save_path} failed: {e})"
    return [MCPImage(data=png_bytes, format="png"), note]


@mcp.tool()
async def list_apps(device: str | None = None) -> str:
    """List all apps installed on the PicoDeck device or simulator."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "list", port)
            apps = []
            for line in lines:
                if "[DEV] Available apps:" in line:
                    continue
                if "[DEV] Total:" in line:
                    break
                if line.strip().startswith("  ") or line.strip().startswith("-"):
                    apps.append(line.strip())
            return "\n".join(apps) if apps else "\n".join(lines)
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "list_dir", {"path": "/apps"})
            entries = result.get("entries", [])
            if not entries:
                return "(no apps found)"
            lines = []
            for e in entries:
                if e.get("is_dir"):
                    lines.append(f"  {e['name']}/")
                else:
                    lines.append(f"  {e['name']}  ({e.get('size', 0)} bytes)")
            return "\n".join(lines)
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def launch_app(app_name: str, device: str | None = None) -> str:
    """Launch an app by name on the simulator. Non-blocking — returns immediately.

    Use wait_for_exit() to wait for the app to finish.
    """
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, f"launch {app_name}", port)
            for line in lines:
                if "Launching" in line:
                    return f"Launching: {line}"
            return f"Sent launch command. Response: {lines}"
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(
                conn.call, "launch_app", {"name": app_name}, timeout=5
            )
            return f"App launch initiated: {result}"
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def wait_for_exit(app_name: str = "", timeout: float = 60.0, device: str | None = None) -> str:
    """Wait for the currently running app to exit. Returns when app exits or timeout."""
    port = resolve_port(device)
    if port:
        return "(wait_for_exit not supported in hardware mode)"
    try:
        conn = get_connection()
        notif = await asyncio.to_thread(conn.wait_for_notification, "app.exited", timeout=timeout)
        if notif:
            params = notif.get("params", {})
            name = params.get("name", "?")
            result = params.get("result", "?")
            error = params.get("error")
            msg = f"App '{name}' exited (result={result}"
            if not params.get("found", True):
                msg += ", not found"
            if error:
                msg += f", error={error}"
            return msg + ")"
        return f"Timeout after {timeout}s — app still running"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def exit_app(device: str | None = None) -> str:
    """Send exit signal to the currently running app."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "exit", port, timeout=2)
            # "Error: exit: no app running" at the launcher.
            if any("no app running" in ln for ln in lines):
                return "No app running (nothing to exit)."
            return "Exit signal sent."
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, "exit_app", timeout=5)
            return f"Exit signal sent: {result}"
        except Exception as e:
            return f"Error: {e}"


# ── Keypress helpers ──────────────────────────────────────────────────────────

# Named keys valid on both transports (aliases normalized before sending).
_KEY_ALIASES = {"escape": "esc", "delete": "del", "bkspc": "backspace", "fn": "sym"}


def _expand_key_sequence(spec: str, count: int) -> list[str]:
    """Expand a key spec into the ordered list of keys to press.

    Accepted forms: "down" | "down,down,enter" | "down down enter" |
    "down 5x" (repeat previous key N times) | "downx5". A bare "5x" repeats
    the preceding key. `count` repeats the whole expanded sequence.
    """
    tokens = [t for t in re.split(r"[\s,]+", spec.strip()) if t]
    keys: list[str] = []
    for tok in tokens:
        m = re.fullmatch(r"(\d+)[xX]", tok)
        if m:
            if not keys:
                raise ValueError(f"repeat token '{tok}' has no preceding key")
            keys.extend([keys[-1]] * (int(m.group(1)) - 1))
            continue
        m = re.fullmatch(r"(.+?)[xX](\d+)", tok)
        if m:
            base = m.group(1)
            if len(base) == 1 or base.lower() in _NAMED_KEYS or base.lower() in _KEY_ALIASES:
                keys.extend([base] * int(m.group(2)))
                continue
        keys.append(tok)
    if not keys:
        raise ValueError("empty key spec")
    return keys * max(1, count)


# Named keys accepted by the simulator's inject_button (after alias
# normalization). Anything else of length 1 is typed via inject_char.
_NAMED_KEYS = {
    "up", "down", "left", "right", "enter", "esc", "menu", "tab",
    "backspace", "del", "shift", "ctrl", "sym",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10",
}

# Modifier keys that may prefix a combo ("ctrl+s", "shift+left").
_MODIFIER_KEYS = {"ctrl", "shift", "sym", "alt"}


def _parse_combo(key: str) -> tuple[list[str], str]:
    """Split "ctrl+s" into (["ctrl"], "s"). Returns ([], key) for non-combos."""
    if "+" not in key or len(key) == 1:
        return [], key
    parts = key.split("+")
    mods, base = parts[:-1], parts[-1]
    if base == "":  # "ctrl++" — literal '+' as the base key
        base = "+"
    norm = [_normalize_key(m) for m in mods]
    if norm and all(m in _MODIFIER_KEYS for m in norm):
        return norm, base
    return [], key


def _normalize_key(key: str) -> str:
    k = key.lower()
    return _KEY_ALIASES.get(k, k) if len(k) > 1 else key


def _is_char_key(key: str) -> bool:
    return len(key) == 1 and key.lower() not in _NAMED_KEYS


def do_keysequence_hardware(keys: list[str], port: str, delay_ms: int,
                            timeout: float = DEFAULT_TIMEOUT) -> list[str]:
    """Send a sequence of key commands over one serial session.

    Combos ("ctrl+s") expand to keydown <mod> / keypress <key> / keyup <mod>.
    """
    with _exclusive_serial(port):
        return _do_keysequence_hardware_once(keys, port, delay_ms, timeout)


def _do_keysequence_hardware_once(keys: list[str], port: str, delay_ms: int,
                                  timeout: float = DEFAULT_TIMEOUT) -> list[str]:
    ser = open_serial(port, timeout)

    def send(cmd: str) -> str:
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if "Key injected" in line:
                return "ok"
            if "Unknown key" in line:
                return "unknown key"
        return "no ack"

    try:
        results = []
        for i, key in enumerate(keys):
            mods, base = _parse_combo(key)
            status = "ok"
            for m in mods:
                st = send(f"keydown {m}")
                if st != "ok":
                    status = st
            if status == "ok":
                base_name = _normalize_key(base) if len(base) > 1 else base
                status = send(f"keypress {base_name}")
            if mods:
                # Give the app a few frames to observe the chord before the
                # modifiers are released.
                time.sleep(max(0.15, delay_ms / 1000.0))
                for m in reversed(mods):
                    send(f"keyup {m}")
            results.append(f"{key}: {status}")
            if i < len(keys) - 1:
                time.sleep(max(0, delay_ms) / 1000.0)
        return results
    finally:
        ser.close()


@mcp.tool()
async def keypress(key: str, count: int = 1, delay_ms: int = 100,
                   device: str | None = None) -> str:
    """Inject keypress(es) on the PicoDeck simulator or hardware.

    Valid named keys: up, down, left, right, enter, esc, menu, f1-f10,
    backspace, tab, del, shift, ctrl, sym. Single characters (a-z, A-Z,
    0-9, punctuation) are typed as character input.

    Modifier combos: "ctrl+s", "shift+left", "ctrl+shift+x" hold the
    modifier(s), press the base key, then release the modifier(s).

    Sequences: "down,down,enter" or "down down enter" presses keys in
    order; "down 5x" repeats the previous key 5 times ("downx5" also
    works). `count` repeats the whole sequence; `delay_ms` is the gap
    between presses (default 100ms).
    """
    try:
        keys = _expand_key_sequence(key, count)
    except ValueError as e:
        return f"Error: {e}"

    port = resolve_port(device)
    if port:
        try:
            results = await asyncio.to_thread(
                do_keysequence_hardware, keys, port, delay_ms
            )
            bad = [r for r in results if not r.endswith(": ok")]
            summary = f"{len(keys)} key(s) injected: " + ", ".join(results)
            return summary if not bad else f"PARTIAL FAILURE — {summary}"
        except Exception as e:
            return f"Error: {e}"

    # Simulator: route single chars to inject_char, named keys to inject_button
    sent = []
    try:
        conn = get_connection()
        for i, k in enumerate(keys):
            mods, base = _parse_combo(k)
            try:
                for m in mods:
                    await asyncio.to_thread(
                        conn.call, "inject_button",
                        {"button": m, "action": "press"}, timeout=5)
                if _is_char_key(base):
                    method, params = "inject_char", {"char": base}
                else:
                    method, params = "inject_button", {
                        "button": _normalize_key(base), "action": "click"}
                await asyncio.to_thread(conn.call, method, params, timeout=5)
                if mods:
                    # Give the app a few frames to observe the chord before
                    # the modifiers are released.
                    await asyncio.sleep(max(0.15, delay_ms / 1000.0))
                    for m in reversed(mods):
                        await asyncio.to_thread(
                            conn.call, "inject_button",
                            {"button": m, "action": "release"}, timeout=5)
                sent.append(f"{k}: ok")
            except JRpcError as e:
                if e.code == -32602:
                    sent.append(f"{k}: unknown key")
                else:
                    sent.append(f"{k}: {e.message}")
            if i < len(keys) - 1:
                await asyncio.sleep(max(0, delay_ms) / 1000.0)
    except Exception as e:
        return f"Error: {e}"
    bad = [s for s in sent if not s.endswith(": ok")]
    summary = f"{len(keys)} key(s) injected: " + ", ".join(sent)
    return summary if not bad else f"PARTIAL FAILURE — {summary}"


# ── Gamepad (the `pad` dev command) ─────────────────────────────────────────

# Button names the firmware's `pad` command takes (src/dev_ops.c), joined by
# '+'; "none" releases everything (the pad stays connected), "off"
# disconnects it.
_PAD_BUTTONS = {"up", "down", "left", "right", "a", "b", "x", "y", "l", "r",
                "start", "select", "home"}


def _parse_pad_states(spec: str) -> list[str]:
    """Split a pad spec into the states to set in turn, normalized.

    "A+Right" -> ["a+right"]; "home, up, a" or "home up a" -> ["home", "up",
    "a"]; "none" and "off" stand alone. Raises ValueError on an unknown
    button or an empty spec.
    """
    states = []
    for tok in re.split(r"[\s,]+", spec.strip()):
        if not tok:
            continue
        tok = tok.lower()
        if tok in ("none", "off"):
            states.append(tok)
            continue
        names = tok.split("+")
        bad = [n for n in names if n not in _PAD_BUTTONS]
        if bad:
            raise ValueError(f"unknown pad button {bad[0]!r} (buttons: "
                             + ", ".join(sorted(_PAD_BUTTONS)) + ")")
        states.append("+".join(names))
    if not states:
        raise ValueError("empty pad state")
    return states


def _pad_commands(spec: str, hold_ms: int = 0) -> list[str]:
    """The `pad` dev commands for a spec. One state is set as given (held
    until the next command, or hold_ms); several are pressed in turn, each
    held hold_ms (100 ms when 0) so each reads as its own press."""
    states = _parse_pad_states(spec)
    if len(states) > 1 and hold_ms <= 0:
        hold_ms = 100
    hold = f" {int(hold_ms)}" if hold_ms > 0 else ""
    return [f"pad {st}{'' if st in ('none', 'off') else hold}"
            for st in states]


def _pad_reply(lines: list[str]) -> str:
    """The firmware's answer to one `pad` command ("Pad: up+a",
    "Error: ...", "Usage: pad ..."), or "no reply"."""
    for ln in lines:
        if ln.startswith("[DEV] ") and not ln.startswith("[DEV] Command:"):
            text = ln[len("[DEV] "):]
            if text.startswith(("Pad:", "Error:", "Usage:", "Unknown")):
                return text
    return "no reply"


@mcp.tool()
async def pad(state: str, hold_ms: int = 0, delay_ms: int = 150,
              device: str | None = None) -> str:
    """Drive the test gamepad on the simulator or hardware (the `pad` dev
    command): apps read it through picocalc.gamepad / api->gamepad, ORed with
    the keyboard, like a physical controller.

    state: buttons joined by '+' ("a", "up+a"; up down left right a b x y l
    r start select, and home, which opens the system menu), "none" (release
    everything, stay connected) or "off" (disconnect). A single state is held
    until the next call, or for hold_ms. A list ("home, up, a") presses each
    in turn, each held hold_ms (default 100), delay_ms apart (default 150).
    """
    try:
        cmds = _pad_commands(state, hold_ms)
    except ValueError as e:
        return f"Error: {e}"
    replies = []
    port = resolve_port(device)
    try:
        conn = None if port else get_connection()
        for i, cmd in enumerate(cmds):
            if port:
                lines = await asyncio.to_thread(do_command_hardware, cmd, port,
                                                timeout=3)
                replies.append(_pad_reply(lines))
            else:
                r = await asyncio.to_thread(conn.call, "dev_command",
                                            {"cmd": cmd}, timeout=10)
                replies.append(r.get("output", "") if isinstance(r, dict)
                               else str(r))
            if i < len(cmds) - 1:
                await asyncio.sleep(max(0, delay_ms) / 1000.0)
    except Exception as e:
        return f"Error: {e}"
    bad = [r for r in replies if not r.startswith("Pad:")]
    summary = "; ".join(replies)
    return summary if not bad else f"PARTIAL FAILURE — {summary}"


@mcp.tool()
async def send_command(command: str, timeout: float = 5.0, device: str | None = None) -> str:
    """Send a raw dev command to PicoDeck and return the response."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, command, port, timeout=timeout)
            return "\n".join(lines) if lines else "(no response)"
        except Exception as e:
            return f"Error: {e}"
    else:
        try:
            parts = command.split(maxsplit=1)
            method = parts[0]
            params = {}
            if len(parts) > 1:
                try:
                    params = json.loads(parts[1])
                except json.JSONDecodeError:
                    pass
            conn = get_connection()
            result = await asyncio.to_thread(conn.call, method, params, timeout=timeout)
            return json.dumps(result, indent=2)
        except JRpcError as e:
            return f"[{e.code}] {e.message}"
        except Exception as e:
            return f"Error: {e}"


@mcp.tool()
async def get_status(device: str | None = None) -> str:
    """Get device status: running app, uptime, WiFi state (and heap on the
    simulator)."""
    port = resolve_port(device)
    if port:
        try:
            lines = await asyncio.to_thread(do_command_hardware, "status", port)
            for line in lines:
                if "[DEV] Status:" in line:
                    return line.split("[DEV] ", 1)[1]
            # Older firmware without the status command: report the build.
            lines = await asyncio.to_thread(do_command_hardware, "ver", port)
            ver = next((l.split("[DEV] ", 1)[1] for l in lines
                        if "PicoDeck build" in l), "unknown firmware")
            return (f"{ver} — this firmware predates the `status` dev "
                    "command; flash the current build for app/wifi/battery "
                    "status.")
        except Exception as e:
            return f"Error: {e}"
    try:
        conn = get_connection()
        app = await asyncio.to_thread(conn.call, "get_running_app", timeout=5)
        heap = await asyncio.to_thread(conn.call, "get_heap_info", timeout=5)
        wifi = await asyncio.to_thread(conn.call, "get_wifi_state", timeout=5)

        app_name = app.get("name") if app else None
        if app_name:
            status = f"Running: {app_name}"
        else:
            status = "Status: Launcher (no app running)"

        return (
            f"{status}\n"
            f"Heap: {heap.get('lua_heap_used_kb', '?')}KB used / "
            f"{heap.get('lua_heap_free_kb', '?')}KB free\n"
            f"PSRAM: {heap.get('psram_total_kb', '?')}KB total\n"
            f"WiFi: {wifi.get('status', '?')} ({wifi.get('ssid', '')}) "
            f"IP: {wifi.get('ip', '?')}"
        )
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_log_buffer(lines: int = 100, device: str | None = None) -> str:
    """Retrieve recent log lines from the simulator or hardware device.

    Hardware: the first call starts a persistent serial capture — the port
    stays open and everything the device prints ([APP] logs, [DEV] output,
    crashes) accumulates in a 5000-line ring buffer. Subsequent calls
    return the newest lines. Commands and file transfers coordinate with
    the capture automatically; use stop_log_capture() to release the port.
    """
    port = resolve_port(device)
    if port:
        try:
            mon, started = await asyncio.to_thread(_get_or_start_monitor, port)
        except Exception as e:
            return f"Error opening {port}: {e}"
        if started:
            return (f"Hardware log capture started on {port} — the ring "
                    "buffer is filling from now on; call get_log_buffer "
                    "again to read it.")
        buf = list(mon.buffer)
        if not buf:
            return "(no log lines captured yet)"
        return "\n".join(buf[-lines:])
    try:
        conn = get_connection()
        result = await asyncio.to_thread(
            conn.call, "get_log_buffer", {"tail": lines}, timeout=5)
        # Entries are {seq, t_ms, src, text}; src is lua/native/os/err.
        log_lines = [
            l if isinstance(l, str)
            else (l.get("text", "") if l.get("src") in ("lua", "native")
                  else f"[{l.get('src')}] {l.get('text', '')}")
            for l in result.get("lines", [])
        ]
        if not log_lines:
            return "(no log lines)"
        return "\n".join(log_lines[-lines:])
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def clear_log_buffer(device: str | None = None) -> str:
    """Clear the simulator log buffer (or the hardware capture buffer)."""
    port = resolve_port(device)
    if port:
        mon = _monitor_for(port)
        if not mon:
            return (f"No log capture running on {port} — call "
                    "get_log_buffer first to start one.")
        mon.buffer.clear()
        return "Hardware log capture buffer cleared."
    try:
        conn = get_connection()
        await asyncio.to_thread(conn.call, "clear_log_buffer", timeout=5)
        return "Log buffer cleared."
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def stop_log_capture(device: str | None = None) -> str:
    """Stop the persistent hardware log capture and release the serial port.

    Use before handing the port to another program (minicom, a script), or
    after a reboot/flash renumbers /dev/ttyACM*."""
    port = resolve_port(device)
    if not port:
        return "(log capture applies to hardware only)"
    with _hw_monitors_lock:
        mon = _hw_monitors.pop(port, None)
    if not mon:
        return f"No active log capture on {port}."
    await asyncio.to_thread(mon.stop)
    return f"Log capture stopped on {port}; serial port released."


@mcp.tool()
async def set_wifi_state(
    status: str = "connected",
    error_code: int = 0,
    error_str: str = "",
    device: str | None = None,
) -> str:
    """Set WiFi state in the simulator (for testing error paths)."""
    port = resolve_port(device)
    if port:
        return "(WiFi control not available in hardware mode)"
    try:
        params = {"status": status}
        if error_code:
            params["error_code"] = str(error_code)
        if error_str:
            params["error_str"] = error_str
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "set_wifi_state", params, timeout=5)
        return f"WiFi state set: {result}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def set_time_multiplier(multiplier: float = 1.0, device: str | None = None) -> str:
    """Set time multiplier in simulator (0=pause, 1=realtime, >1=fast-forward)."""
    port = resolve_port(device)
    if port:
        return "(Time control not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(
            conn.call, "set_time_multiplier", {"multiplier": multiplier}, timeout=5
        )
        return f"Time multiplier set: {result}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_terminal_buffer(device: str | None = None) -> str:
    """Get the active terminal's text buffer (simulator only).

    Returns JSON with cols, rows, cursor position, and text lines.
    Only works when a Lua app has created a picocalc.terminal instance.
    """
    port = resolve_port(device)
    if port:
        return "(terminal buffer not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_terminal_buffer", timeout=5)
        return json.dumps(result, indent=2)
    except JRpcError as e:
        return f"[{e.code}] {e.message}"
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_heap_info(device: str | None = None) -> str:
    """Get memory usage: Lua heap and PSRAM."""
    port = resolve_port(device)
    if port:
        return "(heap info not available in hardware mode)"
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_heap_info", timeout=5)
        return (
            f"Lua heap: {result.get('lua_heap_used_kb', '?')}KB used, "
            f"{result.get('lua_heap_free_kb', '?')}KB free\n"
            f"PSRAM total: {result.get('psram_total_kb', '?')}KB"
        )
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_crash_log(device: str | None = None) -> str:
    """Get the crash log. Simulator: sim crash file. Hardware: the persisted
    hard-fault log at /system/crashlog.txt (via the crashlog dev command)."""
    port = resolve_port(device)
    if port:
        def _hw_crashlog() -> str:
            with _exclusive_serial(port):
                return _hw_crashlog_read()

        def _hw_crashlog_read() -> str:
            ser = open_serial(port, timeout=0.2)
            try:
                ser.write(b"crashlog\n")
                ser.flush()
                collecting = False
                out: list[str] = []
                for line in LineReader(ser).lines(idle_timeout=5.0):
                    text = line.decode("utf-8", errors="replace")
                    if "CRASHLOG BEGIN" in text:
                        collecting = True
                    elif "CRASHLOG END" in text:
                        return "\n".join(out) if out else "(crash log empty)"
                    elif "No crash log" in text:
                        return "No crash log on device."
                    elif "Unknown command" in text:
                        return ("Device firmware lacks the crashlog command — "
                                "flash the current build first.")
                    elif collecting:
                        out.append(text)
                if out:
                    return "\n".join(out) + "\n(warning: END marker not seen)"
                return "No response to crashlog command."
            finally:
                ser.close()
        try:
            return await asyncio.to_thread(_hw_crashlog)
        except Exception as e:
            return f"Error: {e}"

    # Try reading via RPC first
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "get_crash_log", timeout=5)
        crash = result.get("crash_log")
        if crash:
            return f"Crash log:\n{crash}"
        return "No crash log found."
    except Exception:
        pass

    # If simulator is unreachable, try reading the file directly
    crash_log_path = Path("/tmp/picodeck_sim_crash.log")
    if crash_log_path.exists():
        try:
            return f"Crash log (from file):\n{crash_log_path.read_text()}"
        except Exception as e:
            return f"Error reading crash log: {e}"
    return "No crash log found and simulator is unreachable."


@mcp.tool()
async def shutdown_simulator(device: str | None = None) -> str:
    """Gracefully shut down the PicoDeck simulator."""
    port = resolve_port(device)
    if port:
        return "(shutdown not available in hardware mode)"
    try:
        conn = get_connection()
        await asyncio.to_thread(conn.call, "shutdown", timeout=3)
        return "Shutdown signal sent."
    except Exception as e:
        return f"Error (simulator may already be stopped): {e}"


@mcp.tool()
async def start_simulator(
    port: int = 0,
    sd_card_path: str = "",
    headless: bool = True,
    project_root: str = "",
) -> str:
    """Build (if needed) and start a PicoDeck simulator instance.

    Args:
        port: TCP port (0=auto-assign)
        sd_card_path: Path to SD card directory (empty=default)
        headless: Run without display (for CI/testing)
        project_root: Repo or worktree root whose simulator sources and
            build_sim/ are used. Defaults to the PICODECK_PROJECT_ROOT env
            var, else the repo containing this server. Pass your worktree
            path here when the server was launched from the main checkout.

    Returns the TCP port of the started simulator.
    """
    global _sim_manager, _configured_port, _conn
    root = Path(project_root).resolve() if project_root else None
    if _sim_manager is None:
        _sim_manager = SimulatorManager(root)
    elif root and _sim_manager.project_root != root:
        # Different tree requested: manage it with a fresh manager. Any
        # simulator from the old root stays tracked in _tracked_pids, so
        # kill_simulators still reaps it.
        _sim_manager = SimulatorManager(root)
    try:
        actual_port = await asyncio.to_thread(
            _sim_manager.ensure_running,
            port=port,
            sd_card_path=sd_card_path or None,
            headless=headless,
        )
        # Reset connection to use new port (atomic update under lock)
        with _conn_lock:
            _configured_port = actual_port
            if _conn:
                try:
                    _conn.close()
                except Exception:
                    pass
                _conn = None
        return (f"Simulator running on port {actual_port} "
                f"(root: {_sim_manager.project_root})")
    except Exception as e:
        return f"Error starting simulator: {e}"


# ── Simulator Process Management ──────────────────────────────────────────────


def _find_simulator_pids() -> set[int]:
    """Find all picodeck_simulator processes on the system via pgrep."""
    try:
        result = subprocess.run(
            ["pgrep", "-f", "picodeck_simulator"],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode == 0:
            return {int(line) for line in result.stdout.strip().split("\n") if line.strip()}
    except (FileNotFoundError, subprocess.TimeoutExpired, ValueError):
        pass
    return set()


def _is_pid_alive(pid: int) -> bool:
    """Check if a process is still alive."""
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


@mcp.tool()
async def list_simulators() -> str:
    """List all tracked simulator PIDs and any picodeck_simulator processes on the system."""
    with _tracked_pids_lock:
        tracked = set(_tracked_pids)

    system_pids = await asyncio.to_thread(_find_simulator_pids)

    lines = []
    if tracked:
        lines.append("Tracked PIDs:")
        for pid in sorted(tracked):
            alive = _is_pid_alive(pid)
            lines.append(f"  PID {pid}: {'alive' if alive else 'dead'}")
    else:
        lines.append("No tracked PIDs.")

    orphans = system_pids - tracked
    if orphans:
        lines.append("System picodeck_simulator processes (not tracked):")
        for pid in sorted(orphans):
            lines.append(f"  PID {pid}")

    if not tracked and not system_pids:
        lines.append("No simulator processes found.")

    return "\n".join(lines)


@mcp.tool()
async def kill_simulators(include_orphans: bool = True) -> str:
    """Force-kill all tracked simulator processes.

    Args:
        include_orphans: Also kill any picodeck_simulator processes found on the
                         system that weren't launched by this MCP session.
    """
    global _sim_manager, _conn

    with _tracked_pids_lock:
        tracked = set(_tracked_pids)

    pids_to_kill = set(tracked)
    if include_orphans:
        system_pids = await asyncio.to_thread(_find_simulator_pids)
        pids_to_kill |= system_pids

    killed = []
    failed = []
    already_dead = []

    for pid in sorted(pids_to_kill):
        if not _is_pid_alive(pid):
            already_dead.append(pid)
            continue
        try:
            os.kill(pid, signal.SIGKILL)
            killed.append(pid)
        except ProcessLookupError:
            already_dead.append(pid)
        except PermissionError:
            failed.append(pid)

    # Reap child processes to avoid zombies in pgrep
    for pid in killed:
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass

    # Clean up tracked set
    with _tracked_pids_lock:
        _tracked_pids.clear()

    # Reset manager and connection state
    if _sim_manager:
        if _sim_manager.process:
            try:
                _sim_manager.process.wait(timeout=1)
            except Exception:
                pass
        _sim_manager.process = None
        _sim_manager.port = 0

    with _conn_lock:
        if _conn:
            try:
                _conn.close()
            except Exception:
                pass
            _conn = None

    lines = []
    if killed:
        lines.append(f"Killed: {', '.join(str(p) for p in killed)}")
    if already_dead:
        lines.append(f"Already dead: {', '.join(str(p) for p in already_dead)}")
    if failed:
        lines.append(f"Permission denied: {', '.join(str(p) for p in failed)}")
    if not pids_to_kill:
        lines.append("No simulator processes to kill.")

    return "\n".join(lines)


# ── Hardware-only Tools ────────────────────────────────────────────────────────


@mcp.tool()
async def reboot(mode: str = "normal", device: str | None = None) -> str:
    """Reboot the PicoDeck hardware device (not available in simulator)."""
    if mode not in ("normal", "flash"):
        return f"Unknown mode '{mode}'. Use 'normal' or 'flash'."
    try:
        port = resolve_port(device)
        if port is None:
            return ("Reboot targets hardware. No serial device detected "
                    "(simulator is active); pass device= to force one.")
        cmd = "reboot-flash" if mode == "flash" else "reboot"

        def _send_reboot() -> None:
            with _exclusive_serial(port):
                ser = open_serial(port, timeout=1)
                ser.write(f"{cmd}\n".encode())
                ser.flush()
                time.sleep(0.1)
                ser.close()

        await asyncio.to_thread(_send_reboot)
        if mode == "flash":
            return "Reboot-to-flash sent. Device will appear as USB drive."
        return "Reboot sent."
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def flash(file: str, device: str | None = None) -> str:
    """Flash firmware over the SD-staged OTA path.  Safe for cross-version
    updates as of 2026-07-18: ota_write_and_reboot now masks interrupts for
    the entire flash write (IRQ handlers are flash-resident; the old
    per-sector re-enable windows bricked cross-version updates — verified
    fixed by an end-to-end cross-version OTA on hardware).  Requires the
    on-device firmware to include that fix; older firmware may brick on a
    cross-version flash (recovery: BOOTSEL + copy build/picodeck.uf2)."""
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    try:
        port = resolve_port(device)
        if port is None:
            return "Flash targets hardware; no serial device detected."
        data = Path(file).read_bytes()
        if data[:4] == b"UF2\n" or data[:4] == b"UF2\x0a":
            return ("This is a UF2 file — the OTA path needs the raw .bin "
                    "(build/picodeck.bin).")
        if len(data) < 256:
            return "File too small to be firmware."
        sha = hashlib.sha256(data).hexdigest()

        def _do_flash() -> str:
            do_put_file_b64(port, (sha + "\n").encode(), "/system/update.sha256")
            do_put_file_b64(port, data, "/system/update.bin")
            with _exclusive_serial(port):
                ser = open_serial(port, timeout=1)
                ser.write(b"reboot\n")
                ser.flush()
                time.sleep(0.1)
                ser.close()
            return (f"Uploaded {len(data)} bytes (sha256 {sha[:12]}…) and "
                    "rebooted. The device verifies the hash and programs "
                    "flash on boot — watch its screen; do not power off.")
        return await asyncio.to_thread(_do_flash)
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def put_file(local_path: str, remote_path: str, device: str | None = None) -> str:
    """Upload a file to the PicoDeck SD card (hardware: base64 over serial;
    simulator: copies into the simulated SD directory)."""
    try:
        port = resolve_port(device)
    except Exception as e:
        return f"Error: {e}"
    if port is None:
        try:
            sim_apps = os.environ.get("PICODECK_SIMULATOR_SD", ".")
            dest = Path(sim_apps) / remote_path.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(local_path, dest)
            return f"Copied to simulator: {dest}"
        except Exception as e:
            return f"Error: {e}"
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    try:
        data = Path(local_path).read_bytes()
        return await asyncio.to_thread(do_put_file_b64, port, data, remote_path)
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_file(remote_path: str, local_path: str, device: str | None = None) -> str:
    """Download a file from the PicoDeck SD card (hardware: base64 over serial;
    simulator: copies from the simulated SD directory)."""
    try:
        port = resolve_port(device)
    except Exception as e:
        return f"Error: {e}"
    if port is None:
        try:
            sim_apps = os.environ.get("PICODECK_SIMULATOR_SD", ".")
            src = Path(sim_apps) / remote_path.lstrip("/")
            shutil.copy(src, local_path)
            return f"Copied from simulator: {src} -> {local_path}"
        except Exception as e:
            return f"Error: {e}"
    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"
    try:
        data = await asyncio.to_thread(do_get_file_b64, port, remote_path)
        Path(local_path).write_bytes(data)
        return f"Downloaded {len(data)} bytes: {remote_path} -> {local_path}"
    except Exception as e:
        return f"Error: {e}"


# Files never worth shipping to the device.
_PUSH_APP_EXCLUDE = shutil.ignore_patterns(
    ".git", ".git*", "__pycache__", "*.pyc", ".DS_Store")


def _push_app_wanted(rel_posix: str, name: str) -> bool:
    """Filter for the hardware ZIP: mirror _PUSH_APP_EXCLUDE."""
    parts = rel_posix.split("/")
    for p in parts:
        if p == "__pycache__" or p == ".DS_Store" or p.startswith(".git"):
            return False
        if p.endswith(".pyc"):
            return False
    return True


@mcp.tool()
async def push_app(local_dir: str, app_name: str = "",
                   device: str | None = None) -> str:
    """Push a whole app directory to /apps/<name> in one operation.

    Simulator: copies the tree into the simulated SD card.
    Hardware: zips the directory in memory, uploads one file over serial,
    extracts it on-device with the `unzip` dev command and deletes the
    temporary archive — orders of magnitude faster than per-file transfers
    for asset-heavy apps.

    app_name defaults to the basename of local_dir.
    """
    src = Path(local_dir).expanduser().resolve()
    if not src.is_dir():
        return f"Error: {local_dir} is not a directory"
    name = app_name or src.name
    if "/" in name or name in (".", ".."):
        return f"Error: invalid app name: {name}"
    if not any((src / probe).exists()
               for probe in ("app.json", "main.lua", "main.elf")):
        return (f"Error: {src} has no app.json/main.lua/main.elf — "
                "not an app directory")

    try:
        port = resolve_port(device)
    except Exception as e:
        return f"Error: {e}"

    if port is None:
        # Simulator: straight copy into the SD directory the sim is using.
        sd = (_sim_manager.sd_card_path if _sim_manager else None) \
            or os.environ.get("PICODECK_SIMULATOR_SD", ".")
        dest = Path(sd) / "apps" / name
        try:
            if dest.exists():
                shutil.rmtree(dest)
            shutil.copytree(src, dest, ignore=_PUSH_APP_EXCLUDE)
            n = sum(1 for p in dest.rglob("*") if p.is_file())
            return (f"Copied {n} files to simulator: {dest}\n"
                    "Note: launch_app rescans /apps when the name is not "
                    "found, so new apps launch without a restart; after "
                    "changing an existing app's app.json, call the "
                    "rescan_apps RPC.")
        except Exception as e:
            return f"Error: {e}"

    if not HAS_SERIAL:
        return "pyserial not installed: pip install pyserial"

    # Hardware: single ZIP over serial + on-device extraction.
    buf = io.BytesIO()
    count = 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(src.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(src).as_posix()
            if not _push_app_wanted(rel, p.name):
                continue
            zf.write(p, rel)
            count += 1
    if count == 0:
        return f"Error: nothing to push from {src}"
    data = buf.getvalue()

    tmp_zip = "/data/tmp/push_app.zip"
    try:
        upload = await asyncio.to_thread(do_put_file_b64, port, data, tmp_zip)
        # Extraction is SD-bound: allow generous headroom for slow cards.
        # unzip is silent while it inflates each file (a 700 KB file takes
        # ~1.8 s), so wait for its "Unzipped"/"Error:" line, not for quiet.
        timeout = max(30.0, 10.0 + count * 0.5 + len(data) / (64 * 1024))
        lines = await asyncio.to_thread(
            do_command_hardware, f"unzip {tmp_zip} /apps/{name}", port, timeout,
            None)
        result = "\n".join(lines[-3:]) if lines else "(no response)"
        await asyncio.to_thread(do_command_hardware, f"rm {tmp_zip}", port, 10.0)
        if any("Unzipped" in ln for ln in lines):
            return (f"Pushed {count} files ({len(data)} bytes zipped) to "
                    f"/apps/{name}\n{result}")
        return f"Error: extraction did not complete:\n{result}\n({upload})"
    except Exception as e:
        return f"Error: {e}"


# ── Display Diagnostics (simulator only) ─────────────────────────────────────


@mcp.tool()
async def display_stats(
    x: int | None = None,
    y: int | None = None,
    device: str | None = None,
) -> str:
    """Get framebuffer statistics: non-zero pixel count, unique colors, first non-zero pixel.
    Optionally inspect a specific pixel by providing x,y coordinates."""
    if HARDWARE_MODE:
        return "display_stats is only available in simulator mode."
    try:
        conn = get_connection()
        params = {}
        if x is not None:
            params["x"] = x
        if y is not None:
            params["y"] = y
        result = await asyncio.to_thread(conn.call, "display_stats", params, timeout=5)
        total = result.get("total_pixels", 102400)
        nz = result.get("nonzero_pixels", 0)
        pct = (nz * 100.0 / total) if total else 0
        unique = result.get("unique_colors", 0)
        first = result.get("first_nonzero")

        lines = [
            f"Non-zero pixels: {nz}/{total} ({pct:.1f}%)",
            f"Unique colors: {unique}",
        ]
        if first and isinstance(first, dict):
            fx, fy = first.get("x", 0), first.get("y", 0)
            fv = first.get("rgb565", 0)
            r = ((fv >> 11) & 0x1F) << 3
            g = ((fv >> 5) & 0x3F) << 2
            b = (fv & 0x1F) << 3
            lines.append(f"First non-zero: ({fx}, {fy}) = 0x{fv:04X} → RGB({r}, {g}, {b}) [#{r:02X}{g:02X}{b:02X}]")
        else:
            lines.append("First non-zero: none (all black)")

        pixel_at = result.get("pixel_at")
        if pixel_at and isinstance(pixel_at, dict):
            px, py = pixel_at["x"], pixel_at["y"]
            pv = pixel_at["rgb565"]
            pr, pg, pb = pixel_at["r"], pixel_at["g"], pixel_at["b"]
            lines.append(f"Pixel ({px}, {py}): 0x{pv:04X} → RGB({pr}, {pg}, {pb}) [#{pr:02X}{pg:02X}{pb:02X}]")

        return "\n".join(lines)
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def display_diff(
    action: str,
    device: str | None = None,
) -> str:
    """Capture or compare framebuffer snapshots for pixel-level diff.
    action='capture' saves the current framebuffer as reference.
    action='compare' compares current framebuffer against the saved reference."""
    if HARDWARE_MODE:
        return "display_diff is only available in simulator mode."
    try:
        conn = get_connection()
        result = await asyncio.to_thread(conn.call, "display_diff", {"action": action}, timeout=5)

        if action == "capture":
            return "Reference framebuffer captured. Use action='compare' after a change to see the diff."

        if action == "compare":
            changed = result.get("changed_pixels", 0)
            total = result.get("total_pixels", 102400)
            pct = result.get("change_pct", 0.0)
            bbox = result.get("bbox")

            lines = [f"Changed pixels: {changed}/{total} ({pct:.2f}%)"]
            if bbox and isinstance(bbox, dict):
                lines.append(f"Bounding box: x={bbox['x']} y={bbox['y']} w={bbox['w']} h={bbox['h']}")
            else:
                lines.append("No changes detected.")
            return "\n".join(lines)

        return f"Unknown action '{action}'. Use 'capture' or 'compare'."
    except Exception as e:
        return f"Error: {e}"


@mcp.tool()
async def get_pixel(
    x: int,
    y: int,
    w: int = 1,
    h: int = 1,
    device: str | None = None,
) -> str:
    """Read pixel value(s) from the framebuffer. Returns RGB565 and decoded RGB.
    Single pixel: provide x,y. Region: provide x,y,w,h (max 16x16)."""
    if HARDWARE_MODE:
        return "get_pixel is only available in simulator mode."
    try:
        conn = get_connection()
        params: dict[str, Any] = {"x": x, "y": y}
        if w > 1:
            params["w"] = w
        if h > 1:
            params["h"] = h
        result = await asyncio.to_thread(conn.call, "get_pixel", params, timeout=5)

        if "pixels" in result:
            # Region mode
            pixels = result["pixels"]
            rw, rh = result.get("w", w), result.get("h", h)
            lines = [f"Region ({x},{y}) {rw}x{rh}:"]
            idx = 0
            for row in range(rh):
                row_vals = []
                for col in range(rw):
                    if idx < len(pixels):
                        row_vals.append(f"0x{pixels[idx]:04X}")
                    idx += 1
                lines.append(f"  y={y+row}: {' '.join(row_vals)}")
            return "\n".join(lines)
        else:
            # Single pixel
            px = result.get("rgb565", 0)
            r, g, b = result.get("r", 0), result.get("g", 0), result.get("b", 0)
            return f"Pixel ({x}, {y}): 0x{px:04X} → RGB({r}, {g}, {b}) [#{r:02X}{g:02X}{b:02X}]"
    except Exception as e:
        return f"Error: {e}"


# ── Main Entry Point ──────────────────────────────────────────────────────────


def main():
    global HARDWARE_MODE, SIMULATOR_ONLY, _configured_port

    parser = argparse.ArgumentParser(
        description="PicoDeck MCP Server - Control PicoDeck simulator and/or hardware device"
    )
    parser.add_argument("--hardware", action="store_true", help="Hardware only (USB serial)")
    parser.add_argument("--simulator", action="store_true", help="Simulator only")
    parser.add_argument("--port", type=int, default=DEFAULT_TCP_PORT, help="Simulator TCP port (default: 7878)")
    parser.add_argument("--device", help="Device endpoint (serial port or socket path)")
    parser.add_argument(
        "--transport", choices=["stdio", "sse"], default="stdio",
        help="MCP transport type (default: stdio)"
    )
    args = parser.parse_args()

    if args.hardware:
        HARDWARE_MODE = True
        if not HAS_SERIAL:
            print("Warning: pyserial not installed. Hardware mode requires: pip install pyserial", file=sys.stderr)
    elif args.simulator:
        SIMULATOR_ONLY = True

    _configured_port = args.port

    mode_str = ("hardware" if HARDWARE_MODE
                else "simulator" if SIMULATOR_ONLY
                else "auto (simulator if reachable, else serial hardware)")
    print(f"PicoDeck MCP Server starting in {mode_str} mode (port {_configured_port})...", file=sys.stderr)
    mcp.run(transport=args.transport)


if __name__ == "__main__":
    main()
