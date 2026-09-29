"""Helpers shared by the E2E tests (fixtures and hooks live in conftest.py).

- SD card staging: build_sd_card() builds the per-test SD image from a
  manifest; stage_lua_app() writes a per-test inline Lua app onto it.
- Lua test kit runner: run_lua_app() launches a fixture app that uses
  tests/e2e/lib/picotest.lua, waits for app.exited and returns the cases it
  reported; lua_case_names() lists an app's T.case names at collection time so
  a test module can expand them into one pytest id per case.
"""

from __future__ import annotations

import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional, Union

import pytest

from picodeck_simulator import PicodeckSimulator

E2E_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = E2E_DIR.parent.parent
FIXTURE_APPS = E2E_DIR / "apps"
TEST_LIB_DIR = E2E_DIR / "lib"
SYSTEM_LIB_DIR = PROJECT_ROOT / "system" / "lib"
DEFAULT_SD_SOURCE = PROJECT_ROOT / "simulator" / "assets" / "sd_card"
GOLDEN_DIR = E2E_DIR / "fixtures"

# ── SD card ─────────────────────────────────────────────────────────────────

SdExtra = Union[str, tuple]


def launcher_app_cap() -> int:
    """MAX_APPS, read from src/os/launcher.c so it can't drift from the OS."""
    src = (PROJECT_ROOT / "src" / "os" / "launcher.c").read_text()
    m = re.search(r"^#define\s+MAX_APPS\s+(\d+)", src, re.M)
    if not m:
        raise RuntimeError("MAX_APPS not found in src/os/launcher.c")
    return int(m.group(1))


def fixture_app_names() -> list[str]:
    """Every fixture app under tests/e2e/apps/."""
    return sorted(a.name for a in FIXTURE_APPS.iterdir() if a.is_dir())


def build_sd_card(dest: Path, extra: Iterable[SdExtra] = (),
                  default_sd: Path = DEFAULT_SD_SOURCE,
                  fixtures: Optional[Iterable[str]] = None,
                  reserve: int = 0) -> Path:
    """Build a simulator SD card at `dest` from the manifest:

    - apps/hello from the default SD card (simulator/assets/sd_card)
    - every fixture app under tests/e2e/apps/ (or only those named in
      `fixtures`, hello excepted)
    - system/lib/*.lua from the repo, plus the test kit (picotest.lua)

    Nothing else is copied, so untracked content in the default SD card (a
    local C-Dogs install is 2,700 files) never reaches the tests. `extra`
    adds more: a string is a path relative to `default_sd` copied to the same
    place, and a (source, dest) tuple copies `source` (absolute, or relative
    to the repo root) to `dest` (relative to the SD root).

    The launcher keeps at most MAX_APPS apps, in directory order, and drops
    the rest with a warning; which ones it drops depends on the filesystem
    (tmpfs lists new entries first, ext4 and btrfs do not). So a card must
    never hold more than that: this raises if the built card does, or would
    once the test stages `reserve` more apps at runtime. A test that stages
    apps passes `fixtures=` (just the fixture apps it launches) and `reserve=`
    (how many it stages), through `@pytest.mark.sd(fixtures=..., reserve=...)`.
    """
    dest = Path(dest)
    available = fixture_app_names()
    if fixtures is None:
        wanted = available
    else:
        wanted = sorted(set(fixtures))
        unknown = [n for n in wanted if n not in available]
        if unknown:
            raise ValueError(f"build_sd_card: no such fixture app(s) {unknown} "
                             f"under {FIXTURE_APPS}")
    for sub in ("apps", "data", "system/lib"):
        (dest / sub).mkdir(parents=True, exist_ok=True)

    hello = Path(default_sd) / "apps" / "hello"
    if hello.is_dir():
        shutil.copytree(hello, dest / "apps" / "hello", dirs_exist_ok=True)

    for name in wanted:
        shutil.copytree(FIXTURE_APPS / name, dest / "apps" / name,
                        dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__"))

    for lib in list(SYSTEM_LIB_DIR.glob("*.lua")) + list(TEST_LIB_DIR.glob("*.lua")):
        shutil.copy2(lib, dest / "system" / "lib" / lib.name)

    for item in extra:
        if isinstance(item, (tuple, list)):
            src, rel = Path(item[0]), item[1]
            if not src.is_absolute():
                src = PROJECT_ROOT / src
        else:
            src, rel = Path(default_sd) / item, item
        target = dest / rel
        if src.is_dir():
            shutil.copytree(src, target, dirs_exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)

    cap = launcher_app_cap()
    count = sum(1 for a in (dest / "apps").iterdir() if a.is_dir())
    if count + reserve > cap:
        raise RuntimeError(
            f"SD card holds {count} apps"
            + (f" and the test stages {reserve} more" if reserve else "")
            + f", over the launcher's MAX_APPS ({cap}, src/os/launcher.c). "
            "The launcher would silently drop the surplus in directory order, "
            "which differs per filesystem. Do not raise MAX_APPS (the app "
            "table is device PSRAM). Ask for a smaller card: "
            "@pytest.mark.sd(fixtures=[<the fixture apps this test launches>], "
            "reserve=<apps it stages>), or fixtures=... on sim_module_factory "
            "/ lua_suite. See tests/e2e/README.md, 'App cap'.")
    return dest


# ── Simulator ───────────────────────────────────────────────────────────────


def new_simulator(config, binary, sd_path, crash_log, **kwargs) -> PicodeckSimulator:
    """A started PicodeckSimulator with the suite's defaults: headless unless
    --show-window, --test-mode (error screens return at once), no UNIX
    socket, crash log at `crash_log`."""
    kwargs.setdefault("test_mode", True)
    sim = PicodeckSimulator(
        binary_path=str(binary),
        sd_card_path=str(sd_path),
        headless=not config.getoption("--show-window"),
        tcp_port=config.getoption("--port"),
        crash_log_path=str(crash_log),
        **kwargs,
    )
    sim.start()
    return sim


def stop_and_check(sim: PicodeckSimulator):
    """Stop `sim` and fail if it had crashed or a sanitizer reported."""
    sim.stop()
    problems = sim.health_problems()
    if problems:
        pytest.fail("simulator unhealthy at teardown:\n" + "\n".join(problems),
                    pytrace=False)


# ── App staging ─────────────────────────────────────────────────────────────


def stage_lua_app(sd: Path, name: str, code: str, requirements=(),
                  id: Optional[str] = None, files: Optional[dict] = None) -> Path:
    """Write /apps/<name>/{app.json,main.lua} onto the SD card `sd`.

    The simulator rescans /apps when launch_app misses, so an app staged after
    boot can be launched without a restart. `files` adds more files to the app
    directory ({relative_path: str | bytes}).
    """
    app_dir = Path(sd) / "apps" / name
    app_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "id": id or f"com.test.{name}",
        "name": name,
        "description": "E2E inline test app",
        "version": "1.0",
        "author": "PicoDeck E2E",
        "requirements": list(requirements),
    }
    (app_dir / "app.json").write_text(json.dumps(manifest, indent=2))
    (app_dir / "main.lua").write_text(code)
    for rel, content in (files or {}).items():
        path = app_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content)
    return app_dir


