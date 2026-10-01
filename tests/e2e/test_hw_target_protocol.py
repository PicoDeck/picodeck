"""The hardware backend's protocol layer, against a fake serial port.

hw_target.HwTarget drives a PicoCalc over USB serial through
tools/picodeck_mcp.py's helpers (do_command_hardware, do_get_file_b64,
do_put_file_b64, do_screenshot_hardware, push_app). These tests replace
picodeck_mcp.open_serial with a FakeSerial wired to FakeDevice, a small model of
the firmware's dev console that answers with the exact reply formats of
src/dev_commands.c, src/dev_ops.c and src/os/launcher.c (the replies quoted
in the remediation task reports: "[DEV] Status: app=Guinea Pig Run
app_uptime_ms=6507 ...", "[DEV] Launching app by dir: t32_gp (Guinea Pig
Run)", "[DEV] Error: exit: no app running"). So the real helper code parses
real-format bytes and no serial port is opened.

The last group runs an inner pytest session (pytester) to check the
--target option and the hardware/both collection rules.
"""

from __future__ import annotations

import base64
import io
import json
import re
import site
import struct
import textwrap
import threading
import time
import zipfile

import pytest

import hw_target
from hw_target import HwTarget, HwTargetError

pm = hw_target.load_picodeck_mcp()

pytest_plugins = ["pytester"]


def _fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


# ── Fake device ─────────────────────────────────────────────────────────────


class FakeApp:
    """An installed app. On launch it runs for `run_polls` status polls, then
    writes `results` (if any) to /data/<id>/test_results.json and returns;
    `error` is appended to /system/error.log instead when set."""

    def __init__(self, dirname, name, id, requirements=(), run_polls=2,
                 results=None, error=None, hold=False):
        self.dirname, self.name, self.id = dirname, name, id
        self.requirements = list(requirements)
        self.run_polls, self.results, self.error = run_polls, results, error
        self.hold = hold  # runs until `exit`

    def manifest(self) -> bytes:
        return json.dumps({"id": self.id, "name": self.name,
                           "requirements": self.requirements}).encode()


