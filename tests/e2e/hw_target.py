"""Where an E2E test runs: the simulator (default) or a real PicoCalc.

    pytest tests/e2e                                            # simulator
    pytest tests/e2e --target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00

A test asks for the `target` fixture (conftest.py) and gets a SimTarget over
its simulator, or the session's HwTarget on --target hw:<port>. Both offer the
same calls: launch_app / wait_for_exit / exit_app, keypress(_sequence),
screenshot, read_file / write_file / delete_file, push_app / stage_lua_app,
run_lua_app / wait_for_results (the Lua test kit's results file), status,
log_cursor / get_log_lines / wait_for_log.

Collection rules (apply_target_rules):
  - @pytest.mark.hardware: runs only on --target hw; skipped (allow-listed)
    on the simulator.
  - @pytest.mark.both: runs on either; must use `target`, never the
    simulator fixtures.
  - unmarked tests are simulator-only: deselected on --target hw.

HwTarget drives the device over USB serial with tools/picodeck_mcp.py's helpers
(do_command_hardware, do_get_file_b64, do_put_file_b64,
do_screenshot_hardware, do_keysequence_hardware, push_app) and encodes the
device's known traps (see the task-27 report and CLAUDE.md "Debug"):

  - Serial capture drops [APP] lines, so results come from
    /data/<APP_ID>/test_results.json (picotest) and outcomes from `status`
    polls plus /system/error.log growth. The log is advisory only.
  - The launcher caches app.json at boot: push_app reboots when the pushed
    manifest (id, name, requirements, min_psram_kb, system_clock_khz)
    differs from the one on the card, or when `list` does not show the app.
  - Dev commands while an app runs (src/os/lua_bridge.c lua_bridge_service,
    src/main.c sys_poll, src/os/launcher.c): `reboot` and `reboot-flash`
    are HONOURED mid-app (Lua hook / sys.sleep pass, native sys->poll; a
    native app that never polls leaves them latched for the launcher);
    `reboot-ota` is DROPPED ("reboot-ota ignored"), so an OTA flash staged
    mid-app never applies; `usb` is DEFERRED until the launcher. Rebooting
    mid-app kills the app with no teardown, so reboot() exits to the
    launcher first (or refuses) and flash() refuses while an app runs.
  - `ver`'s timestamp comes from main.c and lies after incremental builds:
    nothing here trusts it; a reboot is proven by uptime going back.
  - Two readers split the device's output: preflight refuses a port another
    process holds (stop the MCP server's log capture first).
  - Every wait is bounded; a device that does not answer fails fast.
"""

from __future__ import annotations

import asyncio
import collections
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import types
from pathlib import Path
from typing import Callable, Optional

E2E_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = E2E_DIR.parent.parent
TOOLS_DIR = PROJECT_ROOT / "tools"
TEST_KIT = E2E_DIR / "lib" / "picotest.lua"
HW_APPS_DIR = E2E_DIR / "hw_apps"

HW_SKIP_REASON = ("hardware: needs a PicoCalc "
                  "(--target hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_*-if00)")

# Fixtures that start or wrap a simulator: a `both` test must not use them.
SIM_FIXTURES = {"simulator", "sim_factory", "sim_module", "sim_module_factory",
                "lua_suite", "test_sd_card", "simulator_binary"}


class HwTargetError(RuntimeError):
    """The device did not do what the backend asked (with the evidence)."""


# ── picodeck_mcp ───────────────────────────────────────────────────────────────

_pm = None


def _install_fastmcp_stub():
    """picodeck_mcp imports mcp.server.fastmcp for its MCP server. The helpers
    used here do not need it, so without the package installed a stand-in
    whose tool() decorator returns the function unchanged is enough."""
    class FastMCP:
        def __init__(self, *a, **k):
            pass

        def tool(self, *a, **k):
            return lambda fn: fn

        def run(self, *a, **k):
            raise RuntimeError("mcp is not installed (hw_target stub)")

    class Image:
        def __init__(self, *a, **k):
            pass

    for name in ("mcp", "mcp.server", "mcp.server.fastmcp"):
        sys.modules.setdefault(name, types.ModuleType(name))
    sys.modules["mcp.server.fastmcp"].FastMCP = FastMCP
    sys.modules["mcp.server.fastmcp"].Image = Image