def app_id_of(sd: Path, name: str) -> str:
    """The id declared in /apps/<name>/app.json."""
    manifest = json.loads((Path(sd) / "apps" / name / "app.json").read_text())
    return manifest["id"]


# ── Lua test kit ────────────────────────────────────────────────────────────

# T.case("name", ...) or a local wrapper named case_*("name", ...).
_CASE_RE = re.compile(r"""(?:T\.case|\bcase_\w+)\(\s*(["'])(.+?)\1""")
_LOG_CASE_RE = re.compile(r"^\[T\] CASE (\S+) (PASS|FAIL|SKIP)(?: (.*))?$")
_LOG_DONE_RE = re.compile(r"^\[T\] DONE pass=(\d+) fail=(\d+) skip=(\d+)$")


def lua_case_names(app: Union[str, Path]) -> list[str]:
    """The T.case names in a fixture app's main.lua, in source order.

    Read statically at collection time, so a module can parametrize one
    pytest id per Lua case. Names must be string literals, passed to T.case
    or to a local wrapper whose name starts with case_ (json_test's case_ok).
    """
    path = Path(app)
    if not path.is_absolute():
        path = FIXTURE_APPS / str(app) / "main.lua"
    elif path.is_dir():
        path = path / "main.lua"
    names = [m.group(2) for m in _CASE_RE.finditer(path.read_text())]
    dupes = {n for n in names if names.count(n) > 1}
    assert not dupes, f"{path}: duplicate T.case names {sorted(dupes)}"
    return names