class FakeDevice:
    """The firmware dev console, one command line at a time."""

    def __init__(self):
        self.lock = threading.Condition()
        self.out = bytearray()
        self.inbuf = b""
        self.files: dict[str, bytes] = {}
        self.apps: dict[str, FakeApp] = {}   # the launcher's cached list
        self.running: FakeApp | None = None
        self.polls = 0
        self.boot_ms = time.monotonic()
        self.uptime_base = 37200
        self.commands: list[str] = []
        self.keys: list[str] = []
        self.pads: list[str] = []
        self.fb = bytes(320 * 320 * 2)
        self.present = True
        self.boots = 1
        self.drop_next_status = 0
        self.putb64 = None   # (path, size, b64 buffer, raw bytes)
        self.reboot_delay = 0.2
        self.unzip_s = 0.0   # silence between unzip's echo and its reply
        self.scan_s = 0.0    # after a boot, `list` shows no apps for this long

    # -- helpers --
    def install(self, app: FakeApp, cached=True):
        self.files[f"/apps/{app.dirname}/app.json"] = app.manifest()
        self.files[f"/apps/{app.dirname}/main.lua"] = b"-- fake\n"
        if cached:
            self.apps[app.dirname] = app

    def uptime(self) -> int:
        return self.uptime_base + int((time.monotonic() - self.boot_ms) * 1000)

    def emit(self, text: str):
        self.out += text.encode() + b"\n"

    def emit_raw(self, data: bytes):
        self.out += data

    # -- line input --
    def feed(self, data: bytes):
        with self.lock:
            self.inbuf += data
            while b"\n" in self.inbuf:
                line, self.inbuf = self.inbuf.split(b"\n", 1)
                line = line.rstrip(b"\r").decode(errors="replace")
                if self.putb64 is not None:
                    self._putb64_chunk(line)
                elif line:
                    self._command(line)
            self.lock.notify_all()

    def _putb64_chunk(self, line: str):
        path, size, raw = self.putb64
        raw += base64.b64decode(line)
        if len(raw) >= size:
            self.files[path] = bytes(raw)
            self.putb64 = None
            self.emit(f"[DEV] File received: {path} ({len(raw)} bytes) "
                      f"fnv1a={_fnv1a(bytes(raw)):08x}")
        else:
            self.putb64 = (path, size, raw)
            self.emit(f"[DEV] ACK {len(raw)}")

    def _finish_app(self):
        app = self.running
        self.running = None
        if app.error:
            log = self.files.get("/system/error.log", b"")
            self.files["/system/error.log"] = log + app.error.encode()
        if app.results is not None:
            self.files[f"/data/{app.id}/test_results.json"] = \
                json.dumps(app.results).encode()

    def _find(self, arg):
        for a in self.apps.values():
            if a.id == arg:
                return a, f"[DEV] Launching app by ID: {arg} ({a.name})"
        for a in self.apps.values():
            if a.name.lower() == arg.lower():
                return a, f"[DEV] Launching app: {arg}"
        for a in self.apps.values():
            if a.dirname.lower() == arg.lower():
                return a, f"[DEV] Launching app by dir: {arg} ({a.name})"
        return None, f"[DEV] Error: app '{arg}' not found"

    def _later(self, delay: float, lines):
        def fire():
            with self.lock:
                for ln in lines:
                    self.emit(ln)
                self.lock.notify_all()
        threading.Timer(delay, fire).start()

    def _command(self, cmd: str):
        self.commands.append(cmd)
        self.emit(f"[DEV] Command: {cmd}")
        if cmd == "ping":
            self.emit("[DEV] pong")
        elif cmd == "status":
            if self.running:
                self.polls += 1
                if not self.running.hold and self.polls >= self.running.run_polls:
                    self._finish_app()
            if self.drop_next_status:
                self.drop_next_status -= 1
                return
            app = self.running.name if self.running else "launcher"
            self.emit(f"[DEV] Status: app={app} app_uptime_ms="
                      f"{1234 if self.running else 0} uptime_ms={self.uptime()} "
                      f"wifi=online sd=mounted battery=87")
        elif cmd == "exit":
            if self.running:
                name = self.running.name
                self._finish_app()
                self.emit(f"[DEV] Exit requested: {name}")
            else:
                self.emit("[DEV] Error: exit: no app running")
        elif cmd.startswith("launch "):
            app, line = self._find(cmd[7:])
            self.emit(line)
            if app:
                self.running, self.polls = app, 0
        elif cmd == "list":
            scanned = time.monotonic() - self.boot_ms >= self.scan_s
            apps = list(self.apps.values()) if scanned else []
            self.emit("[DEV] Available apps:")
            for a in apps:
                self.emit(f"  {a.name}  ({a.id})")
            self.emit(f"[DEV] Total: {len(apps)} apps")
        elif cmd.startswith("getb64 "):
            path = cmd[7:]
            if path not in self.files:
                self.emit(f"[DEV] Failed to open file: {path}")
                return
            data = self.files[path]
            self.emit(f"[DEV] B64 size={len(data)}")
            for off in range(0, len(data), 72):
                self.emit("~" + base64.b64encode(data[off:off + 72]).decode())
            self.emit(f"[DEV] B64_END fnv1a={_fnv1a(data):08x} path={path}")
        elif cmd.startswith("putb64 "):
            path, size = cmd[7:].rsplit(" ", 1)
            self.putb64 = (path, int(size), b"")
            self.emit(f"[DEV] Ready B64 {size} bytes for {path} "
                      "(chunk<=384 raw, newline-terminated, await ACK)")
        elif cmd.startswith("mkdir "):
            self.emit(f"[DEV] Created: {cmd[6:]}")
        elif cmd.startswith("rm "):
            path = cmd[3:]
            gone = [p for p in self.files if p == path or p.startswith(path + "/")]
            for p in gone:
                del self.files[p]
            self.emit(f"[DEV] Deleted: {path}" if gone
                      else f"[DEV] Error: rm failed: {path}")
        elif cmd.startswith("unzip "):
            zpath, dest = cmd[6:].split(" ", 1)
            with zipfile.ZipFile(io.BytesIO(self.files[zpath])) as zf:
                names = zf.namelist()
                for n in names:
                    self.files[f"{dest}/{n}"] = zf.read(n)
            done = [f"[DEV] UNZIP {len(names)}/{len(names)}",
                    f"[DEV] Unzipped {len(names)} files (0 skipped)"]
            if self.unzip_s:
                # Inflating a large file: the device says nothing meanwhile.
                self._later(self.unzip_s, done)
            else:
                for ln in done:
                    self.emit(ln)
        elif cmd.startswith("keypress ") or cmd.startswith("keydown ") \
                or cmd.startswith("keyup "):
            self.keys.append(cmd)
            self.emit(f"[DEV] Key injected: {cmd.split(' ', 1)[1]}")
        elif cmd == "pad" or cmd.startswith("pad "):
            # src/dev_ops.c dev_op_pad's replies.
            self.pads.append(cmd)
            args = cmd[4:].split()
            names = {"up", "down", "left", "right", "a", "b", "x", "y", "l",
                     "r", "start", "select", "home"}
            if not args or (len(args) > 1 and not args[1].isdigit()):
                self.emit("[DEV] Usage: pad <none|off|buttons> [hold_ms] "
                          "(buttons: up down left right a b x y l r start "
                          "select home, joined by '+')")
            elif args[0] in ("none", "off"):
                self.emit(f"[DEV] Pad: {args[0]}")
            elif any(n not in names for n in args[0].split("+")):
                bad = next(n for n in args[0].split("+") if n not in names)
                self.emit(f"[DEV] Error: unknown pad button: {bad}")
            else:
                hold = f" for {args[1]} ms" if len(args) > 1 else ""
                self.emit(f"[DEV] Pad: {args[0]}{hold}")
        elif cmd == "screenshot":
            self.emit_raw(b"SCRN" + struct.pack("<HHH", 320, 320, 565) + b"\0\0")
            self.emit_raw(self.fb)
        elif cmd == "stack":
            self.emit("[DEV] Stack: msp_peak=3012 msp_size=4096 core1_peak=900 "
                      "core1_size=4096 app=lua app_peak=21000 app_size=65536 "
                      "os_cmd_peak=5100 os_cmd_size=32768")
        elif cmd == "reboot-ota":
            if self.running:  # dropped mid-app (lua_bridge_service, sys_poll)
                self.emit("[DEV] reboot-ota ignored: an app is running "
                          "(exit it first)")
        elif cmd == "reboot":
            # Honoured mid-app too (the app dies without teardown).
            self.running = None
            self.emit("[DEV] Rebooting...")
            self.present = False
            # The launcher rescans /apps at boot.
            self.apps = {}
            for p, data in self.files.items():
                m = re.fullmatch(r"/apps/([^/]+)/app\.json", p)
                if m:
                    man = json.loads(data)
                    self.apps[m.group(1)] = FakeApp(
                        m.group(1), man["name"], man["id"],
                        man.get("requirements", ()))

            def back():
                time.sleep(self.reboot_delay)
                with self.lock:
                    self.boot_ms = time.monotonic()
                    self.uptime_base = 900
                    self.boots += 1
                    self.present = True
                    self.out.clear()
            threading.Thread(target=back, daemon=True).start()
        else:
            self.emit(f"[DEV] Unknown command: {cmd}")


