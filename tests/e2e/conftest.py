"""pytest fixtures and hooks for the PicoDeck E2E suite.

Usage (from any directory):
    SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
    pytest tests/e2e --show-window          # watch the simulator
    pytest tests/e2e --update-baselines     # rewrite golden PNGs

Every simulator a test uses is health-checked after the test (process alive,
no crash log, no sanitizer report on stderr); a failing test gets the
simulator's stdout/stderr tails, log buffer, /system/error.log and a
screenshot attached. Helpers (SD staging, the Lua test kit runner) live in
helpers.py.
"""

import base64
import fnmatch
import os
import re
import subprocess
from pathlib import Path

import pytest

import hw_target
from helpers import (DEFAULT_SD_SOURCE, E2E_DIR, build_sd_card, new_simulator,
                     run_lua_app, stop_and_check)
from picodeck_simulator import (PicodeckSimulator, binary_firmware_net,
                             binary_sanitizers)

SKIP_ALLOWLIST = E2E_DIR / "skip_allowlist.txt"


def pytest_addoption(parser):
    parser.addoption(
        "--update-baselines", action="store_true", default=False,
        help="Rewrite golden PNGs instead of comparing against them",
    )
    parser.addoption(
        "--show-window", action="store_true", default=False,
        help="Show the simulator's SDL window (default: headless)",
    )
    parser.addoption(
        "--port", action="store", type=int, default=0,
        help="TCP port for the simulator (0 = auto-assign)",
    )
    parser.addoption(
        "--simulator-path", action="store",
        default=str(PicodeckSimulator.DEFAULT_BINARY),
        help="Path to the picodeck_simulator binary (default: $PICODECK_SIM_BINARY, "
             "else build_sim/picodeck_simulator)",
    )
    parser.addoption(
        "--sd-card-path", action="store", default=str(DEFAULT_SD_SOURCE),
        help="SD card the manifest takes apps/hello (and sd-marker extras) from",
    )
    parser.addoption(
        "--artifacts-dir", action="store", default=None,
        help="Write per-failure diagnostics (stdout/stderr, log, error.log, "
             "screenshot) under this directory",
    )
    hw_target.add_target_option(parser)  # --target sim | hw:<port>


def sim_sanitizers(config):
    """(probe_ok, kinds) for the simulator under test: kinds is the set of
    -fsanitize= kinds from its `--build-info` (empty for a release build).
    probe_ok is False when the probe failed, so nothing is known.
    PICODECK_SIM_SANITIZE set by hand overrides the probe."""
    manual = os.environ.get("PICODECK_SIM_SANITIZE")
    if manual:
        return True, set(re.split(r"[;,]", manual)) - {""}
    san = binary_sanitizers(Path(config.getoption("--simulator-path")))
    if san is None:
        return False, set()
    return True, set(san.split(",")) - {""}


def sim_is_sanitized(config) -> bool:
    """True when the simulator under test is an ASan build."""
    return "address" in sim_sanitizers(config)[1]


def pytest_configure(config):
    """PICODECK_SIM_EXPECT_SANITIZE (set by the sanitizer CI legs, e.g.
    "address"): refuse to run unless the binary's --build-info confirms it,
    so a failed probe can't turn an ASan leg into a release run whose
    asan_only tests all skip. PICODECK_SIM_EXPECT_FIRMWARE_NET=1 does the same
    for the firmware_net tests (make simulator-net)."""
    hw_target.configure_target(config)  # a bad --target fails here
    if os.environ.get("PICODECK_SIM_EXPECT_FIRMWARE_NET") == "1":
        # The firmware-net CI legs: a failed probe or a default build must
        # not quietly skip every firmware_net test.
        net_ok, net_on = sim_firmware_net(config)
        if not (net_ok and net_on):
            raise pytest.UsageError(
                "PICODECK_SIM_EXPECT_FIRMWARE_NET=1 but "
                f"`{config.getoption('--simulator-path')} --build-info` "
                + ("failed" if not net_ok else "reports firmware_net=0")
                + " (build it with make simulator-net)")
    expect = set(re.split(r"[;,]", os.environ.get("PICODECK_SIM_EXPECT_SANITIZE", ""))) - {""}
    if not expect:
        return
    probe_ok, kinds = sim_sanitizers(config)
    binary = config.getoption("--simulator-path")
    if not probe_ok:
        raise pytest.UsageError(
            f"PICODECK_SIM_EXPECT_SANITIZE={','.join(sorted(expect))} but "
            f"`{binary} --build-info` failed (missing binary, crash or timeout)")
    missing = expect - kinds
    if missing:
        raise pytest.UsageError(
            f"PICODECK_SIM_EXPECT_SANITIZE={','.join(sorted(expect))} but "
            f"{binary} was built with sanitize="
            f"{','.join(sorted(kinds)) or '(none)'}; missing {','.join(sorted(missing))}")