@dataclass
class LuaRun:
    """What one run of a picotest fixture app produced."""
    name: str
    outcome: dict                       # app.exited params
    results: Optional[dict]             # test_results.json, if written
    cases: dict = field(default_factory=dict)   # name -> {status, detail}
    log: list = field(default_factory=list)     # log entries since launch
    problems: list = field(default_factory=list)  # sim health problems
    done: bool = False
    sd: Optional[Path] = None                    # the SD card it ran on

    def describe(self) -> str:
        lines = [f"app {self.name}: outcome {self.outcome}"]
        lines += [f"  {n}: {c['status']} {c.get('detail', '')}"
                  for n, c in self.cases.items()]
        if self.problems:
            lines.append("simulator problems:")
            lines += ["  " + p for p in self.problems]
        tail = [f"  [{e.get('src')}] {e.get('text')}" for e in self.log[-40:]]
        if tail:
            lines.append("log tail:")
            lines += tail
        return "\n".join(lines)

    def assert_clean_exit(self):
        """The app ran to T.done() and returned normally, and the sim is
        healthy."""
        assert not self.problems, self.describe()
        assert self.outcome.get("result") == "returned", self.describe()
        assert self.done, "the app never reached T.done()\n" + self.describe()

    def assert_all_passed(self, expected: Iterable[str] = ()):
        """Every case passed (no FAIL, no SKIP) and each expected case ran."""
        self.assert_clean_exit()
        missing = [n for n in expected if n not in self.cases]
        assert not missing, f"cases never reported: {missing}\n" + self.describe()
        bad = {n: c for n, c in self.cases.items() if c["status"] != "PASS"}
        assert not bad, "\n".join(
            f"{n}: {c['status']} {c.get('detail', '')}" for n, c in bad.items())

    def check_case(self, name: str):
        """Assert one case: fail on FAIL or a missing result, pytest.skip on
        SKIP (the skip allow-list decides whether that is acceptable)."""
        assert not self.problems, self.describe()
        case = self.cases.get(name)
        if case is None:
            pytest.fail(f"case {name!r} never reported "
                        f"(the app stopped before it ran)\n" + self.describe())
        if case["status"] == "SKIP":
            pytest.skip(f"Lua case skipped: {case.get('detail', '')}")
        assert case["status"] == "PASS", f"{name}: {case.get('detail', '')}"


def results_path(sd: Path, app_id: str) -> Path:
    return Path(sd) / "data" / app_id / "test_results.json"


def run_lua_app(sim, name: str, timeout: float = 30.0) -> LuaRun:
    """Launch a picotest fixture app, wait for it to exit, return its cases.

    Reads /data/<APP_ID>/test_results.json from the SD card on the host; if
    the app died before writing it, falls back to the "[T] CASE" log lines.
    Never raises on a failed case: use LuaRun.assert_* / check_case.
    """
    sd = Path(sim.sd_card_path)
    app_id = app_id_of(sd, name)
    res_file = results_path(sd, app_id)
    if res_file.exists():
        res_file.unlink()

    log = []
    try:
        start_seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
        sim.launch_app(name)
        outcome = sim.wait_for_exit(timeout=timeout)
    except TimeoutError as e:
        outcome = {"name": name, "result": "timeout", "error": str(e)}
    except Exception as e:
        # A simulator that died mid-run drops the connection (RuntimeError
        # "Not connected"). Report the crash evidence, not the socket error.
        # The socket can close a moment before the process is reapable.
        if sim.process is not None:
            try:
                sim.process.wait(timeout=2)
            except Exception:
                pass
        if sim.is_alive():
            raise
        outcome = {"name": name, "result": "simulator_died",
                   "error": f"{type(e).__name__}: {e}"}
    problems = sim.health_problems()
    if not problems and sim.is_alive():
        log = sim.get_log_lines(start_seq)
    elif problems:
        stderr = sim.get_output()["stderr"].strip()
        if stderr:
            problems.append("stderr tail:\n" + "\n".join(stderr.splitlines()[-40:]))

    run = LuaRun(name=name, outcome=outcome, results=None, log=log,
                 problems=problems, sd=sd)
    if res_file.exists():
        try:
            run.results = json.loads(res_file.read_text())
        except ValueError as e:
            run.problems.append(f"unreadable {res_file}: {e}")
    if run.results:
        for c in run.results.get("cases", []):
            run.cases[c["name"]] = {"status": c["status"],
                                    "detail": c.get("detail", "")}
        run.done = bool(run.results.get("done"))
    else:
        for entry in log:
            text = entry.get("text", "")
            m = _LOG_CASE_RE.match(text)
            if m:
                run.cases[m.group(1)] = {"status": m.group(2),
                                         "detail": m.group(3) or ""}
            elif _LOG_DONE_RE.match(text):
                run.done = True
    return run


def log_texts(entries) -> list[str]:
    """Texts of get_log_buffer / get_log_lines entries."""
    return [e if isinstance(e, str) else e.get("text", "") for e in entries]


# ── Heap metrics ────────────────────────────────────────────────────────────

