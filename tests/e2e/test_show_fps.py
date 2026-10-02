"""The OS FPS counter (system menu -> Settings -> Show FPS; src/os/os_overlay.c).

The OS counts presents (or perf.endFrame ticks) and draws "FPS: n" in an
opaque black 52x12 box at the corner chosen by the `show_fps` key of
/system/config.json, over every present: flush, flushRows and flushRegion.
For a partial present whose rows miss the box, the OS pushes the box as a
small window write when its text changes, so the probes below read the
PRESENTED screen (the simulator's GRAM analog), not a framebuffer. (A
firmware screenshot reads the framebuffer, so it has no such box.)

The fixtures paint the whole screen blue, so black pixels can only come from
the counter box, and its text is one of the drawFPS colours (green, yellow,
red; grey for "--" before the first 1 s window completes).

While the counter is on, an app's own perf.drawFPS (Lua or native) draws
nothing, so the screen shows one counter (issue #66).
"""

import json
import shutil
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import app_rel_dir, stage_lua_app

BOX_W, BOX_H = 52, 12
CORNERS = {"tr": (262, 22), "tl": (6, 22), "br": (262, 302), "bl": (6, 302)}
BG = (0, 0, 255)                     # 0x001F, the fixtures' background
NUMBER_INKS = {(0, 255, 0), (255, 255, 0), (255, 0, 0)}
# COLOR_GRAY, "FPS: --", as the simulator's screenshot widens RGB565
# (x * 255 / 31, x * 255 / 63, truncating: sim_socket_handler.c).
DASH_INK = (131, 129, 131)
TOAST_BG = (41, 40, 41)              # TOAST_COLOR_INFO, RGB565(40, 40, 40)

# Toast geometry (ui_widget_toast at y 280): "TOAST" is 30px wide + 2x8
# padding, centred.
TOAST_Y, TOAST_H = 280, 14
TOAST_W = 5 * 6 + 16
TOAST_X = (320 - TOAST_W) // 2

FIXTURE = """
local d = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local MODE, Y0, Y1 = "{mode}", {y0}, {y1}
{setup}
d.clear(0x001F)
if MODE == "flush" then d.flush()
elseif MODE == "rows" then d.flushRows(0, 319)
else d.flushRegion(0, 319) end
local n = 0
while true do
    input.update()
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then return end
    if p & input.BTN_ENTER ~= 0 then picocalc.ui.toast("TOAST") end
    n = n + 1
    local c = (n % 2 == 0) and 0xFFFF or 0x07FF   -- white / cyan band
    if MODE == "flush" then
        d.clear(0x001F)
        d.fillRect(0, 150, 320, 10, c)
        d.flush()
    else
        d.fillRect(0, Y0, 320, Y1 - Y0 + 1, 0x001F)
        if Y0 <= 150 and Y1 >= 159 then d.fillRect(0, 150, 320, 10, c) end
        if MODE == "rows" then d.flushRows(Y0, Y1) else d.flushRegion(Y0, Y1) end
    end
    if n == 3 then sys.log("FPSFIX:READY") end
    sys.sleep(16)
end
"""


def _rgb(img) -> np.ndarray:
    return np.array(img.convert("RGB")).astype(int)


def _box(arr, corner):
    x, y = CORNERS[corner]
    return arr[y:y + BOX_H, x:x + BOX_W]


def _colours(region) -> set:
    return {tuple(p) for p in region.reshape(-1, 3)}


def counter_at(arr, corner, number=True) -> bool:
    """The counter box at `corner`: a black frame around the text, text in
    the colour code (a number) or grey ("--"), and the fixture's blue just
    outside it (the box is exactly BOX_W x BOX_H)."""
    b = _box(arr, corner)
    frame = np.concatenate([b[0], b[-1], b[:, 0], b[:, -1]])
    if frame.any():
        return False
    ink = _colours(b) - {(0, 0, 0)}
    allowed = NUMBER_INKS if number else NUMBER_INKS | {DASH_INK}
    if not ink or not ink <= allowed:
        return False
    x, y = CORNERS[corner]
    return tuple(arr[y - 1, x]) == BG and tuple(arr[y + BOX_H, x + BOX_W]) == BG