class FakeSerial:
    """The subset of pyserial.Serial picodeck_mcp uses, over a FakeDevice."""

    def __init__(self, dev: FakeDevice, timeout: float):
        if not dev.present:
            raise OSError("[Errno 2] could not open port: No such file")
        self.dev, self.timeout, self.closed = dev, timeout, False

    @property
    def in_waiting(self):
        return len(self.dev.out)

    def write(self, data):
        if self.closed or not self.dev.present:
            raise OSError("port closed")
        self.dev.feed(bytes(data))
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        with self.dev.lock:
            self.dev.out.clear()

    def _take(self, n):
        chunk = bytes(self.dev.out[:n])
        del self.dev.out[:n]
        return chunk

    def read(self, n=1):
        if self.closed:
            raise OSError("port closed")
        deadline = time.monotonic() + (self.timeout or 0)
        with self.dev.lock:
            while not self.dev.out:
                left = deadline - time.monotonic()
                if left <= 0:
                    return b""
                self.dev.lock.wait(left)
            return self._take(n)

    def readline(self):
        deadline = time.monotonic() + (self.timeout or 0)
        with self.dev.lock:
            while b"\n" not in self.dev.out:
                left = deadline - time.monotonic()
                if left <= 0:
                    return self._take(len(self.dev.out))
                self.dev.lock.wait(left)
            return self._take(self.dev.out.index(b"\n") + 1)

    def close(self):
        self.closed = True


