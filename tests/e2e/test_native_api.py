"""What the native API does, where the firmware or the simulator got it wrong.

Runs modes of tests/e2e/native/fidelity_probe.c (test_native_fidelity.py).
"""
from __future__ import annotations

import struct
import wave
from pathlib import Path

import pytest

from test_native_fidelity import _run

pytestmark = [pytest.mark.native, pytest.mark.sd(fixtures=[], reserve=4)]


def _beep(path: Path, ms=50):
    rate = 22050
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<h", 1000) * (rate * ms // 1000))


@pytest.mark.parametrize("mode", ["loop_before_play", "loop_off_while_looping"])
def test_player_set_loop(simulator, mode):
    d = Path(simulator.sd_card_path) / "apps" / mode
    d.mkdir(parents=True, exist_ok=True)
    _beep(d / "beep.wav")
    _, log = _run(simulator, mode)
    assert "LOOP looping 1" in log, log
    assert "LOOP stopped 1" in log, log