def untouched(arr, corner) -> bool:
    return _colours(_box(arr, corner)) == {BG}


def wait_screen(sim, pred, timeout=8.0):
    deadline = time.time() + timeout
    while True:
        arr = _rgb(sim.screenshot_pil())
        if pred(arr) or time.time() >= deadline:
            return arr
        time.sleep(0.05)


def boot(sim_factory, sd, show_fps=None, **kwargs):
    if show_fps is not None:
        (sd / "system" / "config.json").write_text(json.dumps({"show_fps": show_fps}))
    return sim_factory(sd, **kwargs)


def run_fixture(sim, name, mode="flush", y0=0, y1=319, setup=""):
    stage_lua_app(Path(sim.sd_card_path), name,
                  FIXTURE.format(mode=mode, y0=y0, y1=y1, setup=setup))
    sim.launch_app(name)
    sim.wait_for_log("FPSFIX:READY", timeout=15)


# ── Off (the default) ───────────────────────────────────────────────────────


@pytest.mark.parametrize("mode", ["flush", "rows", "region"])
def test_off_by_default_leaves_every_corner_untouched(sim_factory, test_sd_card, mode):
    sim = boot(sim_factory, test_sd_card)
    run_fixture(sim, f"fps_off_{mode}", mode, 150, 159)
    time.sleep(1.5)  # past the first 1 s window
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    for corner in CORNERS:
        assert untouched(arr, corner), f"{corner} corner drawn with show_fps off"


# ── Each corner, flush ──────────────────────────────────────────────────────


@pytest.mark.parametrize("corner", list(CORNERS))
def test_counter_at_chosen_corner_flush(sim_factory, test_sd_card, corner):
    sim = boot(sim_factory, test_sd_card, show_fps=corner)
    run_fixture(sim, f"fps_flush_{corner}")
    arr = wait_screen(sim, lambda a: counter_at(a, corner))
    assert counter_at(arr, corner), f"no FPS counter at {corner}"
    for other in CORNERS:
        if other != corner:
            assert untouched(arr, other), f"{other} drawn with show_fps={corner}"


# ── flushRows / flushRegion only: out of the rows, straddling, inside ─────


@pytest.mark.parametrize("corner,mode,y0,y1", [
    ("tr", "rows", 150, 159),     # box rows 22..33 outside the band: window push
    ("bl", "rows", 150, 159),     # box rows 302..313 outside the band
    ("tr", "rows", 0, 27),        # the band straddles the box
    ("br", "rows", 0, 319),       # every call sends the whole screen
    ("tr", "region", 150, 159),   # flushRegion swaps: compose into either buffer
    ("bl", "region", 150, 159),
    ("tl", "region", 0, 27),
])
def test_counter_at_chosen_corner_partial(sim_factory, test_sd_card, corner, mode, y0, y1):
    sim = boot(sim_factory, test_sd_card, show_fps=corner)
    run_fixture(sim, f"fps_{mode}_{corner}_{y0}", mode, y0, y1)
    # "--" first, then a number once a 1 s window completes: two pushes
    # when the box lies outside the band.
    arr = wait_screen(sim, lambda a: counter_at(a, corner))
    assert counter_at(arr, corner), f"no FPS counter at {corner} for {mode}({y0}, {y1})"
    for other in CORNERS:
        if other != corner:
            assert untouched(arr, other), f"{other} drawn with show_fps={corner}"


