"""The menu key toggles the system menu (issue #59).

The menu key (Shift+F5 / F10 on the keyboard) and a pad's Home open the
menu; pressed again while it is open they close it, like Esc at the top
level, from any page.
The press is consumed by the menu, so nothing latched survives to open it
again once it is closed (the bug: a press made while the menu was open did
nothing at the time and re-opened the menu as soon as Esc closed it).

The menu is found by pixels: its panel covers the middle of the screen, so
the middle differs from the frame taken before it opened and is the same
again once it has closed. That needs no knowledge of what is under it, so the
same probe serves a Lua app, a native app and the launcher.

Runs on the simulator and on the device (@pytest.mark.both) except the native
case (the simulator's staged ELF probe) and the Controls case (it reads the
bindings file from the simulator's SD card).
"""

from __future__ import annotations

import io
import json
import shutil
import time
from pathlib import Path

import pytest

from test_controls import TO_A, TO_CONTROLS, _keys

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

# A confirm dialog the test raises with F3 (so no clock decides when it shows):
# the app turns red when it was answered No, green on Yes.
CONFIRM_FIXTURE = r"""
local pc = picocalc
local d, sys, input = pc.display, pc.sys, pc.input
d.clear(d.BLUE)
d.flush()
while true do
    sys.resetIdleTimer()
    input.update()
    if (input.getButtonsPressed() & input.BTN_F3) ~= 0 then break end
    sys.sleep(20)
end
local yes = pc.ui.confirm("Sure?")
d.clear(yes and d.GREEN or d.RED)
d.flush()
while true do
    sys.resetIdleTimer()
    input.update()
    sys.sleep(20)
end
"""

BLUE = (0, 0, 255)
RED = (255, 0, 0)

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


def _wait_colour(target, rgb, what: str, timeout: float = 15.0):
    """Wait for the screen's corner (outside any panel) to show `rgb`."""
    from PIL import Image
    deadline = time.monotonic() + timeout
    while True:
        img = Image.open(io.BytesIO(target.screenshot())).convert("RGB")
        px = img.getpixel((20, 40))
        if all(abs(a - b) <= 40 for a, b in zip(px, rgb)):
            return
        if time.monotonic() >= deadline:
            pytest.fail(f"{what}: corner pixel {px}")
        time.sleep(0.1)


def _wake(target):
    """An injected menu key only sets a latch and is no activity, so a device
    idle past its 60 s dim needs a real key first or the next one only wakes
    the screen (test_menu_restore.py does the same). F3 is ignored by the
    launcher and the menu."""
    target.keypress("f3")
    time.sleep(0.3)


def _dev_mode(target) -> bool:
    try:
        cfg = json.loads(target.read_file("/system/config.json").decode())
        return str(cfg.get("dev_mode", "0")) == "1"
    except Exception:
        return False


def _bt_available(target) -> bool:
    """Whether Settings has its Bluetooth row (between Controls and Time
    zone): the firmware's `bt` dev command says (older builds lack it)."""
    try:
        return any("available=1" in ln for ln in target.command("bt"))
    except Exception:
        return False


# Settings rows: Brightness, Battery %, Show FPS, Controls, [Bluetooth,]
# Time zone, ... (the Bluetooth row only where it is available).
def _downs_to_time_zone(target) -> int:
    return 5 if _bt_available(target) else 4


def _into_settings(target):
    """Open the Settings page from the main menu's first row. The rows run
    Battery, [RAM (dev mode),] Settings, so the count of Downs depends on
    dev mode (read from the config). Returns the Settings frame, which must
    differ a lot from the main page's frame with the same row selected: the
    Settings page is a taller panel, a moved cursor is not."""
    for _ in range(2 if _dev_mode(target) else 1):
        target.keypress("down")
        time.sleep(0.2)
    selected = _region(target)
    target.keypress("enter")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        time.sleep(0.1)
        if _changed(selected, _region(target)) > 6000:
            time.sleep(0.3)
            return _region(target)
    pytest.fail("never reached the Settings page")