@pytest.fixture
def dev(monkeypatch):
    d = FakeDevice()
    monkeypatch.setattr(pm, "open_serial",
                        lambda port, timeout=5: FakeSerial(d, timeout))
    monkeypatch.setattr(pm, "HAS_SERIAL", True)
    return d


@pytest.fixture
def hw(dev):
    t = HwTarget("/dev/fake-picocalc", preflight=False, poll_interval=0.05,
                 boot_timeout=5.0, exit_timeout=3.0)
    t._port_present = lambda: dev.present
    t.connect()
    yield t
    t.close()


# ── Parsers (pure) ──────────────────────────────────────────────────────────


def test_parse_status_recorded_line():
    st = hw_target.parse_status([
        "[DEV] Command: status",
        "[DEV] Status: app=Guinea Pig Run app_uptime_ms=6507 uptime_ms=163144 "
        "wifi=online sd=mounted battery=87"])
    assert st == {"app": "Guinea Pig Run", "app_uptime_ms": 6507,
                  "uptime_ms": 163144, "wifi": "online", "sd": "mounted",
                  "battery": 87}


def test_parse_status_launcher_and_missing():
    st = hw_target.parse_status(["[DEV] Status: app=launcher app_uptime_ms=0 "
                                 "uptime_ms=37200 wifi=disconnected sd=mounted "
                                 "battery=-1"])
    assert st["app"] == "launcher" and st["battery"] == -1
    assert hw_target.parse_status(["[DEV] Command: status"]) is None


def test_parse_exit_reply():
    assert hw_target.parse_exit_reply(
        ["[DEV] Command: exit", "[DEV] Exit requested: Guinea Pig Run"]) == \
        (True, "Exit requested: Guinea Pig Run")
    assert hw_target.parse_exit_reply(
        ["[DEV] Command: exit", "[DEV] Error: exit: no app running"]) == \
        (False, "Error: exit: no app running")
    assert hw_target.parse_exit_reply([]) == (None, "")


def test_parse_launch_reply():
    assert hw_target.parse_launch_reply(
        ["[DEV] Command: launch t32_gp",
         "[DEV] Launching app by dir: t32_gp (Guinea Pig Run)"])[0] == "launched"
    assert hw_target.parse_launch_reply(
        ["[DEV] Error: app 'nope' not found"])[0] == "not_found"
    assert hw_target.parse_launch_reply(["[DEV] Command: launch x"])[0] is None


def test_parse_target_spec():
    assert hw_target.parse_target_spec("sim") == ("sim", None)
    assert hw_target.parse_target_spec("hw:/dev/ttyACM0") == ("hw", "/dev/ttyACM0")
    for bad in ("hw", "hw:", "device", "hw:relative/path"):
        with pytest.raises(ValueError):
            hw_target.parse_target_spec(bad)


def test_parse_stack():
    st = hw_target.parse_stack([
        "[DEV] Stack: msp_peak=3012 msp_size=4096 core1_peak=900 "
        "core1_size=4096 app=lua app_peak=21000 app_size=65536 "
        "os_cmd_peak=5100 os_cmd_size=32768"])
    assert st["msp_peak"] == 3012 and st["app"] == "lua"
    assert st["app_size"] == 65536


# ── Status, launch, exit ────────────────────────────────────────────────────


def test_status_over_the_port(hw):
    st = hw.status()
    assert st["app"] == "launcher" and st["wifi"] == "online"


def test_status_retries_a_dropped_reply(hw, dev):
    dev.drop_next_status = 2
    assert hw.status()["app"] == "launcher"


def test_launch_and_wait_for_exit_returned(hw, dev):
    dev.install(FakeApp("t32_gp", "Guinea Pig Run", "com.test.gp", run_polls=3))
    r = hw.launch_app("t32_gp")
    assert r["launched"] and "Guinea Pig Run" in r["line"]
    out = hw.wait_for_exit(timeout=5)
    assert out["result"] == "returned", out
    assert out["name"] == "t32_gp"
    assert hw.status()["app"] == "launcher"


def test_launch_unknown_app_is_load_failed(hw):
    r = hw.launch_app("nope")
    assert not r["launched"]
    out = hw.wait_for_exit(timeout=2)
    assert out["result"] == "load_failed" and "not found" in out["error"]


