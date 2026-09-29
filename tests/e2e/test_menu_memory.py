"""The system menu's memory on a nearly full heap (src/os/system_menu.c).

While the menu is open it holds its own darkened backdrop (200 KB, restored
under each page and sub-dialog) and, when it fits, a copy of the app's two
framebuffers (400 KB) to give back on close. The copy is the extra: it is
taken only after the backdrop, and given up again if it cost the Controls
page its 8 KB block (CTL_HEAP_MIN), so on any heap the menu does at least
what it did before the copy existed.

The fixture paints both framebuffers blue, then holds the whole PSRAM heap
except one free block of LEAVE bytes (the simulator runs the device's own umm,
--real-umm, so the largest free block is real). Each case walks the menu:
Settings and back (a lost backdrop leaves the Settings panel's title bar
behind), Settings -> Controls (the page opens or shows its out-of-memory
notice), then closes the menu and reads what the app got back.
"""

from __future__ import annotations

import io
import time

import pytest

from helpers import stage_lua_app

APP = "menu_mem"

FIXTURE = r"""
local pc = picocalc
local d, input, sys = pc.display, pc.input, pc.sys
local LEAVE = {leave}
d.clear(d.BLUE)
d.flush()
d.clear(d.BLUE)
d.flush()
collectgarbage("collect")
-- Hold every free block except one of LEAVE bytes: carve the hole first,
-- take everything else, then free the hole. The loop below never allocates.
local hold = {{}}
for i = 1, 64 do hold[i] = false end
local hole = sys.qmiPsramAlloc(LEAVE)
local n = 0
while n < 64 do
    local l = sys.getMemInfo().psram_largest_block
    if l < 1024 then break end
    local h = sys.qmiPsramAlloc(l - 16) or sys.qmiPsramAlloc(l - 512)
    if not h then break end
    n = n + 1
    hold[n] = h
end
sys.qmiPsramFree(hole)
hole = nil
sys.log("MM:READY largest=" .. sys.getMemInfo().psram_largest_block)
while true do
    input.update()
    if (input.getButtonsPressed() & input.BTN_ESC) ~= 0 then return end
    sys.sleep(20)
end
"""

KB = 1024
# LEAVE (bytes), then what the menu can do in it. The backdrop is 200 KB, the
# copy 400 KB and the Controls page wants an 8 KB block: 606 KB fits the
# backdrop and the copy but not the Controls block after them. umm hands out
# 200-byte blocks, a header included: the backdrop takes 1,025 (205,000 B)
# and the copy 2,049 (409,800 B), so 622,700 B (a 622,800 B block) is the
# boundary where both fit and leave 8,000 B, short of the 8,192 Controls
# wants.
CASES = [
    # leave, backdrop, restored, controls
    (4 * KB, False, False, False),
    (300 * KB, True, False, True),
    (500 * KB, True, False, True),
    (606 * KB, True, False, True),
    (622700, True, False, True),
    (700 * KB, True, True, True),
]

BLUE = (0, 0, 255)
DARK_BLUE = (0, 0, 123)  # display_darken halves each channel
TOLERANCE = 40
OUTSIDE = (20, 40)       # left of every panel
CENTRE = (160, 160)      # under every panel
# In a Lua app the main page has 5 items (y 111-207) and Settings 9 (y
# 85-233): (70, 95) is under the Settings title bar only.
ABOVE_MAIN = (70, 95)
CONTROLS_EDGE = (30, 160)  # inside the Controls panel (x 28-291) only
TO_SETTINGS = ["down", "enter"]
TO_CONTROLS = ["down", "down", "down", "enter"]  # from the top of Settings
OOM = r"^\[CONTROLS\] Not enough memory for Controls$"


def _px(sim, xy):
    from PIL import Image
    img = Image.open(io.BytesIO(sim.screenshot())).convert("RGB")
    return tuple(img.getpixel(xy))


def _near(rgb, want) -> bool:
    return all(abs(a - b) <= TOLERANCE for a, b in zip(rgb, want))