def _press(target, how: str):
    if how == "menu":
        target.keypress("menu")
    else:
        target.pad("home", hold_ms=100)
        # Held for 100 ms from the first poll that sees it: wait it out, or a
        # second Home set under load finds it still down (no new edge).
        time.sleep(0.4)


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
    _wait_colour(target, BLUE, "the fixture's blue frame never showed")
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
    _into_settings(target)
    _press(target, how)
    screen.wait(False, "the menu key did not close the menu from Settings")
    screen.stays_closed("the menu re-opened after closing from Settings")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_cancels_a_modal_the_menu_opened(target, how, pad_cleanup):
    """The time-zone picker (Settings, after Bluetooth where it is) is a
    modal of the menu's own: the menu key cancels it like Esc, and takes the
    press, so it neither waits to act after the picker nor opens a second
    menu."""
    screen = _start_lua(target)
    downs = _downs_to_time_zone(target)  # asked before the menu opens
    _press(target, how)
    screen.wait(True, "the menu did not open")
    settings = _into_settings(target)
    for _ in range(downs):
        target.keypress("down")
        time.sleep(0.2)
    selected = _region(target)  # the Time zone row selected, before Enter
    target.keypress("enter")
    time.sleep(0.5)
    picker = _region(target)
    assert _changed(selected, picker) > OPEN_PIXELS, \
        "the time-zone picker did not open"
    _press(target, how)
    time.sleep(0.5)
    after = _region(target)
    assert _changed(picker, after) > OPEN_PIXELS, \
        "the menu key did not cancel the picker"
    # Back on the Settings page: its title bar
    band = (0, 20, 200, 38)
    assert _changed(settings.crop(band), after.crop(band)) <= CLOSED_PIXELS, \
        "the picker did not return to the Settings page"
    _press(target, how)  # now it closes the menu
    screen.wait(False, "the menu key did not close the menu after the picker")
    screen.stays_closed("the menu re-opened after the picker")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_closes_the_whole_menu_from_bluetooth(target, how,
                                                       pad_cleanup):
    """The Bluetooth page (Settings, after Controls) is left by the menu key
    like Controls: the whole menu closes and stays closed."""
    if not _bt_available(target):
        pytest.skip("no Bluetooth on this target")
    screen = _start_lua(target)
    _press(target, how)
    screen.wait(True, "the menu did not open")
    _into_settings(target)
    for _ in range(4):  # Brightness, Battery %, Show FPS, Controls, Bluetooth
        target.keypress("down")
        time.sleep(0.2)
    selected = _region(target)
    target.keypress("enter")
    time.sleep(0.5)
    assert _changed(selected, _region(target)) > OPEN_PIXELS, \
        "the Bluetooth page did not open"
    _press(target, how)
    screen.wait(False, "the menu key did not close the menu from Bluetooth")
    screen.stays_closed("the menu re-opened after leaving Bluetooth")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_is_ignored_by_a_confirm_and_opens_no_menu(
        target, how, pad_cleanup):
    """With an app's ui.confirm up the menu key does nothing: the dialog
    stays (No can mean "discard" to an app, so only Esc may answer it) and
    the system menu does not open, then or after the answer."""
    target.stage_lua_app(APP, CONFIRM_FIXTURE, id=APP_ID)
    assert target.launch_app(APP)["launched"]
    _wait_colour(target, BLUE, "the fixture's blue frame never showed")
    screen = Screen(target)
    target.keypress("f3")
    screen.wait(True, "the confirm dialog did not show", timeout=15.0)
    time.sleep(0.6)  # past the dialog's grace period
    dialog = _region(target)
    _press(target, how)
    time.sleep(1.0)
    assert _changed(dialog, _region(target)) <= CLOSED_PIXELS, \
        "the menu key changed the screen over the confirm dialog"
    target.keypress("esc")  # No
    _wait_colour(target, RED, "Esc did not answer the confirm")
    Screen(target).stays_closed("the system menu opened after the confirm")
    _finish_lua(target)


@both
@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_toggles_the_launcher_menu(target, how, pad_cleanup):
    deadline = time.monotonic() + 15
    while target.status()["app"] != "launcher":
        assert time.monotonic() < deadline, "not at the launcher"
        time.sleep(0.2)
    _wake(target)
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


@pytest.mark.parametrize("how", INPUTS)
def test_menu_key_leaves_controls_saves_and_closes_the_menu(simulator, how):
    """Outside a key capture the menu key leaves Controls like Esc (the
    bindings are saved) and closes the whole menu; a gamepad-only player has
    no other way out of the page. Launcher: the global map is edited."""
    sim = simulator
    screen = Screen(sim)
    _press(sim, how)
    screen.wait(True, "the menu did not open")
    time.sleep(0.3)
    mark = sim.get_log_buffer(tail=1).get("next_seq", 0)
    # test_controls' own route: Settings, Controls, then down to A (bound
    # to Z by the capture that follows)
    _keys(sim, TO_CONTROLS)
    _keys(sim, TO_A + ["enter", "z"])
    _press(sim, how)
    sim.wait_for_log(r"^\[CONTROLS\] bindings saved", timeout=10, since_seq=mark)
    screen.wait(False, "the menu key did not close the menu from Controls")
    screen.stays_closed("the menu re-opened after leaving Controls")
    saved = json.loads((Path(sim.sd_card_path) / "system" /
                        "gamepad.json").read_text())
    assert saved == {"a": ["Z"]}, saved