def test_launch_presence_contract_matches_the_sim(hw, dev):
    """The same check test_hw_smoke.py::test_launch_reports_presence runs
    on the simulator (SimTarget): both backends agree on `launched`."""
    from test_hw_smoke import check_launch_reports_presence
    dev.install(FakeApp("hw_hold", "hw_hold", "com.test.hw_hold", hold=True))
    check_launch_reports_presence(hw, "hw_hold")


def test_error_log_growth_is_an_error_outcome(hw, dev):
    dev.files["/system/error.log"] = b"old entry\n"
    dev.install(FakeApp("bad", "Bad", "com.test.bad", run_polls=1,
                        error="[Lua error] main.lua:3: boom\nHeap: ...\n"))
    hw.launch_app("bad")
    out = hw.wait_for_exit(timeout=5)
    assert out["result"] == "error", out
    assert "boom" in out["error"] and "old entry" not in out["error"]


def test_wait_for_exit_times_out(hw, dev):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    hw.launch_app("spin")
    with pytest.raises(TimeoutError):
        hw.wait_for_exit(timeout=0.5)


def test_exit_at_launcher_is_refused_not_fatal(hw, dev):
    r = hw.exit_app()
    assert r == {"ok": False, "message": "Error: exit: no app running"}
    assert hw.ping()


def test_exit_running_app(hw, dev):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    hw.launch_app("spin")
    r = hw.exit_app()
    assert r["ok"] and r["message"] == "Exit requested: Spin"
    out = hw.wait_for_exit(timeout=5)
    assert out["result"] == "exit_sentinel", out


def test_launch_exits_a_running_app_first(hw, dev):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    dev.install(FakeApp("other", "Other", "com.test.other", hold=True))
    hw.launch_app("spin")
    hw.launch_app("other")
    assert dev.commands.count("exit") == 1
    assert hw.status()["app"] == "Other"


def test_device_reboot_mid_run_is_reported(hw, dev):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    hw.launch_app("spin")
    hw.status()
    dev.running = None
    dev.uptime_base, dev.boot_ms = 10, time.monotonic()   # uptime went back
    dev.files["/system/crashlog.txt"] = b"HardFault PC=0x1000\n"
    out = hw.wait_for_exit(timeout=5)
    assert out["result"] == "device_rebooted", out
    assert "HardFault" in out["error"]


# ── Files and the results file ──────────────────────────────────────────────


def test_read_write_file_roundtrip(hw, dev):
    payload = bytes(range(256)) * 5
    hw.write_file("/data/com.test.x/blob.bin", payload)
    assert dev.files["/data/com.test.x/blob.bin"] == payload
    assert hw.read_file("/data/com.test.x/blob.bin") == payload
    with pytest.raises(FileNotFoundError):
        hw.read_file("/data/nope.txt")


def test_run_lua_app_reads_the_results_file(hw, dev):
    results = {"app": "com.test.kit", "done": True, "pass": 1, "fail": 1,
               "skip": 0, "cases": [
                   {"name": "adds", "status": "PASS", "detail": ""},
                   {"name": "breaks", "status": "FAIL", "detail": "main.lua:4: x"}]}
    dev.install(FakeApp("kit", "Kit", "com.test.kit", run_polls=2,
                        results=results))
    run = hw.run_lua_app("kit", timeout=5)
    assert run.outcome["result"] == "returned"
    assert run.done
    assert run.cases == {"adds": {"status": "PASS", "detail": ""},
                         "breaks": {"status": "FAIL", "detail": "main.lua:4: x"}}


def test_run_lua_app_without_results_file(hw, dev):
    dev.install(FakeApp("kit", "Kit", "com.test.kit", run_polls=1,
                        error="[Lua error] boom\n"))
    # A results file left by an earlier run must not pass for this one.
    dev.files["/data/com.test.kit/test_results.json"] = json.dumps(
        {"done": True, "cases": [{"name": "old", "status": "PASS"}]}).encode()
    run = hw.run_lua_app("kit", timeout=5)
    assert run.results is None and not run.done
    assert run.outcome["result"] == "error"
    with pytest.raises(AssertionError):
        run.assert_clean_exit()


def test_wait_for_results_polls_until_the_case_appears(hw, dev):
    dev.install(FakeApp("kit", "Kit", "com.test.kit", hold=True))
    hw.launch_app("kit")

    def later():
        time.sleep(0.3)
        dev.files["/data/com.test.kit/test_results.json"] = json.dumps(
            {"done": False, "cases": [{"name": "ready", "status": "PASS"}]}).encode()
    threading.Thread(target=later, daemon=True).start()
    doc = hw.wait_for_results("com.test.kit", timeout=5,
                              until=lambda d: any(c["name"] == "ready"
                                                  for c in d.get("cases", [])))
    assert doc["cases"][0]["name"] == "ready"


