"""E2E tests for dev commands sent through the control channel.

The `dev_command` RPC queues one dev-command line for Core 0, exactly as a
line typed on the device's USB serial console: the launcher loop, a running
Lua app's hook / sys.sleep pump or a native app's sys->poll() executes it. That is the path `push_app`
relies on on hardware (`unzip <zip> <dest>`, then `rm <zip>`).

The simulator cannot model the 4 KB main stack, so the unzip tests are
coverage for the command path and its reply; the stack fix itself (commands
run on a PSRAM app stack) is verified on hardware.
"""
import io
import shutil
import time
import zipfile
from pathlib import Path

import pytest

FIXTURE_APP = Path(__file__).parent / "apps" / "zip_test"
FIXTURES = Path(__file__).parent / "fixtures"


def _dev(sim, cmd, timeout=20.0):
    return sim.call("dev_command", {"cmd": cmd}, timeout=timeout)


def _build_archive(dest: Path) -> dict:
    """A multi-file archive in push_app's shape: the zip_test fixture app plus
    nested directories and a binary member. Returns {name: bytes}."""
    members = {}
    for p in sorted(FIXTURE_APP.rglob("*")):
        if p.is_file():
            members[p.relative_to(FIXTURE_APP).as_posix()] = p.read_bytes()
    members["assets/img/tile.bin"] = bytes(range(256)) * 64
    members["assets/snd/a/b/deep.txt"] = b"deep nested\n" * 50
    members["empty.txt"] = b""
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return members


def _assert_extracted(root: Path, members: dict):
    for name, data in members.items():
        f = root / name
        assert f.is_file(), f"{name} was not extracted"
        assert f.read_bytes() == data, f"{name} differs after extraction"


def _wait_running(sim, timeout=10.0):
    """Wait for the launched app to be running. get_running_app always
    answers {"running": bool, "name": str|null}; before the launch is picked
    up it is running:false, so keep polling rather than read it once."""
    deadline = time.monotonic() + timeout
    while True:
        status = sim.call("get_running_app")
        if status["running"]:
            return status["name"]
        if time.monotonic() >= deadline:
            pytest.fail(f"app did not start: {status}")
        time.sleep(0.02)


def test_exit_at_launcher_is_refused(simulator):
    """`exit` with no app running is an error reply, not a shutdown/fault.

    On hardware the launcher loop used to return from launcher_run(), main()
    returned and newlib's _exit hit a breakpoint: a HardFault and reboot.
    """
    r = _dev(simulator, "exit")
    assert r["ok"] is False
    assert "no app running" in r["output"]

    r2 = simulator.exit_app()
    assert r2.get("ok") is False
    assert "no app running" in r2.get("message", "")

    # A later command that Core 0 answers proves the launcher loop carried
    # on past the refused exit (a shutdown would never reply).
    r3 = _dev(simulator, "ping")
    assert r3["ok"] is True, r3
    assert simulator.is_alive(), "simulator shut down on exit at the launcher"
    assert simulator.ping()

    # The refused exit must not linger and kill the next app on launch.
    simulator.launch_app("harness_ok")
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome["result"] == "returned", outcome
    assert "H:DONE" in "\n".join(
        l if isinstance(l, str) else l.get("text", "")
        for l in simulator.get_log_buffer()["lines"])


def test_unzip_and_rm_at_launcher(simulator, test_sd_card):
    members = _build_archive(test_sd_card / "data" / "push.zip")

    r = _dev(simulator, "unzip /data/push.zip /data/unz")
    assert r["ok"] is True, r
    assert f"Unzipped {len(members)} files (0 skipped)" in r["output"]
    _assert_extracted(test_sd_card / "data" / "unz", members)

    r = _dev(simulator, "rm /data/unz")
    assert r["ok"] is True, r
    assert not (test_sd_card / "data" / "unz").exists()

    r = _dev(simulator, "unzip /data/missing.zip /data/unz")
    assert r["ok"] is False
    assert "Error: unzip failed" in r["output"]
    assert simulator.is_alive()


def test_unzip_while_lua_app_running(simulator, test_sd_card):
    """The command runs inside the app's Lua pump (on its app stack on
    hardware: no nested app-stack switch), then `exit` stops the app."""
    members = _build_archive(test_sd_card / "data" / "push.zip")

    simulator.launch_app("harness_ticker")
    _wait_running(simulator)

    r = _dev(simulator, "unzip /data/push.zip /data/unz_app")
    assert r["ok"] is True, r
    assert f"Unzipped {len(members)} files" in r["output"]
    _assert_extracted(test_sd_card / "data" / "unz_app", members)
    assert simulator.call("get_running_app")["running"], \
        "app stopped during unzip"

    r = _dev(simulator, "exit")
    assert r["ok"] is True, r
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome["result"] in ("returned", "exit_sentinel"), outcome
    assert simulator.is_alive()


@pytest.mark.sd(fixtures=[], reserve=1)  # stages native_pad_probe
def test_unzip_while_native_app_running(simulator, test_sd_card):
    """A native app's sys->poll() serves the command, as the firmware's
    sys_poll does: unzip and rm work while the app runs, then `exit`."""
    members = _build_archive(test_sd_card / "data" / "push.zip")
    shutil.copytree(FIXTURES / "native_pad_probe",
                    test_sd_card / "apps" / "native_pad_probe")

    simulator.launch_app("native_pad_probe")
    _wait_running(simulator)

    r = _dev(simulator, "unzip /data/push.zip /data/unz_native")
    assert r["ok"] is True, r
    assert f"Unzipped {len(members)} files" in r["output"]
    _assert_extracted(test_sd_card / "data" / "unz_native", members)
    assert simulator.call("get_running_app")["running"], \
        "app stopped during unzip"

    r = _dev(simulator, "rm /data/push.zip")
    assert r["ok"] is True, r
    assert not (test_sd_card / "data" / "push.zip").exists()

    r = _dev(simulator, "exit")
    assert r["ok"] is True, r
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome["result"] in ("returned", "exit_sentinel"), outcome
    assert simulator.is_alive()


@pytest.mark.sd(fixtures=[], reserve=1)  # stages native_nopoll
def test_dev_command_times_out_on_a_native_app_that_never_polls(simulator,
                                                                test_sd_card):
    """Nothing pumps dev commands while a native app stays out of
    sys->poll() (the fixture spins for 4 s), as on firmware: the RPC gives
    up at its own timeout, the abandoned command does not run late, and the
    next one is served once the app polls again."""
    shutil.copytree(FIXTURES / "native_nopoll",
                    test_sd_card / "apps" / "native_nopoll")
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("native_nopoll")
    simulator.wait_for_log(r"NP:START", timeout=10, since_seq=seq)

    t0 = time.monotonic()
    r = simulator.call("dev_command", {"cmd": "ping", "timeout_ms": 1000},
                       timeout=10)
    assert r["ok"] is False and "timed out waiting for Core 0" in r["output"], r
    assert 0.8 <= time.monotonic() - t0 < 3.0

    simulator.wait_for_log(r"NP:BUSYDONE", timeout=10, since_seq=seq)
    r = _dev(simulator, "ping")
    assert r["ok"] is True and r["output"] == "pong", r
    r = _dev(simulator, "exit")
    assert r["ok"] is True, r
    assert simulator.wait_for_exit(timeout=15)["result"] in ("returned",
                                                              "exit_sentinel")
