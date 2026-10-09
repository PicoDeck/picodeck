"""The native exit flag (simulator/sim_native_api.c): an exit request reaches
a native app's shouldExit(), and does not outlive the app it was meant for.
Shared by the Unicorn trampolines and the web build's compiled-in apps
(PicoDeck/web-sim's platform/web_native.c)."""
import shutil
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.native

# Ends only when shouldExit() turns true; it ignores keys, so exit_app's
# injected Esc cannot end it (tests/e2e/native/exit_probe.c).
EXIT_PROBE = Path(__file__).parent / "fixtures" / "native_exit_probe"


def _start(sim):
    # Staged at run time, as test_gamepad.py does: one more app under
    # tests/e2e/apps would push the launcher past its 64-app cap.
    shutil.copytree(EXIT_PROBE, Path(sim.sd_card_path) / "apps" / "native_exit_probe",
                    dirs_exist_ok=True)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("native_exit_probe")
    sim.wait_for_log(r"EXITREADY", timeout=10, since_seq=seq)
    return seq


def _exit(sim, seq):
    # exit_app sets the exit request (dev_op_exit) and injects Esc; the probe
    # ignores the key, so only shouldExit() can end it.
    assert sim.exit_app().get("ok")
    sim.wait_for_log(r"EXITSEEN", timeout=10, since_seq=seq)
    assert sim.wait_for_exit(timeout=10).get("result") == "returned"


def test_exit_request_reaches_should_exit(simulator):
    _exit(simulator, _start(simulator))


def test_exit_request_does_not_outlive_the_app(simulator):
    _exit(simulator, _start(simulator))
    seq = _start(simulator)
    time.sleep(1.0)
    assert simulator.call("get_running_app").get("running"), \
        "the relaunched app saw the old exit request and quit at once"
    _exit(simulator, seq)