# ── Screenshot ──────────────────────────────────────────────────────────────


def test_screenshot_decodes_big_endian_rgb565(hw, dev):
    import numpy as np
    from PIL import Image
    fb = bytearray(320 * 320 * 2)
    # (0,0) red 0xF800, (1,0) green 0x07E0, (2,0) blue 0x001F, stored
    # high byte first as display.c leaves them for the panel.
    for i, px in enumerate((0xF800, 0x07E0, 0x001F)):
        fb[i * 2:i * 2 + 2] = px.to_bytes(2, "big")
    dev.fb = bytes(fb)
    png = hw.screenshot()
    img = np.array(Image.open(io.BytesIO(png)).convert("RGB"))
    assert img.shape == (320, 320, 3)
    assert tuple(img[0, 0]) == (255, 0, 0)
    assert tuple(img[0, 1]) == (0, 255, 0)
    assert tuple(img[0, 2]) == (0, 0, 255)
    assert tuple(img[0, 3]) == (0, 0, 0)
    assert hw_target.pixel(png, 1, 0) == (0, 255, 0)


# ── Keys ────────────────────────────────────────────────────────────────────


def test_keypress_sequence_and_chords(hw, dev):
    hw.keypress_sequence(["a", "enter", "ctrl+x"], delay_ms=0)
    assert dev.keys == ["keypress a", "keypress enter", "keydown ctrl",
                        "keypress x", "keyup ctrl"]


def test_pad_commands_and_replies(hw, dev):
    """HwTarget.pad sends the `pad` dev command and returns its reply; an
    error reply raises."""
    assert hw.pad("up+a") == "Pad: up+a"
    assert hw.pad("x", hold_ms=120) == "Pad: x for 120 ms"
    assert hw.pad("off") == "Pad: off"
    with pytest.raises(HwTargetError, match="unknown pad button: zz"):
        hw.pad("zz")
    assert dev.pads == ["pad up+a", "pad x 120", "pad off", "pad zz"]


def test_mcp_pad_spec_expansion():
    """The MCP `pad` tool's specs: one state as given, a list pressed in
    turn with a hold (100 ms by default), none/off never timed."""
    assert pm._pad_commands("A+Right") == ["pad a+right"]
    assert pm._pad_commands("up", 250) == ["pad up 250"]
    assert pm._pad_commands("home, up, a") == [
        "pad home 100", "pad up 100", "pad a 100"]
    assert pm._pad_commands("b none off", 50) == [
        "pad b 50", "pad none", "pad off"]
    for bad in ("", "  ", "a+zz", "menu"):
        with pytest.raises(ValueError):
            pm._pad_commands(bad)
    assert pm._pad_reply(["[DEV] Command: pad a", "[DEV] Pad: a"]) == "Pad: a"
    assert pm._pad_reply(["[DEV] Command: pad q",
                          "[DEV] Error: unknown pad button: q"]).startswith(
        "Error:")
    assert pm._pad_reply([]) == "no reply"
    # Both replies end the exchange at once (no idle wait).
    assert pm._is_cmd_end("[DEV] Pad: up+a")
    assert pm._is_cmd_end("[DEV] Usage: pad <none|off|buttons> [hold_ms]")
    assert not pm._is_cmd_end("[DEV] Command: pad a")


# ── push_app and the launcher's app.json cache ──────────────────────────────


def _app_dir(tmp_path, name, requirements=()):
    d = tmp_path / name
    d.mkdir()
    (d / "app.json").write_text(json.dumps(
        {"id": f"com.test.{name}", "name": name,
         "requirements": list(requirements)}))
    (d / "main.lua").write_text("return\n")
    return d


def test_push_new_app_reboots_so_the_launcher_sees_it(hw, dev, tmp_path):
    r = hw.push_app(_app_dir(tmp_path, "newapp"))
    assert r["rebooted"], r
    assert dev.boots == 2
    assert "newapp" in dev.apps
    assert dev.files["/apps/newapp/main.lua"] == b"return\n"


