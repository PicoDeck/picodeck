"""E2E tests for the panels.lua interactive-comics framework.

Drives the text-only panels_test fixture comic through every state machine
transition — advance, choice branch, story-var payoff, save on exit, resume
on relaunch — asserting on the library's own "PANELS:" log markers plus the
fixture's "PT " markers. panels.lua is staged onto the test SD from
system/lib/ (the canonical copy) at test time, exactly as firmware ships it.
"""
import re
import shutil
import time
from pathlib import Path

import pytest

# The simulator runs these tests on a paused virtual clock (--virtual-time,
# multiplier 0): panels.lua's animations are timed with sys.getTimeMs() and
# its frame loop never sleeps, so the clock moves only when the test calls
# step_time. Every timed wait is an exact step plus a frame barrier, never a
# wall-clock sleep. Durations come from Panels.Settings in system/lib/panels.lua.
ADVANCE_MS = 400      # advance-mode snap animation (input dropped while it runs)
TRANSITION_MS = 450   # each half of a fadeToBlack sequence change
SCROLL_STEP_MS = 100  # panels caps a frame's dt at 100 ms (scrollSpeed 180 px/s)

REPO_ROOT = Path(__file__).resolve().parents[2]


def _texts(sim):
    return [e["text"] for e in sim.get_log_lines(0)]


def _wait_for(sim, marker, timeout=15.0):
    """Wait for a log line containing `marker`; returns the whole log."""
    sim.wait_for_log(re.escape(marker), timeout=timeout)
    return "\n".join(_texts(sim))


def _wait_for_count(sim, marker, count, timeout=20.0):
    """Wait until `marker` has appeared in `count` log lines."""
    deadline = time.monotonic() + timeout
    since = 0
    while True:
        joined = "\n".join(_texts(sim))
        if joined.count(marker) >= count:
            return joined
        if time.monotonic() >= deadline:
            pytest.fail(f"marker {marker!r} seen fewer than {count} times "
                        f"within {timeout}s:\n" + "\n".join(_texts(sim)[-30:]))
        # Block on the next log line rather than polling on a timer.
        since = sim.get_log_buffer(tail=1)["next_seq"]
        try:
            sim.wait_for_log(".*", timeout=max(0.1, deadline - time.monotonic()),
                             since_seq=since)
        except TimeoutError:
            pass


def _inject(sim, button, action):
    """press/release `button` and wait until the OS input layer has read it.
    Explicit press/release (not click) never depends on the sim clock, which
    the tests keep paused: a click's 80 ms auto-release would never come."""
    r = sim.call("inject_button", {"button": button, "action": action})
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)


def _tap(sim, key):
    """One press edge: panels reads the press in one frame and the release
    in a later one, so the press has been handled when this returns."""
    _inject(sim, key, "press")
    _inject(sim, key, "release")


def _frame_barrier(sim):
    """Return once a whole panels frame has run after this call: the press
    of an unused key is read by frame N (which then updates and flushes),
    its release by frame N+1."""
    _tap(sim, "tab")


def _advance(sim, ms):
    """Move the paused clock by `ms`, then let a frame see the new time."""
    sim.step_time(ms)
    _frame_barrier(sim)


def _paused_sim(sim_factory, test_sd_card):
    return sim_factory(test_sd_card, virtual_time=True)


def _stage_panels_lib(test_sd_card):
    src = REPO_ROOT / "system" / "lib" / "panels.lua"
    assert src.exists(), "system/lib/panels.lua missing from the repo"
    dest = test_sd_card / "system" / "lib"
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, dest / "panels.lua")