def _wait_px(sim, xy, pred, what, timeout=10.0):
    """Poll screenshots until pred(pixel at xy) holds."""
    deadline = time.monotonic() + timeout
    while True:
        px = _px(sim, xy)
        if pred(px):
            return px
        if time.monotonic() >= deadline:
            pytest.fail(f"{what}: pixel {xy} is {px}")
        time.sleep(0.05)


def _keys(sim, keys, delay=0.2):
    for k in keys:
        sim.keypress(k)
        time.sleep(delay)


def _mark(sim):
    return sim.get_log_buffer(tail=1).get("next_seq", 0)


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


@pytest.mark.sd(fixtures=[], reserve=1)
@pytest.mark.parametrize("leave,backdrop,restored,controls", CASES,
                         ids=[f"{c[0] // KB}KB" if c[0] % KB == 0 else f"{c[0]}B"
                              for c in CASES])
def test_menu_on_a_nearly_full_heap(sim_factory, test_sd_card, leave,
                                    backdrop, restored, controls):
    stage_lua_app(test_sd_card, APP, FIXTURE.format(leave=leave))
    sim = sim_factory(test_sd_card, extra_args=["--real-umm"])
    sim.launch_app(APP)
    line = sim.wait_for_log(r"^MM:READY", timeout=20)
    largest = int(line.split("=")[1])
    assert leave <= largest < leave + 8 * KB, line
    _wait_px(sim, OUTSIDE, lambda p: _near(p, BLUE), "the fixture never drew")

    sim.keypress("menu")
    _wait_px(sim, OUTSIDE, lambda p: _near(p, DARK_BLUE),
             "the menu did not open over the darkened frame")
    main_above = _px(sim, ABOVE_MAIN)
    assert _near(main_above, DARK_BLUE), main_above

    # Settings and back: the main page is drawn over the menu's backdrop.
    _keys(sim, TO_SETTINGS)
    settings_title = _wait_px(sim, ABOVE_MAIN,
                              lambda p: not _near(p, DARK_BLUE),
                              "the Settings page did not open")
    _keys(sim, ["esc"])
    if backdrop:
        _wait_px(sim, ABOVE_MAIN, lambda p: p == main_above,
                 f"with {leave} B free the Settings panel stayed behind "
                 f"the main page (its title bar is {settings_title}): the "
                 "menu lost its own backdrop")
    else:
        time.sleep(0.5)  # no backdrop (too little memory for it): as before

    # Settings -> Controls: the page opens, or says it has no memory.
    _keys(sim, TO_SETTINGS)
    _wait_px(sim, ABOVE_MAIN, lambda p: not _near(p, DARK_BLUE),
             "the Settings page did not open again")
    mark = _mark(sim)
    _keys(sim, TO_CONTROLS)
    if controls:
        _wait_px(sim, CONTROLS_EDGE, lambda p: not _near(p, DARK_BLUE),
                 f"with {leave} B free the Controls page did not open")
        assert not any("Not enough memory" in t for t in _texts(sim, mark)), (
            _texts(sim, mark))
        _keys(sim, ["esc"])  # nothing changed: nothing to save
    else:
        sim.wait_for_log(OOM, timeout=10, since_seq=mark)
        time.sleep(1.7)  # the notice stays up for 1.5 s
    _keys(sim, ["esc", "esc"])  # Settings -> main -> closed

    if restored:
        for xy in (OUTSIDE, CENTRE):
            _wait_px(sim, xy, lambda p: _near(p, BLUE),
                     f"with {leave} B free the app's frame was not "
                     "given back")
    elif backdrop:
        # Nothing was saved: the menu closes over the darkened screen and
        # the app redraws when it next draws (this fixture never does).
        time.sleep(0.5)
        px = _px(sim, OUTSIDE)
        assert _near(px, DARK_BLUE), px
    # Without the backdrop either, the simulator's menu frames alternate
    # with the app's own undarkened one (its display_darken writes only the
    # back buffer; the firmware's writes both): nothing to read there.

    sim.keypress("esc")
    out = sim.wait_for_exit(timeout=15)
    assert out["result"] == "returned", out