def test_push_unchanged_manifest_does_not_reboot(hw, dev, tmp_path):
    d = _app_dir(tmp_path, "same", ["http"])
    dev.install(FakeApp("same", "same", "com.test.same", ["http"]))
    r = hw.push_app(d)
    assert not r["rebooted"], r
    assert dev.boots == 1


def test_push_changed_requirements_reboots(hw, dev, tmp_path):
    d = _app_dir(tmp_path, "req", ["http"])
    dev.install(FakeApp("req", "req", "com.test.req", []))
    r = hw.push_app(d)
    assert r["rebooted"], r
    assert dev.apps["req"].requirements == ["http"]


def test_push_waits_out_a_quiet_extraction(hw, dev, tmp_path):
    # unzip echoes its command, then is silent while it inflates a large
    # file (a 700 KB QOA takes ~1.8 s on the device): the push must wait
    # for "Unzipped", not give up after a second of quiet.
    d = _app_dir(tmp_path, "big")
    dev.install(FakeApp("big", "big", "com.test.big"))
    dev.unzip_s = 1.6
    r = hw.push_app(d)
    assert not r["rebooted"], r
    assert dev.files["/apps/big/main.lua"] == b"return\n"


def test_push_waits_for_unzipped_when_the_echo_holds_a_marker(hw, dev, tmp_path):
    # The device echoes "[DEV] Command: unzip ... /apps/pong": the word
    # "pong" in the echo is not the reply. The push must wait for "Unzipped".
    d = _app_dir(tmp_path, "pong")
    dev.install(FakeApp("pong", "pong", "com.test.pong"))
    dev.unzip_s = 1.6
    t0 = time.monotonic()
    r = hw.push_app(d)
    waited = time.monotonic() - t0
    assert not r["rebooted"], r
    assert waited >= dev.unzip_s, f"push returned after {waited:.2f}s"
    assert dev.files["/apps/pong/main.lua"] == b"return\n"
    # The cleanup rm goes out only after the delayed "Unzipped" arrived: were
    # the echo taken as the reply, rm would follow the unzip at once.
    assert dev.commands.index("rm /data/tmp/push_app.zip") \
        > next(i for i, c in enumerate(dev.commands) if c.startswith("unzip ")), \
        dev.commands


def test_push_waits_for_the_launcher_to_list_a_new_app(hw, dev, tmp_path):
    # On the device a `list` straight after the reboot has missed an app
    # just pushed, which the launcher listed a few seconds later: push_app
    # polls for it.
    dev.scan_s = 0.6
    r = hw.push_app(_app_dir(tmp_path, "late"))
    assert r["rebooted"], r
    assert "late" in dev.apps


def test_push_gives_up_on_an_app_the_launcher_never_lists(hw, dev, tmp_path):
    dev.scan_s = 60
    hw.rescan_timeout = 0.5
    t0 = time.monotonic()
    with pytest.raises(HwTargetError, match="still not listed"):
        hw.push_app(_app_dir(tmp_path, "never"))
    assert time.monotonic() - t0 < 5


def test_reboot_is_refused_while_an_app_runs(hw, dev):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    hw.launch_app("spin")
    with pytest.raises(HwTargetError, match="exit to the launcher"):
        hw.reboot(exit_running=False)
    assert dev.boots == 1 and "reboot" not in dev.commands


def test_flash_is_refused_while_an_app_runs(hw, dev, tmp_path):
    dev.install(FakeApp("spin", "Spin", "com.test.spin", hold=True))
    hw.launch_app("spin")
    fw = tmp_path / "picodeck.bin"
    fw.write_bytes(b"\0" * 4096)
    with pytest.raises(HwTargetError, match="running"):
        hw.flash(fw)
    assert not any(c.startswith("putb64") for c in dev.commands)


# ── Serial log capture ──────────────────────────────────────────────────────


def test_log_capture_is_cursor_based(hw, dev):
    mark = hw.log_cursor()
    with dev.lock:
        dev.emit("[APP] HELLO 1")
        dev.emit("[MOD] DMA ISR=10 underruns=0 ring=100 free=200")
        dev.lock.notify_all()
    line = hw.wait_for_log(r"^\[MOD\] DMA", timeout=3, since=mark)
    assert "ISR=10" in line
    assert "[APP] HELLO 1" in hw.get_log_lines(mark)


# ── Preflight ───────────────────────────────────────────────────────────────


