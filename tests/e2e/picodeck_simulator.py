"""PicoDeck Simulator control wrapper for E2E testing.

Uses JSON-RPC 2.0 over TCP to communicate with the simulator, matching
the protocol used by the MCP server (tools/picodeck_mcp.py).

Usage:
    with PicodeckSimulator(headless=True) as sim:
        sim.launch_app("hello")
        screenshot = sim.screenshot()
        sim.exit_app()
"""

import base64
import io
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import threading
from collections import deque
from pathlib import Path
from typing import Any, Optional, List, Sequence


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# Sanitizer runtime options for sanitizer builds of the simulator (make
# simulator-asan / simulator-tsan); ignored by a release build. These are the
# audit's (§5.1 R4): every ASan/UBSan report is fatal (abort → the health hook
# sees a dead sim plus the report on stderr), and leak checking stays off
# until there is a suppressions file. TSan keeps going after a report: its
# leg is informational (see tests/e2e/README.md).
SANITIZER_ENV = {
    "ASAN_OPTIONS": "abort_on_error=1:halt_on_error=1:detect_leaks=0",
    "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
    "TSAN_OPTIONS": "halt_on_error=0:second_deadlock_stack=1:suppressions="
                    + str(PROJECT_ROOT / "tests" / "e2e" / "tsan.supp"),
}


def _merge_options(ours: str, theirs: str) -> str:
    """Merge two k=v:k=v sanitizer option strings per key; `theirs` (the
    caller's environment) wins on a clash."""
    merged = {}
    for opts in (ours, theirs):
        for item in filter(None, opts.split(":")):
            key, _, value = item.partition("=")
            merged[key] = value
    return ":".join(f"{k}={v}" for k, v in merged.items())


def sanitizer_env(env: dict) -> dict:
    """`env` with SANITIZER_ENV merged in per option key (options already in
    `env` win, the rest of ours are kept)."""
    for key, value in SANITIZER_ENV.items():
        env[key] = _merge_options(value, env.get(key, ""))
    return env


def default_binary() -> Path:
    """$PICODECK_SIM_BINARY (relative to the cwd, else the repo root) or
    build_sim/picodeck_simulator."""
    override = os.environ.get("PICODECK_SIM_BINARY")
    if not override:
        return PROJECT_ROOT / "build_sim" / "picodeck_simulator"
    path = Path(override).expanduser()
    if not path.is_absolute() and not path.exists():
        path = PROJECT_ROOT / path
    return path.resolve()


_build_info_cache: dict = {}


def binary_build_info(binary) -> Optional[dict]:
    """`picodeck_simulator --build-info` as a dict of its key=value lines
    (sanitize=..., firmware_net=0|1); None when the probe failed (missing
    binary, crash, timeout, no sanitize= line), so callers can tell "no"
    from "don't know"."""
    key = str(binary)
    if key not in _build_info_cache:
        info = {}
        try:
            out = subprocess.run([key, "--build-info"], capture_output=True,
                                 text=True, timeout=30,
                                 env=sanitizer_env(os.environ.copy())).stdout
            for line in out.splitlines():
                k, sep, v = line.partition("=")
                if sep:
                    info[k.strip()] = v.strip()
        except (OSError, subprocess.SubprocessError):
            pass
        if "sanitize" not in info:
            return None  # not cached: the binary may be built later
        _build_info_cache[key] = info
    return _build_info_cache[key]


def binary_sanitizers(binary) -> Optional[str]:
    """The -fsanitize= list a simulator binary was built with ('' for a
    release build), from `picodeck_simulator --build-info`; None when the probe
    failed (missing binary, crash, timeout, no sanitize= line), so callers
    can tell "not sanitized" from "don't know"."""
    info = binary_build_info(binary)
    return None if info is None else info["sanitize"]


def binary_firmware_net(binary) -> Optional[bool]:
    """True if the simulator runs the firmware network stack
    (SIM_FIRMWARE_NET=ON, make simulator-net); None when the probe failed.
    Binaries from before the option report no firmware_net line: False."""
    info = binary_build_info(binary)
    return None if info is None else info.get("firmware_net") == "1"