def sim_firmware_net(config):
    """(probe_ok, on): does the simulator under test run the firmware
    network stack (SIM_FIRMWARE_NET, make simulator-net)?"""
    on = binary_firmware_net(Path(config.getoption("--simulator-path")))
    return on is not None, bool(on)


def pytest_collection_modifyitems(config, items):
    """asan_only tests need an ASan build of the simulator
    (PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator); otherwise they skip.
    firmware_net tests need a SIM_FIRMWARE_NET build
    (PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator); otherwise they skip."""
    hw_target.apply_target_rules(config, items)  # hardware / both markers
    if any("firmware_net" in item.keywords for item in items):
        if not sim_firmware_net(config)[1]:
            skip_net = pytest.mark.skip(
                reason="firmware_net: needs the simulator built with the "
                       "firmware network stack (make simulator-net; "
                       "PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator)")
            for item in items:
                if "firmware_net" in item.keywords:
                    item.add_marker(skip_net)
    if not any("asan_only" in item.keywords for item in items):
        return
    if sim_is_sanitized(config):
        return
    skip = pytest.mark.skip(reason="asan_only: needs an ASan build of the "
                                   "simulator (make simulator-asan; "
                                   "PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator)")
    for item in items:
        if "asan_only" in item.keywords:
            item.add_marker(skip)


# ── Simulator lifecycle ─────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def simulator_binary(request) -> Path:
    """The simulator binary, built on demand."""
    binary_path = Path(request.config.getoption("--simulator-path")).resolve()
    # Only the default release binary is built on demand; a missing
    # PICODECK_SIM_BINARY / --simulator-path must not silently fall back to it.
    if not binary_path.exists() and binary_path == PicodeckSimulator.PROJECT_ROOT / "build_sim" / "picodeck_simulator":
        try:
            PicodeckSimulator.build()
        except subprocess.CalledProcessError as e:
            pytest.fail(f"Failed to build simulator: {e}")
    if not binary_path.exists():
        pytest.fail(f"Simulator binary not found: {binary_path}")
    return binary_path


@pytest.fixture
def test_sd_card(tmp_path, request):
    """Per-test SD card built from the manifest (see helpers.build_sd_card).
    `@pytest.mark.sd(extra=[...])` stages more. A test that stages apps at
    runtime asks for room: `@pytest.mark.sd(fixtures=["fs_test"], reserve=3)`
    puts only those fixture apps (plus hello) on the card and checks that
    3 more still fit under the launcher's MAX_APPS."""
    marker = request.node.get_closest_marker("sd")
    kw = marker.kwargs if marker else {}
    return build_sd_card(tmp_path / "sd_card", extra=kw.get("extra", ()),
                         fixtures=kw.get("fixtures"),
                         reserve=kw.get("reserve", 0),
                         default_sd=Path(request.config.getoption("--sd-card-path")))


@pytest.fixture
def sim_factory(request, simulator_binary, tmp_path):
    """Start extra simulators: sim_factory(sd_path, **PicodeckSimulator kwargs).
    Each is stopped and health-checked at teardown."""
    sims = []

    def start(sd_path, **kwargs):
        crash = tmp_path / f"crash_{len(sims)}.log"
        sim = new_simulator(request.config, simulator_binary, sd_path, crash,
                            **kwargs)
        sims.append(sim)
        _register(request.node, sim)
        return sim

    yield start
    errors = []
    for sim in sims:
        if getattr(sim, "_health_reported", False):
            sim.stop()  # the test already failed on this sim's health
            continue
        try:
            stop_and_check(sim)
        except pytest.fail.Exception as e:
            errors.append(str(e))
    if errors:
        pytest.fail("\n".join(errors), pytrace=False)


@pytest.fixture
def simulator(sim_factory, test_sd_card) -> PicodeckSimulator:
    """A fresh simulator per test on the per-test SD card."""
    return sim_factory(test_sd_card)


@pytest.fixture(scope="module")
def sim_module_factory(request, simulator_binary, tmp_path_factory):
    """Module-scoped simulators: start(setup=None, **kwargs) boots one on a
    fresh manifest SD card (`setup(sd_path)` runs before boot). For modules
    that share one run across many test ids. `fixtures` / `reserve` are as for
    build_sd_card (a module that stages apps asks for room). Stopped and
    health-checked when the module finishes."""
    sims = []

    def start(setup=None, fixtures=None, reserve=0, **kwargs):
        base = tmp_path_factory.mktemp("modsim")
        sd = build_sd_card(base / "sd_card", fixtures=fixtures, reserve=reserve,
                           default_sd=Path(request.config.getoption("--sd-card-path")))
        if setup:
            setup(sd)
        sim = new_simulator(request.config, simulator_binary, sd,
                            base / "crash.log", **kwargs)
        sims.append(sim)
        _register(request.node, sim)
        return sim

    yield start
    errors = []
    for sim in sims:
        if getattr(sim, "_health_reported", False):
            sim.stop()
            continue
        try:
            stop_and_check(sim)
        except pytest.fail.Exception as e:
            errors.append(str(e))
    if errors:
        pytest.fail("\n".join(errors), pytrace=False)