# The simulator's umm_* is a counting allocator (simulator/stubs/
# driver_stubs.c), so get_heap_info's lua_heap_free_kb is 8 MB minus the live
# umm/Lua bytes. test_system.py::test_heap_metrics_live proves that; the leak
# tests call require_heap_metrics_live first, so if the metric ever goes
# constant again they fail pointing at it instead of passing vacuously.


def measure_heap_probe(sim) -> tuple[int, int, int]:
    """lua_heap_free_kb before heap_probe, while it holds ~2 MB, and after it
    exits."""
    before = sim.call("get_heap_info")["lua_heap_free_kb"]
    sim.launch_app("heap_probe")
    sim.wait_for_log(r"^HP:HOLD", timeout=10)
    during = sim.call("get_heap_info")["lua_heap_free_kb"]
    sim.keypress("q")
    outcome = sim.wait_for_exit(timeout=10)
    assert outcome.get("result") == "returned", outcome
    after = sim.call("get_heap_info")["lua_heap_free_kb"]
    return before, during, after


def assert_heap_metrics_live(sim, min_drop_kb: int = 1024):
    """Fail unless lua_heap_free_kb drops while heap_probe holds ~2 MB."""
    before, during, _ = measure_heap_probe(sim)
    assert before - during >= min_drop_kb, (
        f"get_heap_info did not see a 2 MB allocation "
        f"(free before={before} KB, while held={during} KB)")


def require_heap_metrics_live(sim):
    """Precondition for leak tests: a constant metric would make any leak
    assertion pass vacuously."""
    try:
        assert_heap_metrics_live(sim)
    except AssertionError as e:
        pytest.fail("precondition failed: heap metrics are not live "
                    f"(see test_system.py::test_heap_metrics_live): {e}",
                    pytrace=False)


# ── Golden images ───────────────────────────────────────────────────────────


def compare_golden(got, golden_path: Path, update: bool = False):
    """Compare an RGB numpy array with a golden PNG, pixel for pixel.

    A missing golden FAILS (it used to be written and the test skipped, so a
    run from the wrong cwd compared the simulator against itself). With
    --update-baselines the golden is rewritten instead and the check passes.
    """
    import numpy as np
    from PIL import Image

    golden_path = Path(golden_path)
    if update:
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(got).save(golden_path)
        return
    if not golden_path.exists():
        pytest.fail(f"golden image missing: {golden_path} "
                    f"(run with --update-baselines to create it)", pytrace=False)
    want = np.array(Image.open(golden_path).convert("RGB"))
    assert want.shape == got.shape, f"size {got.shape} != golden {want.shape}"
    diff = np.argwhere(np.any(want != got, axis=-1))
    assert diff.size == 0, (
        f"{golden_path.name} differs in {len(diff)} pixels, "
        f"first at (x={diff[0][1]}, y={diff[0][0]})")


# ── Known-bug cases ─────────────────────────────────────────────────────────


def known_bug(reason: str):
    """Strict xfail for a case that fails today because of a known, unfixed
    bug. The fix makes the case pass (XPASS), which fails the run until the
    marker is removed."""
    return pytest.mark.xfail(strict=True, reason=reason)


def case_params(names, known_bugs: dict):
    """pytest.param per Lua case name, with the known-bug xfail attached to
    the cases listed in `known_bugs` ({case: reason})."""
    unknown = set(known_bugs) - set(names)
    assert not unknown, f"known_bugs names cases that don't exist: {unknown}"
    return [pytest.param(n, id=n, marks=[known_bug(known_bugs[n])]
                         if n in known_bugs else [])
            for n in names]


def write_wav(path: Path, seconds: float = 0.05, rate: int = 22050):
    """A valid mono 16-bit PCM WAV of silence."""
    import wave
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * int(seconds * rate))


_QOA_SCALEFACTOR_TAB = [1, 7, 21, 45, 84, 138, 211, 304,
                        421, 562, 731, 928, 1157, 1419, 1715, 2048]
_QOA_DEQUANT_TAB = [
    [1, -1, 3, -3, 5, -5, 7, -7],
    [5, -5, 18, -18, 32, -32, 49, -49],
    [16, -16, 53, -53, 95, -95, 147, -147],
    [34, -34, 113, -113, 203, -203, 315, -315],
    [63, -63, 210, -210, 378, -378, 588, -588],
    [104, -104, 345, -345, 621, -621, 966, -966],
    [158, -158, 528, -528, 950, -950, 1477, -1477],
    [228, -228, 760, -760, 1368, -1368, 2128, -2128],
    [316, -316, 1053, -1053, 1895, -1895, 2947, -2947],
    [422, -422, 1405, -1405, 2529, -2529, 3934, -3934],
    [548, -548, 1828, -1828, 3290, -3290, 5117, -5117],
    [696, -696, 2320, -2320, 4176, -4176, 6496, -6496],
    [868, -868, 2893, -2893, 5207, -5207, 8099, -8099],
    [1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933],
    [1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005],
    [1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336],
]
# Residuals -8..8 -> 3-bit quantized index (from the QOA spec).
_QOA_QUANT_TAB = [7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6]