class PicodeckSimulator:
    """Controls PicoDeck Simulator process for E2E testing via JSON-RPC 2.0."""

    # Project root (two levels up from tests/e2e/)
    PROJECT_ROOT = PROJECT_ROOT
    DEFAULT_BINARY = default_binary()
    DEFAULT_SD_CARD = PROJECT_ROOT / "simulator" / "assets" / "sd_card"

    def __init__(
        self,
        binary_path: Optional[str] = None,
        sd_card_path: Optional[str] = None,
        headless: bool = True,
        tcp_port: int = 0,
        timeout: float = 10.0,
        test_mode: bool = False,
        virtual_time: Optional[bool] = None,
        crash_log_path: Optional[str] = None,
        unix_socket: Optional[str] = "none",
        extra_args: Sequence[str] = (),
    ):
        self.binary_path = binary_path or str(self.DEFAULT_BINARY)
        self.sd_card_path = sd_card_path or str(self.DEFAULT_SD_CARD)
        self.headless = headless
        self.requested_port = tcp_port
        self.tcp_port: Optional[int] = None  # actual port after start
        self.timeout = timeout
        # --test-mode: Lua error screens and launch refusals return at once
        # (their text goes to the log's "err" source) and idle dim is off.
        self.test_mode = test_mode
        # --virtual-time (needs test_mode): the clock the OS and apps see is
        # virtual. sleep_ms(n) advances it by n instead of waiting (paced at
        # set_time_multiplier x real time, default 50x; 0 pauses it for
        # step_time). Off by default: audio playback and network I/O stay on
        # real time, and apps with time-boxed input waits would expire before
        # a test could type. None = $PICODECK_SIM_VIRTUAL_TIME=1 turns it on for
        # every test-mode simulator (a whole-suite experiment knob).
        if virtual_time is None:
            virtual_time = test_mode and os.environ.get(
                "PICODECK_SIM_VIRTUAL_TIME") == "1"
        if virtual_time and not test_mode:
            raise ValueError("virtual_time requires test_mode")
        self.virtual_time = virtual_time
        # --crash-log: where the sim's SIGSEGV/SIGABRT handler writes its
        # backtrace. Read from disk after a crash (a dead sim can't answer
        # get_crash_log). None = the sim's per-PID default under /tmp.
        self.crash_log_path = crash_log_path
        # --unix-socket: "none" (default) keeps parallel instances from
        # binding ./picodeck_control in the cwd; None = the sim's default.
        self.unix_socket = unix_socket
        # More simulator flags, e.g. ["--real-umm"] (device-accurate heap).
        self.extra_args = list(extra_args)
        self.process: Optional[subprocess.Popen] = None
        # Exit status once the process has been reaped (stop() or crash).
        self.returncode: Optional[int] = None
        self._launch_id = 0
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._reader_thread: Optional[threading.Thread] = None
        self._reader_done = threading.Event()
        # Notifications other than `log`, in arrival order, each tagged with
        # a receive index ("_rx") so waits can ignore ones that predate a
        # launch. `log` notifications go to _log_events.
        self._notifications: list[dict] = []
        self._notif_lock = threading.Lock()
        self._notif_cond = threading.Condition(self._notif_lock)
        self._rx_counter = 0
        self._launch_rx = 0
        self._log_events: deque = deque(maxlen=50000)
        self._logs_subscribed = False
        # Test hook: while set, the reader thread stops reading the socket
        # (simulates a client that falls behind).
        self._reader_pause = threading.Event()
        self._pending: dict[int, dict] = {}
        self._pending_lock = threading.Lock()
        self._id_counter = 0
        self._connected = False
        self._recv_buf = ""
        # Bounded tails of the child's stdout/stderr. The pipes MUST be drained
        # continuously (see _start_pipe_drains) or the simulator deadlocks once
        # the 64KB kernel pipe buffer fills; these keep the output available for
        # diagnostics without growing without bound.
        self._stdout_tail: deque = deque(maxlen=2000)
        self._stderr_tail: deque = deque(maxlen=2000)
        # A sanitizer report, captured from its first line by the stderr
        # drain and kept apart from the tail, so later output (C-Dogs fills
        # the tail in under a second; TSan keeps running) can't evict it.
        # Bounded: the drain keeps reading past the cap, it just stops
        # storing, so report volume can never back up the pipe.
        self._sanitizer_report: list[str] = []
        # "[LAUNCHER] WARNING: app cap (N) reached, ignoring '<dir>'" lines,
        # kept apart from the tail for the same reason.
        self._app_cap_drops: list[str] = []
        self._drain_threads: list[threading.Thread] = []

    # ── Context Manager ──────────────────────────────────────────────────────

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    # ── Lifecycle ─────────────────────────────────────────────────────────────

    def start(self):
        """Start the simulator process and connect."""
        cmd = [
            self.binary_path,
            "--sd-card", self.sd_card_path,
            "--port", str(self.requested_port),
        ]
        if self.test_mode:
            cmd.append("--test-mode")
        if self.virtual_time:
            cmd.append("--virtual-time")
        if self.crash_log_path:
            cmd += ["--crash-log", str(self.crash_log_path)]
        if self.unix_socket:
            cmd += ["--unix-socket", str(self.unix_socket)]
        cmd += self.extra_args

        env = sanitizer_env(os.environ.copy())
        if self.headless:
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIODRIVER"] = "dummy"

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )

        # stderr is drained from the first byte: a sanitizer build can write
        # to it before the port line appears (TSan races at startup), and an
        # undrained pipe deadlocks the sim (see _start_pipe_drains).
        self._start_pipe_drains(stdout=False)

        # Parse the actual TCP port from simulator stdout
        self.tcp_port = self._parse_port()

        # From here on nothing else reads stdout, so it must be drained
        # continuously too.
        self._start_pipe_drains(stderr=False)

        # Connect and start reader
        self._connect()

    def stop(self):
        """Gracefully stop the simulator."""
        # Try RPC shutdown first
        if self._connected:
            try:
                self.call("shutdown", timeout=1.0)
            except Exception:
                pass

        self._disconnect()

        if self.process:
            try:
                # The shutdown RPC normally ends the process by itself; only
                # signal it if it is still running after a grace period.
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    self.process.terminate()
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=2)
            self.returncode = self.process.returncode
            self._join_drains()
            self.process = None

    # ── Health ────────────────────────────────────────────────────────────────

    # Sanitizer reports (a sanitizer build prints them to stderr).
    SANITIZER_RE = re.compile(
        r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:")
    # Exit statuses that mean the sim died rather than shut down: a fatal
    # signal (negative), or the crash handler's _exit(128 + signal).
    _FATAL = (signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS, signal.SIGFPE,
              signal.SIGILL)
    CRASH_SIGNALS = {-s for s in _FATAL} | {128 + s for s in _FATAL}

    def is_alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def exit_code(self) -> Optional[int]:
        """The process's exit status if it has exited, else None."""
        if self.process is not None:
            return self.process.poll()
        return self.returncode

    def read_crash_log(self) -> str:
        """The crash handler's backtrace, read from disk ('' if none)."""
        if not self.crash_log_path:
            return ""
        try:
            return Path(self.crash_log_path).read_text(errors="replace")
        except OSError:
            return ""

    # Lines of sanitizer report kept (a whole ASan report with its stacks
    # is well under this; TSan may print many).
    SANITIZER_REPORT_MAX = 600

    def sanitizer_lines(self) -> list[str]:
        """stderr lines that look like a sanitizer report."""
        seen = [l for l in self._sanitizer_report if self.SANITIZER_RE.search(l)]
        return seen or [l for l in list(self._stderr_tail)
                        if self.SANITIZER_RE.search(l)]

    def sanitizer_report(self) -> str:
        """The captured sanitizer report (first line onwards, with stacks)."""
        return "\n".join(self._sanitizer_report)

    APP_CAP_RE = re.compile(r"\[LAUNCHER\] WARNING: app cap \(\d+\) reached")

    def _note_stdout_line(self, line: str):
        """Stdout drain hook: remember launcher app-cap drops."""
        if self.APP_CAP_RE.search(line) and len(self._app_cap_drops) < 20:
            self._app_cap_drops.append(line)

    def _note_stderr_line(self, line: str):
        """Stderr drain hook: start or extend the sticky sanitizer report."""
        if self._sanitizer_report or self.SANITIZER_RE.search(line):
            if len(self._sanitizer_report) < self.SANITIZER_REPORT_MAX:
                self._sanitizer_report.append(line)

    def health_problems(self) -> list[str]:
        """Reasons this sim is unhealthy: the process exited without stop()
        being called (or stop() reaped a crash status), it wrote a crash log,
        or a sanitizer reported on stderr. [] = healthy."""
        problems = []
        if self.process is not None:
            code = self.process.poll()
            if code is not None:
                problems.append(
                    f"simulator process exited unexpectedly with status {code}")
                self._join_drains()
        elif self.returncode in self.CRASH_SIGNALS:
            problems.append(f"simulator died with status {self.returncode}")
        crash = self.read_crash_log()
        if crash.strip():
            problems.append("crash log:\n" + crash.strip())
        san = self.sanitizer_lines()
        if san:
            report = self._sanitizer_report or san
            problems.append("sanitizer report on stderr:\n" + "\n".join(report[:120]))
        if self._app_cap_drops:
            problems.append(
                "the launcher dropped apps at its MAX_APPS cap (which apps "
                "depends on the filesystem's directory order, so a test can "
                "pass on tmpfs and fail elsewhere): the SD card held too many "
                "apps. Ask for a smaller card with @pytest.mark.sd(fixtures="
                "[...], reserve=N); see tests/e2e/README.md, 'App cap'.\n  "
                + "\n  ".join(self._app_cap_drops))
        return problems

    def _join_drains(self, timeout: float = 1.0):
        for t in self._drain_threads:
            t.join(timeout=timeout)

    def _start_pipe_drains(self, stdout: bool = True, stderr: bool = True):
        """Continuously drain the child's stdout and/or stderr.

        The simulator is spawned with stdout=PIPE and stderr=PIPE, but after
        _parse_port() nothing reads them again. Once the kernel pipe buffer
        (64KB) fills, the simulator blocks in write(). That is not a clean
        stall: the blocking write happens inside fflush() on the socket thread
        while it holds the stdio FILE lock, so the main thread then blocks in
        printf() waiting for that lock and the whole simulator deadlocks —
        RPCs stop being answered and every subsequent call times out.

        This showed up as 'App hung on cycle 5/10' in the stress tests: it took
        about five app launches' worth of output to fill the pipe.
        """
        def drain(stream, tail, note=None):
            try:
                for raw in iter(stream.readline, b""):
                    line = raw.decode("utf-8", errors="replace").rstrip("\n")
                    tail.append(line)
                    if note is not None:
                        note(line)
            except (ValueError, OSError):
                pass  # stream closed during shutdown
            finally:
                try:
                    stream.close()
                except Exception:
                    pass

        for wanted, stream, tail, note in (
                (stdout, self.process.stdout, self._stdout_tail,
                 self._note_stdout_line),
                (stderr, self.process.stderr, self._stderr_tail,
                 self._note_stderr_line)):
            if not wanted or stream is None:
                continue
            t = threading.Thread(target=drain, args=(stream, tail, note),
                                 daemon=True)
            t.start()
            self._drain_threads.append(t)

    def get_output(self) -> dict:
        """Recent simulator stdout/stderr, for diagnostics."""
        return {
            "stdout": "\n".join(self._stdout_tail),
            "stderr": "\n".join(self._stderr_tail),
        }

    def _parse_port(self) -> int:
        """Read stdout lines until we find '[Socket] TCP port: N'."""
        deadline = time.time() + self.timeout
        port_re = re.compile(r"\[Socket\] TCP port: (\d+)")

        while time.time() < deadline:
            if self.process.poll() is not None:
                stdout = self.process.stdout.read().decode() if self.process.stdout else ""
                self._join_drains()
                stderr = "\n".join(self._stderr_tail)
                raise RuntimeError(
                    f"Simulator exited early (code {self.process.returncode})\n"
                    f"stdout: {stdout}\nstderr: {stderr}"
                )

            # Non-blocking read from stdout
            line = self.process.stdout.readline().decode("utf-8", errors="replace")
            if not line:
                time.sleep(0.05)
                continue
            self._stdout_tail.append(line.rstrip("\n"))
            self._note_stdout_line(line)

            m = port_re.search(line)
            if m:
                return int(m.group(1))

        # Fallback: if requested_port != 0, try that
        if self.requested_port > 0:
            return self.requested_port
        raise TimeoutError("Could not detect simulator TCP port")

    def _connect(self):
        """Connect TCP and start reader thread."""
        delays = [0.1, 0.2, 0.4, 0.8, 1.6]
        last_err = None
        for i, delay in enumerate(delays):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(self.timeout)
                sock.connect(("127.0.0.1", self.tcp_port))
                self._sock = sock
                self._connected = True
                break
            except (socket.error, ConnectionRefusedError) as e:
                last_err = e
                sock.close()
                if i < len(delays) - 1:
                    time.sleep(delay)

        if not self._connected:
            raise RuntimeError(
                f"Cannot connect to simulator port {self.tcp_port}: {last_err}"
            )

        self._reader_done.clear()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def _disconnect(self):
        """Close socket and reader thread."""
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
        with self._pending_lock:
            for entry in self._pending.values():
                entry["event"].set()
            self._pending.clear()

    # ── Reader Thread ─────────────────────────────────────────────────────────

    def _reader_loop(self):
        """Single reader thread routing all incoming JSON messages."""
        buf = ""
        while not self._reader_done.is_set():
            if not self._sock:
                break
            if self._reader_pause.is_set():
                time.sleep(0.01)
                continue
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
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if "id" in msg and msg["id"] is not None:
                        with self._pending_lock:
                            entry = self._pending.get(msg["id"])
                            if entry:
                                entry["result"] = msg
                                entry["event"].set()
                    else:
                        with self._notif_cond:
                            self._rx_counter += 1
                            msg["_rx"] = self._rx_counter
                            if msg.get("method") == "log":
                                self._log_events.append(msg.get("params", {}))
                            else:
                                self._notifications.append(msg)
                            self._notif_cond.notify_all()
            except socket.timeout:
                continue
            except OSError:
                break
        # The simulator closed the connection (it exited or crashed): no reply
        # will come, so fail the calls still waiting instead of letting each
        # run out its timeout. call() registers under the same lock after
        # checking _connected, so none can start waiting after this.
        self._connected = False
        with self._pending_lock:
            for entry in self._pending.values():
                entry["event"].set()

    # ── JSON-RPC ──────────────────────────────────────────────────────────────

    def call(self, method: str, params: Optional[dict] = None,
             timeout: Optional[float] = None) -> Any:
        """Send a JSON-RPC 2.0 request and return the result."""
        if not self._connected or not self._sock:
            raise RuntimeError("Not connected to simulator")

        self._id_counter += 1
        req_id = self._id_counter

        event = threading.Event()
        with self._pending_lock:
            if not self._connected:
                raise RuntimeError("Not connected to simulator")
            self._pending[req_id] = {"event": event, "result": None}

        payload = json.dumps({
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params or {},
        }) + "\n"

        with self._lock:
            self._sock.sendall(payload.encode())

        t = timeout or self.timeout
        if not event.wait(timeout=t):
            with self._pending_lock:
                self._pending.pop(req_id, None)
            raise TimeoutError(f"Timeout calling {method} (>{t}s)")

        with self._pending_lock:
            entry = self._pending.pop(req_id, None)

        if not entry or not entry["result"]:
            raise RuntimeError(
                f"No response for {method}: the simulator closed the connection")

        msg = entry["result"]
        if "error" in msg:
            err = msg["error"]
            raise RuntimeError(f"RPC error [{err.get('code')}]: {err.get('message')}")
        return msg.get("result", {})

    # ── Notifications ─────────────────────────────────────────────────────────

    def get_notifications(self) -> list[dict]:
        """Return and clear accumulated notifications."""
        with self._notif_lock:
            notifs = self._notifications.copy()
            self._notifications.clear()
        return notifs

    def wait_for_notification(self, method: str, timeout: Optional[float] = None,
                              after_rx: int = 0) -> dict:
        """Wait for (and consume) the first `method` notification received
        after receive index `after_rx`."""
        deadline = time.time() + (timeout or self.timeout)
        with self._notif_cond:
            while True:
                for i, n in enumerate(self._notifications):
                    if n.get("method") == method and n.get("_rx", 0) > after_rx:
                        return self._notifications.pop(i)
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"Notification '{method}' not received within timeout")
                self._notif_cond.wait(remaining)

    # ── High-Level Helpers ────────────────────────────────────────────────────

    def ping(self) -> bool:
        """Check if simulator is responsive."""
        try:
            result = self.call("ping", timeout=2.0)
            # Ping returns {"version":"...", "uptime_ms":N}
            return "version" in result or "uptime_ms" in result
        except Exception:
            return False

    def get_status(self) -> dict:
        """Get simulator status (running app, heap, wifi)."""
        app = self.call("get_running_app")
        heap = self.call("get_heap_info")
        wifi = self.call("get_wifi_state")
        return {"app": app, "heap": heap, "wifi": wifi}

    def list_apps(self) -> list[str]:
        """List available apps on the SD card."""
        result = self.call("list_dir", {"path": "/apps"})
        entries = result.get("entries", [])
        return [e["name"] for e in entries if e.get("is_dir")]

    def launch_app(self, name: str) -> dict:
        """Queue an app launch. Returns {"queued": True, "busy": bool}.

        The launch runs when the launcher loop next runs (after any running
        app exits). An app staged after boot is found by the sim's rescan-on-
        miss. Use wait_for_exit() for the outcome.
        """
        with self._notif_cond:
            self._launch_rx = self._rx_counter
        result = self.call("launch_app", {"name": name})
        self._launch_id = result.get("launch_id", 0)
        return result

    def get_last_outcome(self) -> dict:
        """app.exited params of the last finished launch ({"launch_id": 0}
        before any)."""
        return self.call("get_last_outcome")

    def rescan_apps(self) -> dict:
        """Ask the launcher to rescan /apps (e.g. after changing an app.json)."""
        return self.call("rescan_apps")

    def exit_app(self) -> dict:
        """Exit the currently running app."""
        return self.call("exit_app")

    def wait_for_exit(self, timeout: Optional[float] = None) -> dict:
        """Wait for the app from the last launch_app() to finish.

        Returns the app.exited params:
        {name, id, found, result, error, runtime_ms, launch_id} where result
        is "returned" | "error" | "exit_sentinel" | "load_failed".

        The server drops a notification whole when this client's buffer is
        full (e.g. mid-way through a large response), so while waiting this
        also polls get_last_outcome, which keeps the last app.exited params.
        """
        deadline = time.time() + (timeout or 30.0)
        want = self._launch_id
        while True:
            remaining = deadline - time.time()
            try:
                notif = self.wait_for_notification(
                    "app.exited", timeout=max(0.01, min(remaining, 0.5)),
                    after_rx=self._launch_rx)
                params = notif.get("params", {})
                # Ignore an exit that belongs to an earlier launch.
                if params.get("launch_id", want) >= want:
                    return params
                continue
            except TimeoutError:
                pass
            last = self.get_last_outcome()
            if want and last.get("launch_id", 0) >= want:
                return last
            if time.time() >= deadline:
                raise TimeoutError(
                    f"app.exited for launch {want} not received within timeout "
                    f"(last outcome: {last})")

    # Named buttons recognized by inject_button
    _BUTTONS = {
        "up", "down", "left", "right", "enter", "esc", "escape",
        "menu", "sym", "backspace", "tab", "del", "shift",
        "f1", "f2", "f3", "f4", "f5",
    }

    def keypress(self, key: str) -> dict:
        """Send a keypress to the simulator.

        Named keys: up, down, left, right, enter, esc, menu, f1-f5,
        backspace, tab, del, shift.
        Character keys: a-z, A-Z, 0-9, space, punctuation — sent via inject_char.
        """
        if key.lower() in self._BUTTONS:
            return self.call("inject_button", {"button": key, "action": "click"})
        elif len(key) == 1:
            return self.call("inject_char", {"char": key})
        else:
            return self.call("inject_button", {"button": key, "action": "click"})

    def pad(self, state: str, hold_ms: int = 0) -> str:
        """Set the test gamepad source through the `pad` dev command (the
        firmware's handler, src/dev_ops.c): buttons joined by '+' ("up+a",
        "home"), "none" (nothing held, still connected) or "off"
        (disconnected). Held until the next call, or for hold_ms from the
        first poll that sees it. Returns the reply ("Pad: up+a"); raises on
        an error reply."""
        cmd = f"pad {state}" + (f" {int(hold_ms)}" if hold_ms else "")
        # The server gives up before this client does, so a non-polling app
        # gets an error reply and never keeps the socket thread blocked.
        r = self.call("dev_command",
                      {"cmd": cmd, "timeout_ms": int(self.timeout * 800)})
        if not r.get("ok"):
            raise RuntimeError(f"{cmd!r}: {r.get('output')}")
        return r.get("output", "")

    def get_input_state(self) -> dict:
        """{issued_seq, consumed_seq}: every injection with seq <= consumed_seq
        has been read by the OS input layer."""
        return self.call("get_input_state")

    def wait_input_consumed(self, seq: int, timeout: Optional[float] = None) -> dict:
        """Wait until the injection with `seq` (the input_seq returned by
        keypress/inject_*) has been read by the OS."""
        deadline = time.time() + (timeout or self.timeout)
        while True:
            state = self.get_input_state()
            if state.get("consumed_seq", 0) >= seq:
                return state
            if time.time() >= deadline:
                raise TimeoutError(f"input seq {seq} not consumed: {state}")
            time.sleep(0.01)

    def present_count(self) -> int:
        """Frames presented to the (simulated) panel since boot."""
        return self.call("display_stats").get("present_count", 0)

    def wait_frames(self, n: int = 1, timeout: Optional[float] = None) -> int:
        """Wait until `n` more frames have been presented. Returns the new
        present_count."""
        target = self.present_count() + n
        deadline = time.time() + (timeout or self.timeout)
        while True:
            count = self.present_count()
            if count >= target:
                return count
            if time.time() >= deadline:
                raise TimeoutError(f"only {count - target + n} of {n} frames presented")
            time.sleep(0.01)

    def keypress_sequence(self, keys: list[str], delay_ms: int = 100):
        """Send a sequence of keypresses with delays."""
        for key in keys:
            self.keypress(key)
            time.sleep(delay_ms / 1000.0)

    def screenshot(self) -> bytes:
        """Take a screenshot and return PNG bytes."""
        result = self.call("screenshot")
        png_b64 = result.get("data", "") or result.get("png_base64", "")
        return base64.b64decode(png_b64)

    def screenshot_pil(self):
        """Take a screenshot and return a PIL Image (requires Pillow)."""
        from PIL import Image
        png_data = self.screenshot()
        return Image.open(io.BytesIO(png_data))

    def get_log_buffer(self, since_seq: int = 0, tail: int = 0) -> dict:
        """Log entries with seq >= since_seq (0 = everything still held).

        Returns {lines: [{seq, t_ms, src, text}], next_seq, dropped, more}.
        src is "lua" | "native" | "os" | "err". Pass next_seq back as
        since_seq to read only newer entries; "more" means the response was
        paged. tail > 0 returns only the newest `tail` entries.
        """
        params = {"since_seq": since_seq}
        if tail:
            params["tail"] = tail
        return self.call("get_log_buffer", params)

    def get_log_lines(self, since_seq: int = 0) -> list[dict]:
        """Every held entry with seq >= since_seq, following pages."""
        return self._read_log(since_seq)[0]

    def _read_log(self, since_seq: int) -> tuple[list[dict], int]:
        out: list[dict] = []
        seq = since_seq
        while True:
            page = self.get_log_buffer(since_seq=seq)
            out.extend(page.get("lines", []))
            seq = page.get("next_seq", seq)
            if not page.get("more"):
                return out, seq

    def clear_log(self) -> dict:
        """Clear the simulator log buffer (sequence numbers keep counting)."""
        return self.call("clear_log_buffer")

    def subscribe_logs(self, on: bool = True) -> dict:
        """Receive every new log entry as a `log` notification."""
        result = self.call("subscribe", {"logs": on})
        self._logs_subscribed = on
        return result

    def wait_for_log(self, pattern: str, timeout: Optional[float] = None,
                     since_seq: int = 0, src: Optional[str] = None) -> str:
        """Wait until a log entry matching the regex appears (optionally only
        from source `src`) and return its text.

        Event-driven: subscribes to log notifications, then checks the entries
        already held from since_seq on (0 = all), then waits for new ones.
        """
        regex = re.compile(pattern)

        def match(entry: dict) -> bool:
            if src is not None and entry.get("src") != src:
                return False
            return bool(regex.search(entry.get("text", "")))

        if not self._logs_subscribed:
            self.subscribe_logs(True)
        deadline = time.time() + (timeout or self.timeout)
        # Held entries first; later ones arrive as notifications (the
        # subscription is already active, so nothing falls in between).
        held, cursor = self._read_log(since_seq)
        for entry in held:
            if match(entry):
                return entry.get("text", "")

        while True:
            timed_out = False
            with self._notif_cond:
                fresh = self._log_events_since(cursor)
                if not fresh:
                    remaining = deadline - time.time()
                    if remaining <= 0:
                        timed_out = True
                    else:
                        self._notif_cond.wait(remaining)
                        fresh = self._log_events_since(cursor)
            # The server drops `log` notifications whole when this client's
            # write buffer is full, so a seq gap means entries were missed:
            # backfill them from the ring (outside the lock — the RPC reply
            # is delivered by the reader thread). Also backfill once before
            # giving up, in case the matching line's notification was lost.
            seqs = [e.get("seq", 0) for e in fresh]
            gap = bool(fresh) and (seqs[0] != cursor or
                                   any(b - a != 1 for a, b in zip(seqs, seqs[1:])))
            if gap or timed_out:
                fresh, cursor = self._read_log(cursor)
            elif fresh:
                cursor = seqs[-1] + 1
            for entry in fresh:
                if match(entry):
                    return entry.get("text", "")
            if timed_out:
                raise TimeoutError(f"Log pattern '{pattern}' not found within timeout")

    def _log_events_since(self, cursor: int) -> list[dict]:
        """Pushed log entries with seq >= cursor, oldest first. Scans from the
        newest end only as far as needed. Caller holds _notif_cond."""
        out = []
        for entry in reversed(self._log_events):
            if entry.get("seq", 0) < cursor:
                break
            out.append(entry)
        out.reverse()
        return out

    def get_terminal_buffer(self) -> dict:
        """Get the active terminal's text buffer."""
        return self.call("get_terminal_buffer")

    def get_heap_info(self) -> dict:
        """Get heap usage info."""
        return self.call("get_heap_info")

    def set_time_multiplier(self, multiplier: float) -> dict:
        """Set the speed of simulated time.

        With --virtual-time: virtual time runs at `multiplier` x real time
        while the OS sleeps (sleep_ms(n) advances the clock by n and waits
        n / multiplier real ms; default 50), stretches without a sleep past
        50 ms advance it at min(multiplier, 1) x real time, and 0 pauses the
        clock: sleeps then return only as step_time() moves it.

        Without --virtual-time: it only scales hal_sleep_ms() delays; the
        clock Lua sees (sys.sleep, getTimeMs) stays the wall clock."""
        return self.call("set_time_multiplier", {"multiplier": multiplier})

    def step_time(self, ms: int) -> dict:
        """Advance the virtual clock by `ms` (--virtual-time only; the sim
        refuses it on the wall clock). Returns {"now_ms": N}, the clock
        after the step; step_time(0) reads it."""
        return self.call("step_time", {"ms": int(ms)})

    # ── Build Helper ──────────────────────────────────────────────────────────

    @classmethod
    def build(cls, jobs: int = 4) -> Path:
        """Build the simulator, returning the binary path."""
        build_dir = cls.PROJECT_ROOT / "build_sim"
        build_dir.mkdir(exist_ok=True)

        subprocess.run(
            ["cmake", "-B", str(build_dir), str(cls.PROJECT_ROOT / "simulator")],
            cwd=str(cls.PROJECT_ROOT),
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["make", "-C", str(build_dir), f"-j{jobs}"],
            check=True,
            capture_output=True,
        )
        return build_dir / "picodeck_simulator"