@pytest.fixture(scope="module")
def sim_module(sim_module_factory):
    """One simulator shared by a module (manifest SD card)."""
    return sim_module_factory()


@pytest.fixture(scope="session")
def lua_suite(request, simulator_binary, tmp_path_factory):
    """Run a picotest fixture app once in its own simulator:
    lua_suite(name, setup=None, timeout=30, fixtures=None, reserve=0)
    -> helpers.LuaRun (`fixtures` / `reserve`: see build_sd_card).

    For module-scoped fixtures that expand an app's cases into one pytest id
    each. `setup(sd_path)` runs before boot (stage files). The simulator is
    stopped after the run; its health problems land in LuaRun.problems.
    """
    def run(name, setup=None, timeout=30.0, fixtures=None, reserve=0):
        base = tmp_path_factory.mktemp(f"suite_{name}")
        sd = build_sd_card(base / "sd_card", fixtures=fixtures, reserve=reserve,
                           default_sd=Path(request.config.getoption("--sd-card-path")))
        if setup:
            setup(sd)
        sim = new_simulator(request.config, simulator_binary, sd,
                            base / "crash.log")
        try:
            result = run_lua_app(sim, name, timeout=timeout)
        finally:
            sim.stop()
        result.problems.extend(p for p in sim.health_problems()
                               if p not in result.problems)
        return result

    return run


@pytest.fixture
def target(request):
    """Where the test runs: a hw_target.SimTarget over this test's
    simulator, or the session's HwTarget on --target hw:<port>. Tests marked
    `both` or `hardware` use this instead of the simulator fixtures."""
    if hw_target.target_spec(request.config)[0] == "hw":
        return hw_target.session_target(request.config)
    return hw_target.SimTarget(request.getfixturevalue("simulator"))


@pytest.fixture
def update_baselines(request):
    return request.config.getoption("--update-baselines")


# ── Health check and failure diagnostics (audit R2) ─────────────────────────


def _register(node, sim):
    """Record a factory-made simulator on a collection node (the test item
    for sim_factory, the module for sim_module_factory) so the health hook
    finds it; factories hand tests a function, not the simulator."""
    if not hasattr(node, "_picodeck_sims"):
        node._picodeck_sims = []
    node._picodeck_sims.append(sim)


def _sims_of(item) -> list:
    """Every PicodeckSimulator the test used: fixture values that are
    simulators, plus simulators registered by sim_factory on the item and
    by sim_module_factory on its module. (lua_suite stops its simulator
    before returning; its health lands in LuaRun.problems instead.)"""
    funcargs = getattr(item, "funcargs", {}) or {}
    found = [v for v in funcargs.values() if isinstance(v, PicodeckSimulator)]
    for node in item.listchain():
        found += getattr(node, "_picodeck_sims", [])
    seen, out = set(), []
    for sim in found:
        if id(sim) not in seen:
            seen.add(id(sim))
            out.append(sim)
    return out


def _tail(text: str, n: int = 200) -> str:
    lines = text.splitlines()
    return "\n".join(lines[-n:])


def _collect_diagnostics(sim: PicodeckSimulator) -> dict:
    """Text sections plus an optional screenshot (PNG bytes)."""
    diag = {}
    out = sim.get_output()
    diag["stdout (tail)"] = _tail(out["stdout"])
    diag["stderr (tail)"] = _tail(out["stderr"])
    crash = sim.read_crash_log()
    if crash:
        diag["crash log"] = crash
    error_log = Path(sim.sd_card_path) / "system" / "error.log"
    if error_log.exists():
        diag["/system/error.log"] = _tail(error_log.read_text(errors="replace"))
    png = None
    if sim.is_alive():
        try:
            lines = sim.get_log_lines(0)
            diag["log buffer"] = "\n".join(
                f"{e.get('seq')} [{e.get('src')}] {e.get('text')}" for e in lines)
        except Exception as e:  # diagnostics are best effort
            diag["log buffer"] = f"(unavailable: {e})"
        try:
            png = sim.screenshot()
        except Exception as e:
            diag["screenshot"] = f"(unavailable: {e})"
    return {"sections": diag, "png": png}