def load_picodeck_mcp():
    """tools/picodeck_mcp.py as a module (shared with anything else that
    imported it as `picodeck_mcp`)."""
    global _pm
    if _pm is not None:
        return _pm
    if "picodeck_mcp" in sys.modules:
        _pm = sys.modules["picodeck_mcp"]
        return _pm
    try:
        import mcp.server.fastmcp  # noqa: F401
    except ImportError:
        _install_fastmcp_stub()
    spec = importlib.util.spec_from_file_location("picodeck_mcp",
                                                  TOOLS_DIR / "picodeck_mcp.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["picodeck_mcp"] = mod
    spec.loader.exec_module(mod)
    _pm = mod
    return mod


# ── Dev-console reply parsers ───────────────────────────────────────────────

_STATUS_RE = re.compile(
    r"\[DEV\] Status: app=(.*?) app_uptime_ms=(\d+) uptime_ms=(\d+) "
    r"wifi=(\S+) sd=(\S+) battery=(-?\d+)")


def parse_status(lines) -> Optional[dict]:
    """The `status` reply (src/dev_commands.c), or None if it is missing."""
    for line in lines:
        m = _STATUS_RE.search(line)
        if m:
            return {"app": m.group(1), "app_uptime_ms": int(m.group(2)),
                    "uptime_ms": int(m.group(3)), "wifi": m.group(4),
                    "sd": m.group(5), "battery": int(m.group(6))}
    return None


def parse_exit_reply(lines) -> tuple:
    """(ok, message) for the `exit` reply (src/dev_ops.c dev_op_exit):
    True "Exit requested: <app>", False "Error: exit: no app running",
    (None, "") when the reply is missing."""
    for line in lines:
        if "[DEV] Exit requested:" in line:
            return True, line.split("[DEV] ", 1)[1].strip()
        if "[DEV] Error: exit:" in line:
            return False, line.split("[DEV] ", 1)[1].strip()
    return None, ""


def parse_launch_reply(lines) -> tuple:
    """("launched" | "not_found" | None, line) for `launch <arg>`
    (src/os/launcher.c launcher_launch_by_name)."""
    for line in lines:
        if "[DEV] Launching app" in line:
            return "launched", line.strip()
        if "[DEV] Error: app '" in line or "[DEV] Error: no app name" in line:
            return "not_found", line.strip()
    return None, ""


def parse_list(lines) -> list:
    """[(name, id)] from the `list` reply."""
    out = []
    for line in lines:
        m = re.match(r"^  (.*)  \(([^()]*)\)\s*$", line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def parse_stack(lines) -> Optional[dict]:
    """The `stack` reply: msp_peak, msp_size, core1_*, app, app_peak,
    app_size, os_cmd_* (ints where numeric)."""
    for line in lines:
        if "[DEV] Stack:" in line:
            out = {}
            for k, v in re.findall(r"(\w+)=(\S+)", line.split("Stack:", 1)[1]):
                out[k] = int(v) if v.isdigit() else v
            return out
    return None


def parse_target_spec(spec: str) -> tuple:
    """"sim" -> ("sim", None); "hw:<absolute serial path>" -> ("hw", path)."""
    if spec == "sim":
        return "sim", None
    if spec.startswith("hw:") and spec[3:].startswith("/"):
        return "hw", spec[3:]
    raise ValueError(f"--target {spec!r}: expected 'sim' or 'hw:<absolute "
                     "serial port path>' (e.g. hw:/dev/serial/by-id/"
                     "usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00)")


def pixel(png: bytes, x: int, y: int) -> tuple:
    """(r, g, b) of one pixel of a PNG screenshot."""
    from PIL import Image
    return tuple(Image.open(io.BytesIO(png)).convert("RGB").getpixel((x, y)))


# ── Port preflight ──────────────────────────────────────────────────────────


def port_holders(port: str) -> list:
    """[(pid, command line)] of other processes that have the port open."""
    real = os.path.realpath(port)
    me = os.getpid()
    found = []
    if not os.path.isdir("/proc"):
        return found
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == me:
            continue
        try:
            fds = os.listdir(f"/proc/{pid}/fd")
        except OSError:
            continue
        for fd in fds:
            try:
                if os.readlink(f"/proc/{pid}/fd/{fd}") == real:
                    try:
                        cmd = Path(f"/proc/{pid}/cmdline").read_bytes()
                        cmd = cmd.replace(b"\0", b" ").decode(errors="replace").strip()
                    except OSError:
                        cmd = "?"
                    found.append((int(pid), cmd))
                    break
            except OSError:
                continue
    return found


def preflight_port(port: str):
    """Refuse, without opening it, a path that is not a free serial tty."""
    if not os.path.exists(port):
        raise HwTargetError(f"{port} does not exist (device unplugged, in "
                            "BOOTSEL, or in USB-MSC mode?)")
    real = os.path.realpath(port)
    name = os.path.basename(real)
    if os.path.isdir("/sys/class/tty"):
        if not os.path.exists(f"/sys/class/tty/{name}/device"):
            raise HwTargetError(f"{port} is not a serial tty ({real})")
    holders = port_holders(port)
    if holders:
        who = "; ".join(f"pid {p}: {c[:80]}" for p, c in holders)
        raise HwTargetError(
            f"{port} is already open by another process ({who}). Two readers "
            "split the device's output: stop that capture first (MCP: "
            "stop_log_capture(device=...), or quit the terminal program)")
    try:
        import serial  # noqa: F401
    except ImportError:
        raise HwTargetError("pyserial is not installed (pip install pyserial)")


# ── Common target API ───────────────────────────────────────────────────────


class Target:
    """What tests may use on both targets (see the module docstring)."""

    kind = "?"
    poll_interval = 0.5

    def wait_for_results(self, app_id: str, timeout: float = 30.0,
                         until: Optional[Callable[[dict], bool]] = None) -> dict:
        """Poll /data/<app_id>/test_results.json until `until(doc)` (default:
        doc["done"]) holds; returns the document."""
        path = f"/data/{app_id}/test_results.json"
        until = until or (lambda d: bool(d.get("done")))
        deadline = time.monotonic() + timeout
        last = None
        while True:
            try:
                doc = json.loads(self.read_file(path))
                last = doc
                if until(doc):
                    return doc
            except (FileNotFoundError, ValueError):
                pass  # not written yet, or caught mid-rewrite
            if time.monotonic() >= deadline:
                raise TimeoutError(f"{path}: condition not met within "
                                   f"{timeout}s (last: {last})")
            time.sleep(self.poll_interval)

    def stage_lua_app(self, name: str, code: str, requirements=(),
                      id: Optional[str] = None, files: Optional[dict] = None):
        """Write app.json + main.lua (+ files) and push them as /apps/<name>."""
        from helpers import stage_lua_app
        with tempfile.TemporaryDirectory() as tmp:
            app_dir = stage_lua_app(Path(tmp), name, code, requirements, id, files)
            return self.push_app(app_dir, name)

    def read_json(self, path: str) -> dict:
        return json.loads(self.read_file(path))


class SimTarget(Target):
    """The simulator behind the Target API (files through its SD directory)."""

    kind = "sim"
    poll_interval = 0.05

    def __init__(self, sim):
        self.sim = sim
        self.sd = Path(sim.sd_card_path).resolve()

    def _host(self, path: str) -> Path:
        if not path.startswith("/"):
            raise ValueError(f"SD path must be absolute: {path!r}")
        p = (self.sd / path.lstrip("/")).resolve()
        if p != self.sd and self.sd not in p.parents:
            raise ValueError(f"{path!r} escapes the SD card")
        return p

    def status(self) -> dict:
        """The subset of the device's `status` the simulator can answer."""
        app = self.sim.call("get_running_app")
        wifi = self.sim.call("get_wifi_state")
        return {"app": app.get("name") or "launcher", "wifi": wifi.get("status")}

    def _ensure_launcher(self, timeout: float = 15.0):
        """Like HwTarget.ensure_launcher: exit a running app and wait."""
        if not self.sim.call("get_running_app").get("running"):
            return
        self.sim.exit_app()
        deadline = time.monotonic() + timeout
        while self.sim.call("get_running_app").get("running"):
            if time.monotonic() >= deadline:
                raise HwTargetError(f"app did not exit within {timeout}s")
            time.sleep(self.poll_interval)

    def launch_app(self, name: str, timeout: float = 10.0) -> dict:
        """Launch from the launcher (a running app is exited first, as on
        the device). {"launched": False} when the simulator cannot find the
        app: its outcome for this launch says found=false. {"launched":
        True} once the app runs, or has already finished having been found."""
        self._ensure_launcher()
        r = self.sim.launch_app(name)
        want = r.get("launch_id", 0)
        deadline = time.monotonic() + timeout
        while True:
            last = self.sim.get_last_outcome()
            if want and last.get("launch_id", 0) >= want:
                found = bool(last.get("found", True))
                return {"launched": found,
                        "line": f"launch {want}: found={found} "
                                f"result={last.get('result')}"}
            if self.sim.call("get_running_app").get("running"):
                return {"launched": True, "line": f"launch {want}: running"}
            if time.monotonic() >= deadline:
                raise TimeoutError(f"launch {name!r} (id {want}) neither "
                                   f"started nor finished within {timeout}s")
            time.sleep(0.02)

    def wait_for_exit(self, timeout: float = 30.0) -> dict:
        return self.sim.wait_for_exit(timeout=timeout)

    def exit_app(self) -> dict:
        r = self.sim.exit_app()
        return {"ok": bool(r.get("ok")), "message": r.get("message", "")}

    def keypress(self, key: str):
        return self.sim.keypress(key)

    def keypress_sequence(self, keys, delay_ms: int = 100):
        return self.sim.keypress_sequence(list(keys), delay_ms)

    def screenshot(self) -> bytes:
        return self.sim.screenshot()

    def read_file(self, path: str) -> bytes:
        p = self._host(path)
        if not p.is_file():
            raise FileNotFoundError(path)
        return p.read_bytes()

    def write_file(self, path: str, data: bytes):
        p = self._host(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def delete_file(self, path: str) -> bool:
        p = self._host(path)
        if p.is_dir():
            shutil.rmtree(p)
            return True
        if p.exists():
            p.unlink()
            return True
        return False

    def push_app(self, local_dir, name: Optional[str] = None) -> dict:
        src = Path(local_dir)
        name = name or src.name
        dest = self.sd / "apps" / name
        if dest.resolve() != src.resolve():
            shutil.copytree(src, dest, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__"))
        self.sim.rescan_apps()
        return {"pushed": str(dest), "rebooted": False}

    def ensure_test_kit(self):
        pass  # the SD manifest stages picotest.lua

    def log_cursor(self) -> int:
        return self.sim.get_log_buffer(tail=1).get("next_seq", 0)

    def get_log_lines(self, since: int = 0) -> list:
        return [e.get("text", "") for e in self.sim.get_log_lines(since)]

    def wait_for_log(self, pattern: str, timeout: float = 10.0, since: int = 0) -> str:
        return self.sim.wait_for_log(pattern, timeout=timeout, since_seq=since)

    def run_lua_app(self, name: str, timeout: float = 30.0):
        from helpers import run_lua_app
        return run_lua_app(self.sim, name, timeout=timeout)


class _CountingDeque(collections.deque):
    """The monitor's ring buffer, counting every line ever appended, so a
    cursor survives the ring wrapping."""

    def __init__(self, iterable=(), maxlen=None):
        super().__init__(iterable, maxlen)
        self.total = len(self)

    def append(self, x):
        super().append(x)
        self.total += 1


def _manifest_key(m: Optional[dict]):
    """Fields the launcher caches at boot and uses to launch the app (so a
    change in any of them means the on-device cache is stale and the device
    must reboot before the new app.json takes effect): id, name,
    requirements, min_psram_kb, system_clock_khz. Cosmetic-only fields
    (description, author, version, category) are deliberately excluded."""
    if m is None:
        return None
    return (m.get("id"), m.get("name"), sorted(m.get("requirements") or []),
            m.get("min_psram_kb"), m.get("system_clock_khz"))


class HwTarget(Target):
    """A PicoCalc on a serial port (see the module docstring for the traps)."""

    kind = "hw"

    def __init__(self, port: str, *, preflight: bool = True,
                 poll_interval: float = 0.5, boot_timeout: float = 60.0,
                 exit_timeout: float = 15.0, connect_timeout: float = 5.0,
                 command_timeout: float = 5.0):
        self.pm = load_picodeck_mcp()
        self.port = port
        self.preflight = preflight
        self.poll_interval = poll_interval
        self.boot_timeout = boot_timeout
        self.exit_timeout = exit_timeout
        self.connect_timeout = connect_timeout
        self.command_timeout = command_timeout
        self._mon = None
        self._launch: Optional[dict] = None
        self._manifests: dict = {}
        self._last_uptime: Optional[int] = None

    # -- connection --

    def _port_present(self) -> bool:
        return os.path.exists(self.port)

    def _start_monitor(self):
        mon, _ = self.pm._get_or_start_monitor(self.port)
        if not isinstance(mon.buffer, _CountingDeque):
            mon.buffer = _CountingDeque(mon.buffer, maxlen=mon.buffer.maxlen)
        self._mon = mon

    def _stop_monitor(self):
        with self.pm._hw_monitors_lock:
            mon = self.pm._hw_monitors.pop(self.port, None)
        if mon:
            mon.stop()
        self._mon = None

    def connect(self):
        """Open the port (persistent log capture) and require a pong."""
        if self.preflight:
            preflight_port(self.port)
        try:
            self._start_monitor()
        except Exception as e:
            raise HwTargetError(f"cannot open {self.port}: {e}") from e
        if not self.ping(timeout=self.connect_timeout):
            self._stop_monitor()
            raise HwTargetError(
                f"{self.port}: no answer to ping within {self.connect_timeout}s. "
                "Is PicoDeck running (not BOOTSEL / USB-MSC), current enough to "
                "have the dev console, and is nothing else reading the port?")
        return self

    def close(self):
        self._stop_monitor()

    # -- dev console --

    def command(self, cmd: str, timeout: Optional[float] = None) -> list:
        try:
            return self.pm.do_command_hardware(cmd, self.port,
                                               timeout or self.command_timeout)
        except HwTargetError:
            raise
        except Exception as e:
            raise HwTargetError(f"{cmd!r} on {self.port}: {e}") from e

    def ping(self, timeout: float = 3.0) -> bool:
        try:
            return any("pong" in ln for ln in self.command("ping", timeout))
        except HwTargetError:
            return False

    def status(self, retries: int = 3) -> dict:
        """The `status` reply; retried because a reply can be dropped."""
        last = []
        for _ in range(retries):
            last = self.command("status", timeout=3.0)
            st = parse_status(last)
            if st:
                self._last_uptime = st["uptime_ms"]
                return st
        raise HwTargetError(f"no Status reply in {retries} tries "
                            f"(firmware without `status`?); last: {last}")

    def stack(self) -> Optional[dict]:
        return parse_stack(self.command("stack", timeout=2.0))

    def ensure_launcher(self, timeout: Optional[float] = None):
        """Exit any running app and wait until the launcher is back."""
        st = self.status()
        if st["app"] == "launcher":
            return
        self.command("exit", timeout=3.0)
        deadline = time.monotonic() + (timeout or self.exit_timeout)
        while time.monotonic() < deadline:
            time.sleep(self.poll_interval)
            try:
                if self.status()["app"] == "launcher":
                    return
            except HwTargetError:
                continue
        raise HwTargetError(f"app {st['app']!r} did not exit within "
                            f"{timeout or self.exit_timeout}s")

    # -- apps --

    def launch_app(self, name: str) -> dict:
        """Launch /apps/<name> (dir, id or display name) from the launcher.
        Any running app is exited first so the outcome is this launch's."""
        self.ensure_launcher()
        errlog0 = self._read_optional("/system/error.log")
        st0 = self.status()
        lines = self.command(f"launch {name}", timeout=10.0)
        kind, line = parse_launch_reply(lines)
        if kind is None:
            # The reply was not captured: the app is running if status says so.
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                st = self.status()
                if st["app"] != "launcher":
                    kind, line = "launched", f"(status: app={st['app']})"
                    break
                time.sleep(self.poll_interval)
        if kind is None:
            raise HwTargetError(
                f"launch {name}: no 'Launching' reply and status still shows "
                f"the launcher (reply lines: {lines}); the app may have run "
                "and exited already: check /system/error.log")
        self._launch = {"name": name, "kind": kind, "line": line,
                        "errlog0": errlog0, "uptime0": st0["uptime_ms"],
                        "exit_sent": False}
        return {"launched": kind == "launched", "line": line}

    def wait_for_exit(self, timeout: float = 30.0) -> dict:
        """Poll `status` until the launcher is back. Returns
        {name, found, result, error, runtime_ms} where result is
        "returned" | "error" (error.log grew) | "exit_sentinel" (after
        exit_app) | "load_failed" | "device_rebooted" (uptime went back)."""
        L = self._launch
        if L is None:
            raise HwTargetError("wait_for_exit without a launch_app")
        out = {"name": L["name"], "found": True, "error": None, "runtime_ms": 0}
        if L["kind"] == "not_found":
            self._launch = None
            return {**out, "found": False, "result": "load_failed",
                    "error": L["line"]}
        prev_uptime = L["uptime0"]
        last = None
        deadline = time.monotonic() + timeout
        while True:
            try:
                st = self.status()
            except HwTargetError:
                st = None
            if st:
                if st["uptime_ms"] < prev_uptime:
                    self._launch = None
                    ev = self.crash_evidence()
                    return {**out, "result": "device_rebooted",
                            "error": ev["crashlog"][-2000:] or "(no crashlog)",
                            "evidence": ev}
                prev_uptime = st["uptime_ms"]
                if st["app"] == "launcher":
                    break
                last = st
            if time.monotonic() >= deadline:
                raise TimeoutError(f"app {L['name']!r} still running after "
                                   f"{timeout}s (status: {last})")
            time.sleep(self.poll_interval)
        self._launch = None
        if last:
            out["runtime_ms"] = last["app_uptime_ms"]
        errlog = self._read_optional("/system/error.log")
        base = L["errlog0"]
        new = errlog[len(base):] if errlog.startswith(base) else errlog
        new_text = new.decode("utf-8", errors="replace").strip()
        if new_text:
            return {**out, "result": "error", "error": new_text[-2000:]}
        return {**out, "result": "exit_sentinel" if L["exit_sent"] else "returned"}

    def exit_app(self) -> dict:
        ok, msg = parse_exit_reply(self.command("exit", timeout=3.0))
        if ok and self._launch:
            self._launch["exit_sent"] = True
        return {"ok": ok, "message": msg}

    def keypress(self, key: str):
        return self.keypress_sequence([key])

    def keypress_sequence(self, keys, delay_ms: int = 100) -> list:
        """Inject keys ("a", "enter", chords like "ctrl+x") in order."""
        try:
            results = self.pm.do_keysequence_hardware(list(keys), self.port,
                                                      delay_ms)
        except Exception as e:
            raise HwTargetError(f"keypress {keys}: {e}") from e
        bad = [r for r in results if not r.endswith(": ok")]
        if bad:
            raise HwTargetError(f"keys not injected: {bad}")
        return results

    def screenshot(self) -> bytes:
        """PNG of the framebuffer the panel shows (RGB565 as display.c
        stores it, decoded big-endian). CDC ports use the binary transfer,
        UART adapters the base64 one."""
        real = os.path.realpath(self.port)
        try:
            if "ttyUSB" in real or "usbserial" in real:
                return self.pm.do_screenshot_b64(self.port)
            return self.pm.do_screenshot_hardware(self.port, timeout=10.0)
        except Exception as e:
            raise HwTargetError(f"screenshot: {e}") from e

    # -- files --

    def read_file(self, path: str) -> bytes:
        try:
            return self.pm.do_get_file_b64(self.port, path)
        except FileNotFoundError:
            raise
        except Exception as e:
            raise HwTargetError(f"read {path}: {e}") from e

    def _read_optional(self, path: str) -> bytes:
        try:
            return self.read_file(path)
        except FileNotFoundError:
            return b""

    def write_file(self, path: str, data: bytes):
        try:
            self.pm.do_put_file_b64(self.port, bytes(data), path)
        except Exception as e:
            raise HwTargetError(f"write {path}: {e}") from e

    def delete_file(self, path: str) -> bool:
        return any("Deleted:" in ln for ln in self.command(f"rm {path}", 5.0))

    def crash_evidence(self) -> dict:
        def text(p):
            try:
                return self._read_optional(p).decode("utf-8", errors="replace")
            except HwTargetError as e:
                return f"(unreadable: {e})"
        return {"crashlog": text("/system/crashlog.txt"),
                "error_log": text("/system/error.log")[-2000:],
                "running": text("/system/running.txt")}

    def ensure_test_kit(self):
        """Put the current picotest.lua at /system/lib/ if it differs."""
        kit = TEST_KIT.read_bytes()
        if self._read_optional("/system/lib/picotest.lua") != kit:
            self.write_file("/system/lib/picotest.lua", kit)

    # -- staging --

    def list_apps(self) -> list:
        return parse_list(self.command("list", timeout=5.0))

    def push_app(self, local_dir, name: Optional[str] = None) -> dict:
        """Push a directory to /apps/<name> (one zip, extracted on device).
        Reboots when the launcher's boot-time cache cannot know the app as
        pushed: a new or changed manifest, or an id `list` does not show."""
        src = Path(local_dir)
        name = name or src.name
        man_path = src / "app.json"
        manifest = json.loads(man_path.read_text()) if man_path.exists() else None
        before = self._read_optional(f"/apps/{name}/app.json")
        try:
            old = json.loads(before) if before else None
        except ValueError:
            old = None
        self.ensure_launcher()
        msg = asyncio.run(self.pm.push_app(str(src), name, device=self.port))
        if not msg.startswith("Pushed"):
            raise HwTargetError(f"push_app {name}: {msg}")
        why = None
        if _manifest_key(old) != _manifest_key(manifest):
            why = "app.json is new or changed"
        elif manifest and manifest.get("id") not in [i for _, i in self.list_apps()]:
            why = "the launcher does not list it"
        if why:
            self.reboot()
            if manifest and manifest.get("id") not in [i for _, i in self.list_apps()]:
                raise HwTargetError(f"{name} ({manifest.get('id')}) still not "
                                    "listed after a reboot")
        self._manifests[name] = manifest
        return {"pushed": msg.splitlines()[0], "rebooted": why is not None,
                "why": why}

    def _app_id(self, name: str) -> str:
        man = self._manifests.get(name)
        if man is None:
            try:
                man = json.loads(self.read_file(f"/apps/{name}/app.json"))
            except (FileNotFoundError, ValueError) as e:
                raise HwTargetError(f"no readable /apps/{name}/app.json: {e}")
            self._manifests[name] = man
        return man["id"]

    def run_lua_app(self, name: str, timeout: float = 30.0):
        """Launch a picotest app, wait for it, return a helpers.LuaRun built
        from its results file (the serial log is only a fallback)."""
        from helpers import LuaRun, _LOG_CASE_RE, _LOG_DONE_RE
        res = f"/data/{self._app_id(name)}/test_results.json"
        self.delete_file(res)
        mark = self.log_cursor()
        self.launch_app(name)
        try:
            outcome = self.wait_for_exit(timeout)
        except TimeoutError as e:
            outcome = {"name": name, "result": "timeout", "error": str(e)}
            try:
                self.ensure_launcher()
            except HwTargetError:
                pass
        texts = [t[len("[APP] "):] if t.startswith("[APP] ") else t
                 for t in self.get_log_lines(mark)]
        problems = []
        if outcome.get("result") == "device_rebooted":
            problems.append("device rebooted during the run:\n"
                            + (outcome.get("error") or ""))
        run = LuaRun(name=name, outcome=outcome, results=None,
                     log=[{"src": "serial", "text": t} for t in texts],
                     problems=problems)
        try:
            run.results = json.loads(self.read_file(res))
        except FileNotFoundError:
            pass
        except ValueError as e:
            run.problems.append(f"unreadable {res}: {e}")
        if run.results:
            for c in run.results.get("cases", []):
                run.cases[c["name"]] = {"status": c["status"],
                                        "detail": c.get("detail", "")}
            run.done = bool(run.results.get("done"))
        else:  # drop-tolerant fallback: whatever the serial capture kept
            for t in texts:
                m = _LOG_CASE_RE.match(t)
                if m:
                    run.cases[m.group(1)] = {"status": m.group(2),
                                             "detail": m.group(3) or ""}
                elif _LOG_DONE_RE.match(t):
                    run.done = True
        return run

    # -- serial log (advisory: lines are dropped) --

    def log_cursor(self) -> int:
        return self._mon.buffer.total if self._mon else 0

    def get_log_lines(self, since: int = 0) -> list:
        if not self._mon:
            return []
        buf = self._mon.buffer
        lines, total = list(buf), buf.total
        n = min(max(0, total - since), len(lines))
        return lines[len(lines) - n:] if n else []

    def wait_for_log(self, pattern: str, timeout: float = 10.0, since: int = 0) -> str:
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            for line in self.get_log_lines(since):
                if rx.search(line):
                    return line
            if time.monotonic() >= deadline:
                raise TimeoutError(f"no serial line matching {pattern!r} within "
                                   f"{timeout}s (lines can be dropped: prefer "
                                   "the results file)")
            time.sleep(0.05)

    # -- reboot / flash --

    def reboot(self, exit_running: bool = True):
        """Reboot and wait for the launcher. The firmware honours `reboot`
        mid-app (the app dies without teardown), so the backend exits it
        first (exit_running) or refuses: every reboot starts at the
        launcher."""
        st = self.status()
        if st["app"] != "launcher":
            if not exit_running:
                raise HwTargetError(
                    f"refusing to reboot: app {st['app']!r} is running (a "
                    "mid-app reboot skips its teardown); exit to the launcher "
                    "first")
            self.ensure_launcher()
            st = self.status()
        before = st["uptime_ms"]
        self._stop_monitor()
        try:
            ser = self.pm.open_serial(self.port, timeout=1)
            ser.write(b"reboot\n")
            ser.flush()
            time.sleep(0.1)
            ser.close()
        except Exception as e:
            raise HwTargetError(f"reboot: {e}") from e
        drop_deadline = time.monotonic() + 10.0
        while time.monotonic() < drop_deadline and self._port_present():
            time.sleep(0.05)
        deadline = time.monotonic() + self.boot_timeout
        while True:
            if self._port_present():
                try:
                    self._start_monitor()
                    if self.ping(timeout=2.0):
                        break
                except Exception:
                    pass
                self._stop_monitor()
            if time.monotonic() >= deadline:
                raise HwTargetError(f"device did not come back within "
                                    f"{self.boot_timeout}s of `reboot`")
            time.sleep(self.poll_interval)
        after = self.status()["uptime_ms"]
        if after >= before:
            raise HwTargetError(f"reboot ignored: uptime {before} -> {after} ms")

    def flash(self, firmware, timeout: float = 300.0):
        """Flash `firmware` (build/picodeck.bin) with tools/ota_flash.py.
        Refused while an app runs: the firmware drops the `reboot-ota`
        that applies the image ("reboot-ota ignored: an app is running").
        Proof of the new build is the OTA tool's .flashed check plus the
        caller's behaviour check, never `ver` (its timestamp lies)."""
        st = self.status()
        if st["app"] != "launcher":
            raise HwTargetError(
                f"refusing to flash: app {st['app']!r} is running (the "
                "firmware drops reboot-ota while an app runs); exit to the "
                "launcher first")
        self._stop_monitor()
        cmd = [sys.executable, str(TOOLS_DIR / "ota_flash.py"), str(firmware),
               "--device", self.port, "--timeout", str(int(timeout))]
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout + 120)
        if r.returncode != 0:
            raise HwTargetError(f"ota_flash failed ({r.returncode}):\n"
                                f"{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
        self.connect()
        return r.stdout


# ── pytest integration (called from conftest.py) ────────────────────────────


def add_target_option(parser):
    parser.addoption(
        "--target", action="store",
        default=os.environ.get("PICODECK_E2E_TARGET", "sim"),
        help="Where tests run: 'sim' (default) or 'hw:<serial port>', e.g. "
             "hw:/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_<serial>-if00. "
             "hw runs only tests marked hardware or both (serially).")


def target_spec(config) -> tuple:
    return getattr(config, "_picodeck_target", ("sim", None))


def configure_target(config):
    """Validate --target up front: a bad spec or a path that is not a free
    serial tty is a usage error before anything is collected."""
    try:
        kind, port = parse_target_spec(config.getoption("--target"))
    except ValueError as e:
        raise _usage_error(str(e))
    config._picodeck_target = (kind, port)
    if kind != "hw":
        return
    n = getattr(config.option, "numprocesses", None)
    if n not in (None, 0, "0") and not hasattr(config, "workerinput"):
        raise _usage_error("--target hw drives one device: run without -n "
                           "(pytest-xdist)")
    if os.environ.get("PICODECK_HW_PREFLIGHT", "1") != "0":
        try:
            preflight_port(port)
        except HwTargetError as e:
            raise _usage_error(f"--target hw:{port}: {e}")


def _usage_error(msg):
    import pytest
    return pytest.UsageError(msg)


def apply_target_rules(config, items):
    """hardware -> hw only (skipped, allow-listed, on the sim); both -> any
    target; unmarked -> simulator only (deselected on hw)."""
    import pytest
    bad = [it.nodeid for it in items if it.get_closest_marker("both")
           and SIM_FIXTURES & set(getattr(it, "fixturenames", ()))]
    if bad:
        raise pytest.UsageError(
            "@pytest.mark.both tests must use the `target` fixture, not the "
            "simulator fixtures (they cannot run on hardware): "
            + ", ".join(bad))
    kind, _ = target_spec(config)
    if kind == "sim":
        skip = pytest.mark.skip(reason=HW_SKIP_REASON)
        for it in items:
            if it.get_closest_marker("hardware") and not it.get_closest_marker("both"):
                it.add_marker(skip)
        return
    keep = [it for it in items if it.get_closest_marker("hardware")
            or it.get_closest_marker("both")]
    drop = [it for it in items if it not in keep]
    if drop:
        config.hook.pytest_deselected(items=drop)
        items[:] = keep


def session_target(config) -> HwTarget:
    """The session's HwTarget, connected on first use; a device that does
    not answer ends the session at once with the reason."""
    import pytest
    tgt = getattr(config, "_picodeck_hw", None)
    if tgt is None:
        _, port = target_spec(config)
        tgt = HwTarget(port, preflight=os.environ.get("PICODECK_HW_PREFLIGHT", "1") != "0")
        try:
            tgt.connect()
            tgt.ensure_launcher()
            tgt.ensure_test_kit()
        except (HwTargetError, TimeoutError) as e:
            tgt.close()
            pytest.exit(f"--target hw:{port}: {e}", returncode=pytest.ExitCode.USAGE_ERROR)
        config._picodeck_hw = tgt
        config.add_cleanup(tgt.close)
    return tgt
