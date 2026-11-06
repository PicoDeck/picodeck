"""The SDK's recipes as an outside developer follows them.

docs/Native-Loading.md says how to use the C library (sdk/native/stubs.c with -lc
-lm -lgcc) and what works in C++. These run those recipes' committed builds
(tests/e2e/native/Makefile), and check what the sample app does at an exit request.
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from helpers import PROJECT_ROOT

pytestmark = [pytest.mark.native, pytest.mark.sd(fixtures=[], reserve=3)]

FIXTURES = Path(__file__).parent / "fixtures"


def _stage(sim, src: Path, name: str):
    shutil.copytree(src, Path(sim.sd_card_path) / "apps" / name, dirs_exist_ok=True)


def _run(sim, src: Path, name: str):
    _stage(sim, src, name)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(name)
    outcome = sim.wait_for_exit(timeout=30)
    lines = sim.get_log_buffer(since_seq=seq).get("lines", [])
    return outcome, "\n".join(line["text"] for line in lines)


def test_c_library_recipe(simulator):
    outcome, log = _run(simulator, FIXTURES / "native_libc", "native_libc")
    assert outcome["result"] == "returned", (outcome, log)
    assert "LIBC copy 7 len 17" in log, log       # "Native Libc Probe"
    assert "LIBC float 43 div 3333" in log, log   # soft-float, 64-bit divide
    assert "LIBC sqrt 4 sin 909" in log, log      # libm
    assert "LIBC snprintf 42|   ab|beef" in log, log
    assert "LIBC float format [3.14]" in log, log  # full newlib, not nano
    assert "LIBC malloc 1" in log, log
    assert "LIBC malloc over the heap 0" in log, log
    assert "LIBC DONE" in log, log


def test_cpp_subset(simulator):
    outcome, log = _run(simulator, FIXTURES / "native_cpp", "native_cpp")
    assert outcome["result"] == "returned", (outcome, log)
    assert "CPP virtual 36 template 42" in log, log
    assert "CPP array 5" in log, log
    # The docs say so: the loader does not run static constructors.
    assert "CPP static constructor ran 0" in log, log
    assert "CPP DONE" in log, log


@pytest.mark.parametrize("app", ["apps/hello_c", "sdk/native"])
def test_sample_app_honours_an_exit_request(simulator, app):
    """The dev `exit` sets what the system menu's Exit App sets, with no key: only
    sys->shouldExit() can end the app. A stale hello_c used to ignore it."""
    dst = Path(simulator.sd_card_path) / "apps" / "hello_c"
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copy(PROJECT_ROOT / app / "main.elf", dst / "main.elf")
    (dst / "app.json").write_text(json.dumps({"id": "com.test.hello", "name": "hello_c"}))
    simulator.launch_app("hello_c")
    simulator.wait_frames(2, timeout=10)
    simulator.call("dev_command", {"cmd": "exit"})
    assert simulator.wait_for_exit(timeout=10).get("result") == "returned"