def _attach(item, report, sims):
    html = item.config.pluginmanager.getplugin("html")
    extras = getattr(report, "extras", [])
    art_root = item.config.getoption("--artifacts-dir")
    for i, sim in enumerate(sims):
        d = _collect_diagnostics(sim)
        label = f"sim{i}" if len(sims) > 1 else "sim"
        for name, text in d["sections"].items():
            report.sections.append((f"{label} {name}", text))
            if html:
                extras.append(html.extras.text(text, name=f"{label} {name}"))
        if d["png"] and html:
            extras.append(html.extras.png(base64.b64encode(d["png"]).decode(),
                                          name=f"{label} screenshot"))
        if art_root:
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", item.nodeid)
            out = Path(art_root) / safe / label
            out.mkdir(parents=True, exist_ok=True)
            for name, text in d["sections"].items():
                fname = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") + ".txt"
                (out / fname).write_text(text)
            if d["png"]:
                (out / "screenshot.png").write_bytes(d["png"])
    if html:
        report.extras = extras


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if report.when not in ("setup", "call"):
        return
    sims = _sims_of(item)
    if not sims:
        _quarantine(item, report)
        return
    if report.when == "call":
        problems = []
        for sim in sims:
            found = sim.health_problems()
            if found:
                # Reported here (or expected by an xfail): don't fail the
                # fixture teardown a second time.
                sim._health_reported = True
                problems += found
        if problems and not hasattr(report, "wasxfail"):
            report.outcome = "failed"
            report.longrepr = ("Simulator health check failed:\n"
                               + "\n".join(problems))
    if report.failed:
        _attach(item, report, sims)
    _quarantine(item, report)


def _quarantine(item, report):
    """A failing @pytest.mark.flaky test is reported as a quarantined flake
    (an xfail with a "quarantined flake" reason, listed in its own summary
    section) instead of failing the run. No retries.

    Never for a health failure: if any simulator the test used crashed,
    wrote a crash log or reported a sanitizer error, the failure stands."""
    marker = item.get_closest_marker("flaky")
    if not marker or not report.failed or report.when not in ("setup", "call"):
        return
    if any(sim.health_problems() for sim in _sims_of(item)):
        return
    reason = marker.kwargs.get("reason", "") or (marker.args[0] if marker.args else "")
    report.outcome = "skipped"
    report.wasxfail = f"quarantined flake: {reason}"


# ── Skip allow-list (audit C7) ──────────────────────────────────────────────

_skipped: list = []


def _allowlist() -> list[str]:
    if not SKIP_ALLOWLIST.exists():
        return []
    pats = []
    for line in SKIP_ALLOWLIST.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            pats.append(line)
    return pats


_quarantined: list = []


def pytest_runtest_logreport(report):
    if report.skipped and not hasattr(report, "wasxfail"):
        _skipped.append(report)
    if str(getattr(report, "wasxfail", "")).startswith("quarantined flake"):
        _quarantined.append(report)


def _skip_allowed(report, patterns, probe_ok: bool = True,
                  net_probe_ok: bool = True) -> bool:
    if "hardware" in report.keywords:
        return True
    # Likewise a firmware_net skip on a simulator without the firmware
    # network stack, as long as --build-info answered.
    if "firmware_net" in report.keywords and net_probe_ok:
        return True
    # An asan_only skip is fine on a release build, but not when the
    # --build-info probe failed: then nobody knows the tests didn't need to run.
    if "asan_only" in report.keywords and probe_ok:
        return True
    nodeid = report.nodeid
    # Patterns are relative to tests/e2e so they work from any rootdir.
    short = nodeid.split("tests/e2e/", 1)[-1]
    return any(fnmatch.fnmatch(short, p) or fnmatch.fnmatch(nodeid, p)
               for p in patterns)


def pytest_sessionfinish(session, exitstatus):
    if hasattr(session.config, "workerinput"):
        return  # xdist worker: the controller sees every report
    patterns = _allowlist()
    probe_ok = sim_sanitizers(session.config)[0]
    net_probe_ok = sim_firmware_net(session.config)[0]
    bad = [r for r in _skipped
           if not _skip_allowed(r, patterns, probe_ok, net_probe_ok)]
    if bad:
        session.config._picodeck_bad_skips = bad
        if session.exitstatus == 0:
            session.exitstatus = pytest.ExitCode.TESTS_FAILED


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    if _quarantined and not hasattr(config, "workerinput"):
        terminalreporter.section("quarantined flaky tests that failed", yellow=True)
        for r in _quarantined:
            terminalreporter.line(f"{r.nodeid} ({r.when}): {r.wasxfail}")
    bad = getattr(config, "_picodeck_bad_skips", [])
    if bad:
        terminalreporter.section("skips not on tests/e2e/skip_allowlist.txt",
                                 red=True)
        for r in bad:
            reason = r.longrepr[2] if isinstance(r.longrepr, tuple) else r.longrepr
            terminalreporter.line(f"{r.nodeid}: {reason}")
        terminalreporter.line(
            "A skip hides a test that no longer runs. Fix the cause, or add "
            "the test to tests/e2e/skip_allowlist.txt with a reason.")
