"""A held menu key opens the menu once (simulator, host key auto-repeat).

A desktop holding F10 sends SDL KEYDOWN events with `repeat` set. The
STM32 repeats a held F-key, Esc and the modifiers as HOLD, which makes no new
press edge, so a held menu key must not re-trigger the menu: before this was
fixed the simulator (and the browser demo built from it) opened and closed
the menu on every repeat once the menu key toggled it (issue #59).

The RPC cannot inject a repeat, so an LD_PRELOAD shim over SDL_PollEvent
(fixtures/sdl_key_hold_shim.c) plays the host's key for this test. It is
skipped when there is no C compiler, pkg-config or SDL2 header (CI has all
three; a sanitizer build links its runtime statically, so it runs there too).
"""

import io
import shutil
import subprocess
import time
from pathlib import Path

import pytest

SHIM = Path(__file__).parent / "fixtures" / "sdl_key_hold_shim.c"
REGION = (60, 60, 260, 260)

pytestmark = [pytest.mark.timeout(120), pytest.mark.sd(fixtures=[], reserve=2)]


@pytest.fixture
def hold_shim(tmp_path, monkeypatch, request):
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not shutil.which("pkg-config"):
        pytest.skip("needs a C compiler and pkg-config")
    flags = subprocess.run(["pkg-config", "--cflags", "sdl2"],
                           capture_output=True, text=True)
    if flags.returncode != 0:
        pytest.skip("needs the SDL2 headers")
    so = tmp_path / "sdl_key_hold_shim.so"
    r = subprocess.run([cc, "-shared", "-fPIC", "-o", str(so), str(SHIM),
                        *flags.stdout.split(), "-ldl"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip(f"the shim did not compile: {r.stderr[-300:]}")
    trigger = tmp_path / "hold_now"
    monkeypatch.setenv("LD_PRELOAD", str(so))
    monkeypatch.setenv("KEYHOLD_TRIGGER", str(trigger))
    monkeypatch.setenv("KEYHOLD_MS", "2000")
    return trigger


def _region(sim):
    from PIL import Image
    return Image.open(io.BytesIO(sim.screenshot())).convert("RGB").crop(REGION)


def _open(base, img) -> bool:
    from PIL import ImageChops
    d = ImageChops.difference(base, img).convert("L").point(
        lambda v: 255 if v > 24 else 0)
    return sum(1 for v in d.tobytes() if v) >= 2000


def test_held_menu_key_opens_the_menu_once(hold_shim, simulator):
    sim = simulator
    time.sleep(1.0)
    base = _region(sim)
    assert not _open(base, _region(sim))
    states = [False]  # the closed baseline, sampled before the key goes down
    hold_shim.touch()
    end = time.monotonic() + 3.0   # the key is down for 2 s
    while time.monotonic() < end:
        states.append(_open(base, _region(sim)))
        time.sleep(0.02)
    transitions = sum(1 for a, b in zip(states, states[1:]) if a != b)
    assert any(states), "the held menu key never opened the menu"
    assert transitions == 1, (
        f"{transitions} open/close transitions during one held key press: "
        + "".join("O" if s else "." for s in states))
    assert states[-1], "the menu closed by itself while the key was held"