def test_no_window_push_while_hardware_scrolled(sim_factory, test_sd_card):
    """With a non-zero scroll offset the panel shows GRAM rows at scrolled
    positions, so the box is neither pushed nor drawn: no black pixel
    anywhere on screen."""
    sim = boot(sim_factory, test_sd_card, show_fps="tr")
    run_fixture(sim, "fps_scrolled", "rows", 150, 159,
                setup="d.setScrollArea(0, 320, 160)\nd.setScrollOffset(64)")
    time.sleep(1.5)
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    black = (arr.sum(axis=2) == 0)
    assert not black.any(), (
        f"counter pixels on screen while scrolled: {int(black.sum())} black px, "
        f"first at {tuple(np.argwhere(black)[0])}")


# Unscrolled, then scrolled, then back. Two flushes then a clear leave both
# buffers blue with no counter in them (flush draws it into the one it
# sends), so the counter reaches the panel only by window pushes.
SCROLL_FIXTURE = """
local d = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
d.clear(0x001F) d.flush() d.clear(0x001F) d.flush() d.clear(0x001F)
local phase, n = 1, 0
while true do
    input.update()
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then return end
    if p & input.BTN_ENTER ~= 0 then
        phase = phase + 1
        if phase == 2 then d.setScrollArea(0, 320, 160) d.setScrollOffset(64)
        elseif phase == 3 then d.setScrollOffset(0) end
    end
    n = n + 1
    d.fillRect(0, 150, 320, 10, (n % 2 == 0) and 0xFFFF or 0x07FF)
    d.flushRows(150, 159)
    if n == 3 or p & input.BTN_ENTER ~= 0 then
        sys.log("FPSSCROLL:PHASE " .. phase .. " n=" .. n)
    end
    sys.sleep(16)
end
"""


def test_counter_leaves_and_returns_with_hardware_scroll(sim_factory, test_sd_card):
    """A counter pushed before the app scrolls must not stay in frame memory
    and slide with the content; when the offset is back to 0 it returns."""
    sim = boot(sim_factory, test_sd_card, show_fps="tr")
    stage_lua_app(Path(sim.sd_card_path), "fps_scroll_cycle", SCROLL_FIXTURE)
    sim.launch_app("fps_scroll_cycle")
    sim.wait_for_log("FPSSCROLL:PHASE 1", timeout=15)
    arr = wait_screen(sim, lambda a: counter_at(a, "tr"))
    assert counter_at(arr, "tr"), "no counter before scrolling"

    r = sim.keypress("enter")
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    sim.wait_for_log("FPSSCROLL:PHASE 2", timeout=10)
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    black = (arr.sum(axis=2) == 0)
    assert not black.any(), (
        f"counter left in frame memory while scrolled: {int(black.sum())} black "
        f"px, first at {tuple(np.argwhere(black)[0])}")

    r = sim.keypress("enter")
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    sim.wait_for_log("FPSSCROLL:PHASE 3", timeout=10)
    arr = wait_screen(sim, lambda a: counter_at(a, "tr"))
    assert counter_at(arr, "tr"), "counter did not come back at offset 0"


# ── Toasts over flushRows ───────────────────────────────────────────────────


def _toast_region(arr):
    return arr[TOAST_Y:TOAST_Y + TOAST_H, TOAST_X:TOAST_X + TOAST_W]


@pytest.mark.parametrize("mode", ["rows", "region"])
def test_toast_reaches_panel_and_clears_with_partial_flush(sim_factory, test_sd_card, mode):
    """A toast outside the flushRows/flushRegion band is pushed when it
    appears and, when it expires, the app's own pixels are pushed back (none
    of it was left in the app's draw buffers)."""
    sim = boot(sim_factory, test_sd_card, virtual_time=True)
    run_fixture(sim, f"fps_toast_{mode}", mode, 150, 159)
    assert _colours(_toast_region(_rgb(sim.screenshot_pil()))) == {BG}
    r = sim.keypress("enter")
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    arr = wait_screen(sim, lambda a: _colours(_toast_region(a)) != {BG})
    assert BG not in _colours(_toast_region(arr)), "toast not pushed to the panel"
    # 3 s of sim time later the toast is gone and the fixture's blue is back.
    arr = wait_screen(sim, lambda a: _colours(_toast_region(a)) == {BG}, timeout=15)
    assert _colours(_toast_region(arr)) == {BG}, "expired toast left pixels behind"


