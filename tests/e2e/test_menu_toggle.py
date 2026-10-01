"""The menu key toggles the system menu (issue #59).

Sym (the keyboard's menu key) and a pad's Home open the menu; pressed again
while it is open they close it, like Esc at the top level, from any page.
The press is consumed by the menu, so nothing latched survives to open it
again once it is closed (the bug: a press made while the menu was open did
nothing at the time and re-opened the menu as soon as Esc closed it).

The menu is found by pixels: its panel covers the middle of the screen, so
the middle differs from the frame taken before it opened and is the same
again once it has closed. That needs no knowledge of what is under it, so the
same probe serves a Lua app, a native app and the launcher.

Runs on the simulator and on the device (@pytest.mark.both) except the native
case, which needs the simulator's staged ELF probe.
"""

from __future__ import annotations

import io
import shutil
import time
from pathlib import Path

import pytest

pytestmark = [pytest.mark.timeout(300),
              pytest.mark.sd(fixtures=[], reserve=2)]
both = pytest.mark.both

APP = "menu_toggle"
APP_ID = "net.picodeck.menu_toggle"
NATIVE_PROBE = Path(__file__).parent / "fixtures" / "native_pad_probe"

# A steady blue frame that ignores every key (the test leaves with exit_app),
# so a menu that is open or came back shows against it and Esc is harmless.
FIXTURE = r"""
local pc = picocalc
local d, sys = pc.display, pc.sys
d.clear(d.BLUE)
d.flush()
sys.log("MT:READY")
while true do
    sys.resetIdleTimer()
    pc.input.update()
    sys.sleep(20)
end
"""

# The menu panel (200 px wide, centred); the header and clock are outside it.
REGION = (60, 60, 260, 260)
OPEN_PIXELS = 2000     # a menu changes thousands of these
CLOSED_PIXELS = 300    # an unchanged screen changes (almost) none
SETTLE = 1.5           # how long "stays closed" is watched

INPUTS = ["menu", "pad home"]


def _region(target):
    from PIL import Image
    img = Image.open(io.BytesIO(target.screenshot())).convert("RGB")
    return img.crop(REGION)


def _changed(a, b) -> int:
    from PIL import ImageChops
    diff = ImageChops.difference(a, b).convert("L").point(
        lambda v: 255 if v > 24 else 0)
    return sum(1 for v in diff.tobytes() if v)


class Screen:
    """The frame before the menu opened, and what the middle does against it."""

    def __init__(self, target):
        self.target = target
        time.sleep(0.3)
        self.base = _region(target)

    def changed(self) -> int:
        return _changed(self.base, _region(self.target))

    def wait(self, want_open: bool, what: str, timeout: float = 10.0):
        deadline = time.monotonic() + timeout
        while True:
            n = self.changed()
            if (n >= OPEN_PIXELS) if want_open else (n <= CLOSED_PIXELS):
                return
            if time.monotonic() >= deadline:
                pytest.fail(f"{what} ({n} pixels differ from the frame "
                            "before the menu)")
            time.sleep(0.05)

    def stays_closed(self, what: str, seconds: float = SETTLE):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            n = self.changed()
            assert n <= CLOSED_PIXELS, f"{what} ({n} pixels differ)"
            time.sleep(0.05)


def _press(target, how: str):
    if how == "menu":
        target.keypress("menu")
    else:
        target.pad("home", hold_ms=100)


@pytest.fixture
def pad_cleanup(target):
    yield
    try:
        target.pad("off")  # never leave a test pad connected on a device
    except Exception:
        pass


def _start_lua(target) -> Screen:
    target.stage_lua_app(APP, FIXTURE, id=APP_ID)
    assert target.launch_app(APP)["launched"]
    time.sleep(1.0)
    return Screen(target)


def _finish_lua(target):
    target.exit_app()
    target.wait_for_exit(timeout=15)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_closes_the_menu_in_an_app(target, how, pad_cleanup):
    screen = _start_lua(target)
    _press(target, how)
    screen.wait(True, "the menu did not open")
    _press(target, how)
    screen.wait(False, "the menu key did not close the open menu")
    screen.stays_closed("the menu re-opened after the menu key closed it")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_then_esc_leaves_the_menu_closed(target, how, pad_cleanup):
    """The issue's sequence: the second menu key closes the menu, the Esc
    after it goes to the app, and nothing latched opens the menu again."""
    screen = _start_lua(target)
    _press(target, how)
    screen.wait(True, "the menu did not open")
    _press(target, how)
    target.keypress("esc")
    screen.wait(False, "the menu is still open")
    screen.stays_closed("the menu re-opened after menu key, then Esc")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_closes_the_whole_menu_from_settings(target, how, pad_cleanup):
    """On a sub-page the menu key closes the menu, not just the page."""
    screen = _start_lua(target)
    _press(target, how)
    screen.wait(True, "the menu did not open")
    target.keypress("down")   # Battery -> Settings
    target.keypress("enter")  # into the Settings page
    time.sleep(0.4)
    assert screen.changed() >= OPEN_PIXELS, "the Settings page did not open"
    _press(target, how)
    screen.wait(False, "the menu key did not close the menu from Settings")
    screen.stays_closed("the menu re-opened after closing from Settings")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_toggles_the_launcher_menu(target, how, pad_cleanup):
    assert target.status()["app"] == "launcher"
    screen = Screen(target)
    _press(target, how)
    screen.wait(True, "the launcher's menu did not open")
    _press(target, how)
    screen.wait(False, "the menu key did not close the launcher's menu")
    screen.stays_closed("the launcher's menu re-opened")
    _press(target, how)  # and it still opens afterwards
    screen.wait(True, "the launcher's menu did not open a second time")
    target.keypress("esc")
    screen.wait(False, "Esc did not close the launcher's menu")
    screen.stays_closed("the launcher's menu re-opened after Esc")


@pytest.mark.native
def test_menu_key_closes_the_menu_over_a_native_app(simulator):
    # Sym only: the simulator's native trampolines do not pump the `pad` dev
    # command (the Lua runner and the menu loop do), so Home cannot be sent
    # to a native app here. Its path into the menu is the same latch.
    how = "menu"
    sim = simulator
    shutil.copytree(NATIVE_PROBE,
                    Path(sim.sd_card_path) / "apps" / "native_pad_probe",
                    dirs_exist_ok=True)
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("native_pad_probe")
    sim.wait_for_log(r"PADREADY", timeout=10, since_seq=seq)
    screen = Screen(sim)
    _press(sim, how)
    screen.wait(True, "the menu did not open over the native app")
    _press(sim, how)
    screen.wait(False, "the menu key did not close the menu over the native app")
    screen.stays_closed("the menu re-opened over the native app")
    sim.exit_app()
    sim.wait_for_exit(timeout=10)