def write_qoa(path: Path, seconds: float = 1.0, rate: int = 22050,
              channels: int = 1, hz: int = 0):
    """A valid QOA file ("qoaf"), encoded per the reference algorithm but
    with a two-pass per-slice scalefactor pick instead of the brute-force
    16-way search (25x faster, still spec-valid; the fixtures don't need
    fidelity).  Content is a sine of `hz` (0 = near-silence)."""
    import math
    import struct
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    total = int(seconds * rate)
    pcm = [
        [int(12000 * math.sin(2 * math.pi * hz * i / rate)) if hz else 0
         for _ in range(channels)]
        for i in range(total)
    ]

    def predict(lms):
        return sum(w * h for w, h in zip(lms[0], lms[1])) >> 13

    def update(lms, sample, residual):
        delta = residual >> 4
        lms[0] = [w + (-delta if h < 0 else delta) for w, h in zip(lms[0], lms[1])]
        lms[1] = lms[1][1:] + [sample]

    out = bytearray(b"qoaf" + struct.pack(">I", total))
    pos = 0
    while pos < total:
        fsamples = min(256 * 20, total - pos)
        slices = (fsamples + 19) // 20
        fsize = 8 + 16 * channels + 8 * slices * channels
        out += struct.pack(">B", channels) + rate.to_bytes(3, "big") + \
               struct.pack(">HH", fsamples, fsize)
        # Per the reference encoder, frames start from weights {0,0,-1,2}
        # (in .13 fixed point) and zero history.
        lms = [[[0, 0, -(1 << 13), 1 << 14], [0, 0, 0, 0]]
               for _ in range(channels)]  # [channel][weights, history]
        for w, h in lms:
            out += struct.pack(">4h", *h) + struct.pack(">4h", *w)
        for s in range(slices):
            for c in range(channels):
                n = min(20, fsamples - s * 20)
                # Pass 1: residuals under perfect reconstruction, to size
                # the scalefactor.
                probe = [list(lms[c][0]), list(lms[c][1])]
                peak = 0
                for k in range(n):
                    r = pcm[pos + s * 20 + k][c] - predict(probe)
                    peak = max(peak, abs(r))
                    update(probe, pcm[pos + s * 20 + k][c], r)
                sf = 0
                while sf < 15 and peak > _QOA_SCALEFACTOR_TAB[sf] * 8:
                    sf += 1
                # Pass 2: encode for real.
                slice_bits = sf
                for k in range(n):
                    predicted = predict(lms[c])
                    residual = pcm[pos + s * 20 + k][c] - predicted
                    scaled = (residual * ((1 << 16) // _QOA_SCALEFACTOR_TAB[sf])
                              + (1 << 15)) >> 16
                    quantized = _QOA_QUANT_TAB[max(-8, min(8, scaled)) + 8]
                    dequantized = _QOA_DEQUANT_TAB[sf][quantized]
                    reconstructed = max(-32768, min(32767, predicted + dequantized))
                    update(lms[c], reconstructed, dequantized)
                    slice_bits = (slice_bits << 3) | quantized
                slice_bits <<= (20 - n) * 3
                out += slice_bits.to_bytes(8, "big")
        pos += fsamples
    path.write_bytes(bytes(out))


def write_mp3(path: Path, frames: int = 8):
    """A decodable MP3 of silence: MPEG-1 Layer III, 128 kbps, 44.1 kHz,
    joint stereo, no padding (417-byte frames; all-zero side info and main
    data decode to silence)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    frame = b"\xff\xfb\x90\x64" + b"\x00" * (417 - 4)
    path.write_bytes(frame * frames)


def write_mod(path: Path):
    """A minimal valid ProTracker MOD ("M.K.", 4 channels): one silent
    pattern, no sample data."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    header = b"picodeck test".ljust(20, b"\x00")
    # 31 sample headers: name, length (words), finetune, volume, loop
    # start, loop length (1 word = no loop).
    sample = b"\x00" * 22 + b"\x00\x00" + b"\x00" + b"\x40" + b"\x00\x00" + b"\x00\x01"
    header += sample * 31
    header += bytes([1, 127]) + bytes(128) + b"M.K."
    path.write_bytes(header + bytes(64 * 4 * 4))