def test_toast_shown_again_after_system_menu(sim_factory, test_sd_card):
    """The system menu draws over the panel; when it closes, a toast still
    running outside the flushRows band is pushed again rather than left
    under the menu's picture."""
    sim = boot(sim_factory, test_sd_card)
    run_fixture(sim, "fps_toast_menu", "rows", 150, 159)
    r = sim.keypress("enter")
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)
    arr = wait_screen(sim, lambda a: TOAST_BG in _colours(_toast_region(a)))
    assert TOAST_BG in _colours(_toast_region(arr)), "toast not shown"
    sim.keypress("menu")
    wait_screen(sim, lambda a: TOAST_BG not in _colours(_toast_region(a)), timeout=3)
    sim.keypress("esc")
    arr = wait_screen(sim, lambda a: TOAST_BG in _colours(_toast_region(a)), timeout=2)
    assert TOAST_BG in _colours(_toast_region(arr)), "toast not shown after the menu closed"


# ── The Settings item ───────────────────────────────────────────────────────


def _show_fps(sim):
    cfg = json.loads((Path(sim.sd_card_path) / "system" / "config.json").read_text())
    return cfg.get("show_fps")


def _wait_cfg(sim, want, timeout=5.0):
    deadline = time.time() + timeout
    while True:
        try:
            got = _show_fps(sim)
        except (OSError, ValueError):   # mid-save: read again
            got = "<unreadable>"
        if got == want or time.time() >= deadline:
            return got
        time.sleep(0.05)


def test_settings_item_cycles_and_applies_on_close(sim_factory, test_sd_card):
    sim = boot(sim_factory, test_sd_card)
    run_fixture(sim, "fps_menu")
    # Main page (in an app): Battery, Settings, ...  Settings page:
    # Brightness, Battery %, Show FPS, ...
    sim.keypress("menu")
    time.sleep(0.3)
    sim.keypress_sequence(["down", "enter", "down", "down"], delay_ms=200)
    # Off is no key at all: it takes no room in the config store.
    for want in ["tr", "tl", "br", "bl", None, "tr"]:
        sim.keypress("enter")
        assert _wait_cfg(sim, want) == want, f"Show FPS did not cycle to {want}"
    sim.keypress_sequence(["esc", "esc"], delay_ms=200)
    arr = wait_screen(sim, lambda a: counter_at(a, "tr", number=False))
    assert counter_at(arr, "tr", number=False), "counter not shown after the menu closed"


# Presents blue frames for 1.5 s (so the counter shows a number), then
# flushes a full blue frame only on Enter.
STATIC_FIXTURE = """
local d = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local t0 = sys.getTimeMs()
repeat
    d.clear(0x001F)
    d.flush()
    sys.sleep(16)
until sys.getTimeMs() - t0 >= 1500
sys.log("FPSSTATIC:READY")
while true do
    input.update()
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then return end
    if p & input.BTN_ENTER ~= 0 then
        d.clear(0x001F)
        d.flush()
        sys.log("FPSSTATIC:FLUSHED")
    end
    sys.sleep(16)
end
"""


