"""Apps waiting for a key must not redraw and flush the screen flat out.

A loop with no wait keeps Core 0 and the panel's bus busy on a battery device
while nothing on the screen changes. These apps present only for input and for
what changes by itself (a clock, a timer, the cursor's blink), and a key still
reaches the screen.
"""

import time

import pytest


def presents_while_idle(sim, seconds=2.0):
    start = sim.present_count()
    time.sleep(seconds)
    return sim.present_count() - start


def lit_changed_by(sim, key):
    before = sim.screenshot_pil().convert("RGB").tobytes()
    seq = sim.keypress(key)["input_seq"]
    sim.wait_input_consumed(seq)
    time.sleep(0.3)
    return sim.screenshot_pil().convert("RGB").tobytes() != before


@pytest.mark.sd(fixtures=[], extra=[("apps/calculator", "apps/calculator")])
def test_calculator_sleeps_until_a_key(simulator):
    simulator.launch_app("calculator", hidden=False)
    time.sleep(0.6)
    # Once a second, for the status bar's clock and battery.
    assert presents_while_idle(simulator) <= 4
    assert lit_changed_by(simulator, "7")
    simulator.exit_app()
    simulator.wait_for_exit(timeout=10)


@pytest.mark.sd(fixtures=[], extra=[("apps/minesweeper", "apps/minesweeper")])
def test_minesweeper_sleeps_until_a_key(simulator):
    simulator.launch_app("minesweeper", hidden=False)
    time.sleep(0.6)
    # Once a second, for the header's timer.
    assert presents_while_idle(simulator) <= 4
    assert lit_changed_by(simulator, "right")
    simulator.exit_app()
    simulator.wait_for_exit(timeout=10)


@pytest.mark.sd(fixtures=[], extra=[("apps/editor", "apps/editor")])
def test_editor_sleeps_until_a_key(simulator):
    simulator.launch_app("editor", hidden=False)
    time.sleep(0.6)
    simulator.keypress("esc")  # the file browser it starts in
    time.sleep(0.3)
    # Twice a second, for the cursor's blink.
    assert presents_while_idle(simulator) <= 8
    assert lit_changed_by(simulator, "x")
    simulator.exit_app()
    simulator.wait_for_exit(timeout=10)


@pytest.mark.sd(fixtures=[], extra=[("apps/editor", "apps/editor")])
def test_editor_cursor_still_blinks_while_idle(simulator):
    simulator.launch_app("editor", hidden=False)
    time.sleep(0.6)
    simulator.keypress("esc")
    time.sleep(0.3)
    frames = {simulator.screenshot_pil().convert("RGB").tobytes()}
    for _ in range(12):
        time.sleep(0.15)
        frames.add(simulator.screenshot_pil().convert("RGB").tobytes())
    assert len(frames) >= 2
    simulator.exit_app()
    simulator.wait_for_exit(timeout=10)
