"""What a native app sees of the machine and the API, in the simulator.

The simulator exists to catch what would go wrong on the device, so each of
these is a way it used to pass an app the device would stop, or stop an app the
device would run. tests/e2e/native/fidelity_probe.c is one ELF with one behaviour
per mode; the mode is the app's name in app.json.
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

import elfgen
from test_native_malformed import stage_native_app

pytestmark = [pytest.mark.native, pytest.mark.sd(fixtures=[], reserve=4)]

PROBE =Path(__file__).parent / "fixtures" / "native_fidelity"
ELF = (PROBE / "main.elf").read_bytes()

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)

# lua_psram_alloc.c's heap, the 64 KB app stack (native_loader.c).
HEAP = 6 * 1024 * 1024 - 128 * 1024
STACK = 64 * 1024


def _start(sim, mode, ready=None):
    d = Path(sim.sd_card_path) / "apps" / mode
    d.mkdir(parents=True, exist_ok=True)
    shutil.copy(PROBE / "main.elf", d / "main.elf")
    (d / "app.json").write_text(json.dumps({"id": f"com.test.{mode}", "name": mode}))
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app(mode)
    if ready:
        sim.wait_for_log(ready, timeout=15, since_seq=seq)
    return seq


def _run(sim, mode):
    """Run a mode that ends by itself. Returns (outcome, log text)."""
    seq = _start(sim, mode)
    outcome = sim.wait_for_exit(timeout=30)
    lines = sim.get_log_buffer(since_seq=seq).get("lines", [])
    return outcome, "\n".join(line["text"] for line in lines)


def _pixels(sim, *points):
    img = sim.screenshot_pil().convert("RGB")
    return [img.getpixel(p) for p in points]


def _leave(sim):
    assert sim.exit_app().get("ok")
    assert sim.wait_for_exit(timeout=10).get("result") == "returned"


def test_a_string_at_the_top_of_the_stack_reads_whole(simulator):
    # The API reads strings out of emulated memory in chunks, which must not run
    # off the end of the stack: picodeck_main's locals are right below it.
    _, log = _run(simulator, "string_at_stack_top")
    assert "TOP [ok]" in log, log


# --- The framebuffer: raw writes and draw calls are one buffer ---------------

def test_raw_writes_and_draw_calls_compose_in_order(simulator):
    # Pointer fetched first: clear, raw over it, a draw over that, raw again.
    seq = _start(simulator, "fb_order", r"FB READY")
    assert _pixels(simulator, (100, 100), (0, 0), (5, 5), (6, 6)) == \
        [RED, BLUE, GREEN, WHITE]
    # The pointer also reads what the draw calls drew (byte-swapped pure red).
    simulator.wait_for_log(r"FB read f8", timeout=5, since_seq=seq)
    _leave(simulator)


def test_raw_writes_show_after_a_draw_call_in_the_same_frame(simulator):
    _start(simulator, "fb_refetch", r"FB READY")
    assert _pixels(simulator, (5, 5), (50, 50)) == [BLUE, RED]
    _leave(simulator)


@pytest.mark.parametrize("mode", ["flush_region", "flush_rows"])
def test_partial_present_shows_draw_calls(simulator, mode):
    # With no raw writes there is nothing to take from the emulated buffer.
    _start(simulator, mode, r"FB READY")
    assert _pixels(simulator, (160, 160)) == [RED]
    _leave(simulator)


def test_terminal_render_survives_flush(simulator):
    # A draw path that is not a display primitive, with nothing else in the frame.
    _start(simulator, "term_only", r"FB READY")
    img = simulator.screenshot_pil().convert("RGB")
    assert any(c != (0, 0, 0) for _, c in img.getcolors(maxcolors=1 << 17))
    _leave(simulator)


# --- qmiAlloc: a real heap, the device's size --------------------------------

def test_qmi_free_really_frees(simulator):
    outcome, log = _run(simulator, "qmi_recycle")
    assert "QMI recycled" in log, log
    assert outcome["result"] == "returned"


def test_qmi_free_merges_neighbours(simulator):
    _, log = _run(simulator, "qmi_merge")
    assert "QMI merged 1 same 1" in log, log


def test_qmi_heap_is_the_devices_size(simulator):
    # The device has ~6 MB and refuses more; umm_malloc(0) is NULL.
    _, log = _run(simulator, "qmi_budget")
    assert "QMI 5MB 1" in log, log
    assert "QMI 7MB 0" in log, log
    assert "QMI 0 0" in log, log


# --- The image limit ---------------------------------------------------------

def _load(sim, name, memsz):
    elf = elfgen.build_elf(load=dict(p_memsz=memsz))
    stage_native_app(sim.sd_card_path, name, elf)
    sim.launch_app(name)
    return sim.wait_for_exit(timeout=15)


def test_image_larger_than_the_device_heap_is_refused(simulator):
    # One byte more than fits with the stack, then the largest that fits.
    outcome = _load(simulator, "big_image", HEAP - STACK + 1)
    assert outcome["result"] == "load_failed", outcome
    assert "ELF: out of PSRAM" in outcome["error"], outcome


def test_image_that_fits_the_device_heap_loads(simulator):
    outcome = _load(simulator, "fits_image", HEAP - STACK)
    assert outcome["result"] == "returned", outcome


def test_image_over_the_loaders_limit_is_refused_as_too_large(simulator):
    outcome = _load(simulator, "huge_image", 7 * 1024 * 1024 + 1)
    assert outcome["result"] == "load_failed", outcome
    assert "ELF: image too large" in outcome["error"], outcome


# --- Faults: what would stop the app on the device stops it here --------------

@pytest.mark.parametrize("mode", ["null_write", "wild_read", "wild_write",
                                  "wild_call"])
def test_bad_access_ends_the_app_with_an_error(simulator, mode):
    outcome, log = _run(simulator, mode)
    assert "FAULT READY" in log, log
    assert "SURVIVED" not in log, log
    assert outcome["result"] == "error", outcome
    assert "fault" in outcome["error"], outcome
    assert simulator.ping()
    # The launcher runs the next app normally after a fault.
    outcome, log = _run(simulator, "qmi_recycle")
    assert outcome["result"] == "returned", (outcome, log)


@pytest.mark.parametrize("mode", ["null_read", "stack_48k"])
def test_what_the_device_allows_still_runs(simulator, mode):
    # Address 0 is the boot ROM on the device: reading it does not fault.
    outcome, log = _run(simulator, mode)
    assert "SURVIVED" in log, log
    assert "stack passed" not in log, log
    assert outcome["result"] == "returned", outcome


@pytest.mark.parametrize("mode", ["stack_128k", "stack_600k"])
def test_stack_past_the_devices_is_reported_and_keeps_running(simulator, mode):
    # The device's 64 KB would fault here. A real app (C-Dogs) goes well past it in
    # the simulator, so this says where instead of stopping the app.
    outcome, log = _run(simulator, mode)
    assert log.count("stack passed 64 KB at PC=") == 1, log
    assert "SURVIVED" in log, log
    assert outcome["result"] == "returned", outcome