def test_new_corner_shows_at_the_next_present_after_the_menu(sim_factory, test_sd_card):
    """The menu gives the app its screen back as the app last presented it,
    counter included: a new corner shows from the app's next present (the
    OS does not repaint the counter on close; see os_overlay.h)."""
    sim = boot(sim_factory, test_sd_card, show_fps="tr")
    stage_lua_app(Path(sim.sd_card_path), "fps_static", STATIC_FIXTURE)
    sim.launch_app("fps_static")
    sim.wait_for_log("FPSSTATIC:READY", timeout=15)
    arr = wait_screen(sim, lambda a: counter_at(a, "tr"))
    assert counter_at(arr, "tr"), "no counter before the menu"

    sim.keypress("menu")
    time.sleep(0.3)
    sim.keypress_sequence(["down", "enter", "down", "down", "enter"], delay_ms=200)
    assert _wait_cfg(sim, "tl") == "tl"
    sim.keypress_sequence(["esc", "esc"], delay_ms=200)
    # The restored frame, its counter still at the old corner.
    arr = wait_screen(sim, lambda a: untouched(a, "br") and untouched(a, "bl"))
    assert counter_at(arr, "tr"), "the app's frame was not given back"
    assert untouched(arr, "tl"), "counter drawn at the new corner before a present"

    mark = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.keypress("enter")
    sim.wait_for_log("FPSSTATIC:FLUSHED", timeout=10, since_seq=mark)
    arr = wait_screen(sim, lambda a: counter_at(a, "tl"))
    assert counter_at(arr, "tl"), "no counter at the new corner"
    assert untouched(arr, "tr"), "old corner not cleared by the app's frame"


# ── The app's own counter (perf.drawFPS) ────────────────────────────────────


# The SDK Showcase's frame (issue #66): paced at 60, a full redraw with a
# 14px header and clipped content, its own perf.drawFPS(W - 30, HEADER_H + 1)
# (a counter that runs off the right edge: only "FPS:" shows), one flush and
# endFrame. With Show FPS on, that call drew a second, cut-off counter just
# above the OS box. Nothing but a counter draws green, yellow, red or grey.
APP_COUNTER_FIXTURE = """
local d = picocalc.display
local input = picocalc.input
local perf = picocalc.perf
local sys = picocalc.sys
perf.setTargetFPS(60)
local n = 0
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    d.clear(0x001F)
    d.fillRect(0, 0, 320, 14, 0x4208)
    d.drawText(4, 3, "SHOWCASE", 0xFFFF, 0x4208)
    d.setClipRect(0, 27, 320, 281)
    d.fillRect(20, 60, 100, 100, 0xFFFF)
    d.clearClipRect()
    perf.drawFPS({at})
    d.flush()
    perf.endFrame()
    n = n + 1
    if n == 3 then sys.log("FPSAPP:READY") end
end
"""


def counter_ink(arr) -> np.ndarray:
    """Where the screen shows counter text: the drawFPS colours, or the grey
    of the OS counter's "--"."""
    mask = np.zeros(arr.shape[:2], dtype=bool)
    for ink in NUMBER_INKS | {DASH_INK}:
        mask |= (arr == ink).all(axis=2)
    return mask


def only_counter_at(arr, corner) -> bool:
    """The OS counter at `corner` and no counter text anywhere else."""
    if not counter_at(arr, corner):
        return False
    ink = counter_ink(arr)
    x, y = CORNERS[corner]
    ink[y:y + BOX_H, x:x + BOX_W] = False
    return not ink.any()


def _ink_report(arr, corner=None) -> str:
    ink = counter_ink(arr)
    if corner:
        x, y = CORNERS[corner]
        ink[y:y + BOX_H, x:x + BOX_W] = False
    if not ink.any():
        return "no counter text outside the box"
    ys, xs = np.nonzero(ink)
    return (f"{int(ink.sum())} counter px outside the box, "
            f"x {xs.min()}..{xs.max()}, y {ys.min()}..{ys.max()}")


@pytest.mark.parametrize("corner,at", [
    ("tr", "290, 15"),   # the Showcase's call: two counters in issue #66
    ("bl", ""),          # drawFPS's default spot (top right), the box elsewhere
    ("tl", "200, 100"),
])
def test_os_counter_replaces_the_apps_own(sim_factory, test_sd_card, corner, at):
    """While Show FPS is on, perf.drawFPS draws nothing: the screen shows
    exactly one counter, the OS one at the chosen corner."""
    sim = boot(sim_factory, test_sd_card, show_fps=corner)
    name = f"fps_app_{corner}"
    stage_lua_app(Path(sim.sd_card_path), name, APP_COUNTER_FIXTURE.format(at=at))
    sim.launch_app(name)
    sim.wait_for_log("FPSAPP:READY", timeout=15)
    arr = wait_screen(sim, lambda a: only_counter_at(a, corner))
    assert counter_at(arr, corner), f"no OS counter at {corner}"
    assert only_counter_at(arr, corner), (
        f"a second counter on screen (drawFPS({at})): {_ink_report(arr, corner)}")


