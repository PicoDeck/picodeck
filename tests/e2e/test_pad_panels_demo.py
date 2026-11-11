"""apps/panels_demo read from the gamepad. The comic takes the pad through
system/lib/panels.lua; main.lua's own screen for a card without the library
(the firmware writes it from v0.7.0, but a full or failing card can lack it)
leaves on B too. (The simulator never writes the card's system files, so a
test can delete the library.)"""
from pathlib import Path

import pytest

from pad_apps import EXITED, finish, finish_key, launch, mark, press

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)


def test_comic_quits_on_b(simulator):
    sim = simulator
    seq = mark(sim)
    launch(sim, "panels_demo", settle=0)
    sim.wait_for_log(r"^PANELS:SEQ ", timeout=15, since_seq=seq)
    press(sim, "b")
    sim.wait_for_log(r"^PANELS_DEMO: exited with quit", timeout=10,
                     since_seq=seq)
    assert sim.wait_for_exit(timeout=10).get("result") in EXITED


def test_missing_library_screen_leaves_on_b(simulator):
    sim = simulator
    (Path(sim.sd_card_path) / "system" / "lib" / "panels.lua").unlink()
    launch(sim, "panels_demo")
    finish(sim)


def test_missing_library_screen_leaves_on_esc(simulator):
    sim = simulator
    (Path(sim.sd_card_path) / "system" / "lib" / "panels.lua").unlink()
    launch(sim, "panels_demo")
    finish_key(sim, "esc")