def test_preflight_rejects_a_non_tty(tmp_path):
    with pytest.raises(HwTargetError, match="not a serial"):
        hw_target.preflight_port("/dev/null")
    with pytest.raises(HwTargetError, match="does not exist"):
        hw_target.preflight_port(str(tmp_path / "missing"))


def test_connect_fails_fast_when_the_device_does_not_answer(monkeypatch):
    class Mute(FakeSerial):
        def write(self, data):
            return len(data)   # swallows every command
    d = FakeDevice()
    monkeypatch.setattr(pm, "open_serial", lambda port, timeout=5: Mute(d, timeout))
    t = HwTarget("/dev/fake", preflight=False, connect_timeout=1.0)
    t0 = time.monotonic()
    with pytest.raises(HwTargetError, match="no answer to ping"):
        t.connect()
    t.close()
    assert time.monotonic() - t0 < 10


# ── --target and collection rules (inner pytest session) ────────────────────

from helpers import E2E_DIR  # noqa: E402

INNER_CONFTEST = textwrap.dedent(f"""
    import importlib.util, sys
    sys.path.insert(0, {str(E2E_DIR)!r})
    _spec = importlib.util.spec_from_file_location(
        "picodeck_e2e_conftest", {str(E2E_DIR / "conftest.py")!r})
    _mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_mod)
    globals().update({{k: v for k, v in vars(_mod).items()
                      if not k.startswith("__")}})
""")

INNER_TESTS = """
    import pytest

    @pytest.mark.hardware
    def test_hw_only(target):
        assert target.kind == "hw"

    @pytest.mark.both
    def test_on_both(target):
        assert target.kind in ("sim", "hw")

    def test_sim_only():
        pass
"""


def _inner(pytester, simulator_binary, src, *args, env=None):
    pytester._monkeypatch.setenv("PYTHONUSERBASE", site.getuserbase())
    for k, v in (env or {}).items():
        pytester._monkeypatch.setenv(k, v)
    pytester.makeconftest(INNER_CONFTEST)
    pytester.makepyfile(test_inner=textwrap.dedent(src))
    return pytester.runpytest_subprocess(
        "-p", "no:cacheprovider", "-p", "no:xdist",
        f"--simulator-path={simulator_binary}", *args, timeout=120)


def test_sim_target_skips_hardware_and_runs_both(pytester, simulator_binary):
    result = _inner(pytester, simulator_binary, INNER_TESTS, "-rs")
    assert result.ret == pytest.ExitCode.OK, result.stdout.str()
    result.assert_outcomes(passed=2, skipped=1)
    result.stdout.fnmatch_lines(["*SKIPPED*hardware: needs a PicoCalc*--target hw:*"])


def test_hw_target_selects_hardware_and_both(pytester, simulator_binary):
    # Collection only: nothing connects. PICODECK_HW_PREFLIGHT=0 skips the tty
    # check so a made-up path gets through to the collection rules.
    result = _inner(pytester, simulator_binary, INNER_TESTS,
                    "--target", "hw:/dev/fake-picocalc", "--collect-only", "-q",
                    env={"PICODECK_HW_PREFLIGHT": "0"})
    out = result.stdout.str()
    assert "test_hw_only" in out and "test_on_both" in out, out
    assert "test_sim_only" not in out, out
    assert "1 deselected" in out, out


def test_hw_target_on_a_non_serial_path_fails_fast(pytester, simulator_binary):
    t0 = time.monotonic()
    result = _inner(pytester, simulator_binary, INNER_TESTS,
                    "--target", "hw:/dev/null")
    assert result.ret == pytest.ExitCode.USAGE_ERROR, result.stdout.str()
    result.stderr.fnmatch_lines(["*--target hw:/dev/null*not a serial*"])
    assert time.monotonic() - t0 < 30


def test_bad_target_spec_is_a_usage_error(pytester, simulator_binary):
    result = _inner(pytester, simulator_binary, INNER_TESTS, "--target", "phone")
    assert result.ret == pytest.ExitCode.USAGE_ERROR, result.stdout.str()


def test_both_test_must_not_use_simulator_fixtures(pytester, simulator_binary):
    result = _inner(pytester, simulator_binary, """
        import pytest
        @pytest.mark.both
        def test_wrong(simulator):
            pass
    """)
    assert result.ret != pytest.ExitCode.OK
    assert "use the `target` fixture" in result.stdout.str() + result.stderr.str()