def test_apps_own_counter_draws_with_show_fps_off(sim_factory, test_sd_card):
    """With Show FPS off, perf.drawFPS(290, 15) draws "FPS: n" in the colour
    code there (cut off at the right edge), and nothing else does."""
    sim = boot(sim_factory, test_sd_card)
    stage_lua_app(Path(sim.sd_card_path), "fps_app_off",
                  APP_COUNTER_FIXTURE.format(at="290, 15"))
    sim.launch_app("fps_app_off")
    sim.wait_for_log("FPSAPP:READY", timeout=15)
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    ink = counter_ink(arr)
    ys, xs = np.nonzero(ink)
    assert ink.any(), "perf.drawFPS drew nothing with Show FPS off"
    assert xs.min() >= 290 and ys.min() >= 15 and ys.max() <= 22, _ink_report(arr)
    # Its glyph cells: the colour code on black, from x 290 to the edge.
    assert _colours(arr[15:23, 290:320]) - {(0, 0, 0)} <= NUMBER_INKS


# ── Native apps ─────────────────────────────────────────────────────────────


@pytest.mark.sd(extra=[("apps/hello_c", "apps/hello_c")])
def test_counter_over_native_app(sim_factory, test_sd_card):
    """The native display->flush draws the counter too (hello_c clears to
    black every frame, so only the text shows)."""
    sim = boot(sim_factory, test_sd_card, show_fps="tl")
    sim.launch_app("hello_c")
    sim.wait_frames(3, timeout=10)

    def ink(a):
        return _colours(_box(a, "tl")) - {(0, 0, 0)}

    arr = wait_screen(sim, lambda a: ink(a) and ink(a) <= NUMBER_INKS)
    assert ink(arr) and ink(arr) <= NUMBER_INKS, f"no counter over hello_c: {ink(arr)}"


@pytest.mark.parametrize("show_fps", [None, "tr"])
def test_native_drawfps_matches_lua(sim_factory, test_sd_card, show_fps):
    """Native perf->drawFPS is the Lua call: "FPS: n" in the colour code
    with Show FPS off (the simulator's trampoline used to draw "n fps" in
    white), nothing with it on, so the OS counter is the only one."""
    sim = boot(sim_factory, test_sd_card, show_fps=show_fps)
    shutil.copytree(Path(__file__).parent / "fixtures" / "native_drawfps",
                    Path(sim.sd_card_path) / app_rel_dir("native_drawfps", True))
    sim.launch_app("native_drawfps")
    sim.wait_for_log("DF:READY", timeout=15)
    if show_fps:
        arr = wait_screen(sim, lambda a: only_counter_at(a, show_fps))
        assert counter_at(arr, show_fps), f"no OS counter at {show_fps}"
        assert only_counter_at(arr, show_fps), (
            f"a second counter on screen: {_ink_report(arr, show_fps)}")
        # Not even in another colour: rows 15..21 there are the app's blue.
        assert _colours(arr[15:22, 290:320]) == {BG}, "drawFPS drew with Show FPS on"
        return
    sim.wait_frames(3)
    arr = _rgb(sim.screenshot_pil())
    ink = counter_ink(arr)
    assert ink.any(), "perf->drawFPS drew no counter text with Show FPS off"
    ys, xs = np.nonzero(ink)
    assert xs.min() >= 290 and ys.min() >= 15 and ys.max() <= 22, _ink_report(arr)
    # "FPS:" from x 290: the F's top bar is the first glyph row's ink.
    assert ink[15, 290:295].all(), "not the firmware's \"FPS: n\" text"