def test_panels_full_lifecycle(sim_factory, test_sd_card):
    _stage_panels_lib(test_sd_card)
    simulator = _paused_sim(sim_factory, test_sd_card)

    simulator.launch_app("panels_test")
    joined = _wait_for(simulator, "PANELS:SEQ 1")
    assert "PT NOLIB" not in joined
    assert "PT START" in joined
    simulator.set_time_multiplier(0)

    # Custom renderFunction ran with the panel clip active.
    _wait_for(simulator, "PT RENDERFN")
    _advance(simulator, TRANSITION_MS)   # any opening fade has finished

    # Advance mode: forward key steps panels, then crosses into sequence 2.
    _tap(simulator, "down")              # panel 1 -> panel 2 (snap starts)
    _advance(simulator, ADVANCE_MS)      # snap done; input accepted again
    _tap(simulator, "down")              # past last panel -> fade out
    simulator.step_time(TRANSITION_MS)   # SEQ 2 logs at the fade midpoint
    _wait_for(simulator, "PANELS:SEQ 2")
    _advance(simulator, TRANSITION_MS)   # fade-in done; the choice arms
    _frame_barrier(simulator)            # on the first frame after it

    # Choice UI: select the SECOND option, confirm, land in sequence 3 with
    # the story var set by that choice.
    _tap(simulator, "down")              # highlight "second"
    _tap(simulator, "enter")
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:CHOICE 2 -> seq 3" in joined
    simulator.step_time(TRANSITION_MS)
    _wait_for(simulator, "PT PAYOFF picked=b")

    # ESC saves progress and exits cleanly back to the launcher.
    _tap(simulator, "esc")
    joined = _wait_for(simulator, "PT EXIT quit")
    assert "PANELS:EXIT quit" in joined
    assert simulator.wait_for_exit(timeout=10.0)["result"] == "returned"

    # Relaunch: resume=true restores the saved sequence, not sequence 1.
    simulator.clear_log()
    simulator.launch_app("panels_test")
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:SEQ 1" not in joined, "resume went back to the start"

    _tap(simulator, "esc")
    _wait_for(simulator, "PT EXIT quit")


def _scroll(sim, frames):
    """Hold the forward key for `frames` frames of SCROLL_STEP_MS each: the
    paused clock gives exactly one frame per step a non-zero dt, so each
    step scrolls exactly SCROLL_STEP_MS * 180 px/s = 18 px."""
    _inject(sim, "down", "press")
    for _ in range(frames):
        _advance(sim, SCROLL_STEP_MS)
    _inject(sim, "down", "release")
    _frame_barrier(sim)


def _wait_screen(sim, pred, what, timeout=5.0):
    """Poll screenshots until pred(png) holds (a condition, not a delay)."""
    deadline = time.monotonic() + timeout
    while True:
        shot = sim.screenshot()
        if pred(shot):
            return shot
        if time.monotonic() >= deadline:
            pytest.fail(what)
        time.sleep(0.02)


def test_panels_hw_scroll(sim_factory, test_sd_card):
    """Rigid vertical scroll sequences must engage the LCD hardware-scroll
    ring (PANELS:HW on), stay pixel-stable when idle, survive a system-menu
    takeover (foreign register write -> resync -> re-enter), and release the
    ring cleanly (PANELS:HW off) when the end-of-sequence transition fires."""
    _stage_panels_lib(test_sd_card)
    simulator = _paused_sim(sim_factory, test_sd_card)
    simulator.clear_log()
    simulator.launch_app("panels_hw_test")
    joined = _wait_for(simulator, "PANELS:HW on")
    assert "PHW NOLIB" not in joined
    simulator.set_time_multiplier(0)
    _frame_barrier(simulator)

    top = simulator.screenshot()

    # Scroll in: the composed screen must change.
    _scroll(simulator, 4)                # 72 px
    mid = simulator.screenshot()
    assert mid != top, "hardware scroll did not move the screen"

    # Idle ring: no input, no redraws — frames must be pixel-identical,
    # however much time passes.
    _advance(simulator, 1000)
    assert simulator.screenshot() == mid, "ring not static without input"

    # System menu takeover: the OS resets the scroll register.  panels must
    # detect the foreign write via the write counter, repaint, and re-enter
    # the fast path (second "PANELS:HW on"), restoring the exact frame.
    r = simulator.keypress("menu")
    simulator.wait_input_consumed(r["input_seq"], timeout=5.0)
    _wait_screen(simulator, lambda s: s != mid, "system menu did not appear")
    _tap(simulator, "esc")               # read by the menu: closes it
    _wait_for_count(simulator, "PANELS:HW on", 2)
    _frame_barrier(simulator)
    assert simulator.screenshot() == mid, (
        "frame not restored after system-menu takeover"
    )

    # Drive past maxScroll (446px): the clamped edge press starts the cut
    # transition (PANELS:HW off), which loops back into sequence 1 and
    # re-enters the fast path (third "PANELS:HW on").
    _scroll(simulator, 24)               # 72 + 432 px: clamped at 446
    _tap(simulator, "down")              # forward press at the end: advance
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for_count(simulator, "PANELS:SEQ 1", 2)
    assert "PANELS:HW off" in joined, (
        "transition started without releasing the hardware-scroll ring"
    )
    _wait_for_count(simulator, "PANELS:HW on", 3)

    _tap(simulator, "esc")
    _wait_for(simulator, "PHW EXIT quit")
