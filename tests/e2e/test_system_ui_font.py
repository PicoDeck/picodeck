"""System-owned UI draws in the built-in 6x8 font whatever the app selected.

The system menu, the header's right-hand status (the title keeps the app's
font), ui.textInput / ui.confirm and toasts are laid out for 6x8. An app on
FONT_8X12 or FONT_SCIENTIFICA must get the same pixels as an app on 6x8, and
its own font back afterwards.

The fixtures draw no text of their own (a flat blue screen), so two runs that
differ only in the app's font must produce identical screenshots (the
header: identical right of the title).

Not covered here: the Lua error, load-failure, panic, HardFault, launcher
app-failure and OTA screens. In test mode the error screen is flushed and the
app ends at once (the text goes to the err log), so no stable frame exists to
compare; they are covered by review of the font-0 selection only.
"""

import shutil
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import stage_lua_app

FIXTURE = """
local d, ui, input, sys = picocalc.display, picocalc.ui, picocalc.input, picocalc.sys
local MODE = "{mode}"
d.setFont({font})
d.clear(0x001F)
if MODE == "header" then
    ui.drawHeader("Title")
    sys.log("SUF:HDRFONT " .. d.getFont())
end
if MODE == "menuitem" then
    sys.addMenuItem("Probe", function() sys.log("SUF:CB " .. d.getFont()) end)
end
d.flush()
sys.log("SUF:READY")
while true do
    input.update()
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then return end
    if p & input.BTN_RIGHT ~= 0 then sys.log("SUF:FONT " .. d.getFont()) end
    if p & input.BTN_ENTER ~= 0 then
        if MODE == "toast" then
            ui.toast("TOAST")
        elseif MODE == "input" then
            sys.log("SUF:CALL")
            local r = ui.textInput("Name", "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz")
            sys.log("SUF:RESULT " .. tostring(r) .. " FONT " .. d.getFont())
        elseif MODE == "browse" then
            sys.log("SUF:CALL")
            local r = picocalc.fs.browse()
            sys.log("SUF:RESULT " .. tostring(r) .. " FONT " .. d.getFont())
        elseif MODE == "confirm" then
            sys.log("SUF:CALL")
            local r = ui.confirm("Really do this? It is a fairly long question to wrap.")
            sys.log("SUF:RESULT " .. tostring(r) .. " FONT " .. d.getFont())
        end
    end
    if MODE == "toast" or MODE == "plain" then d.clear(0x001F); d.flush() end   -- toasts are drawn at a present
    sys.sleep(16)
end
"""

FONTS = {"8x12": 1, "scientifica": 2}


def _start(sim, mode, font):
    stage_lua_app(Path(sim.sd_card_path), f"suf_{mode}",
                  FIXTURE.format(mode=mode, font=font))
    sim.clear_log()
    sim.launch_app(f"suf_{mode}")
    sim.wait_for_log("SUF:READY", timeout=15)


def _shot(sim):
    return np.array(sim.screenshot_pil().convert("RGB")).astype(int)


def _w(v, bits):
    return v * 255 // ((1 << bits) - 1)


def rgb565(r, g, b):
    """RGB565(r, g, b) as the harness screenshot widens it."""
    return (_w(r >> 3, 5), _w(g >> 2, 6), _w(b >> 3, 5))


MENU_PANEL = rgb565(20, 28, 50)     # system_menu.c C_PANEL_BG
MENU_SEL = rgb565(40, 80, 160)      # C_SEL_BG
DLG_BG = rgb565(20, 28, 50)         # ui.c DLG_BG
HDR_CLOCK = rgb565(150, 155, 175)   # ui.c C_HDR_CLOCK


def _count(arr, colour):
    return int((arr == np.array(colour)).all(axis=-1).sum())


def _settled(sim, need=None, tries=60):
    """A screenshot once two consecutive ones agree and `need(arr)` holds
    (the modal is completely drawn, not a half-drawn frame)."""
    prev = _shot(sim)
    for _ in range(tries):
        time.sleep(0.1)
        cur = _shot(sim)
        if (np.array_equal(cur, prev) and not np.array_equal(cur, _BLUE(cur))
                and (need is None or need(cur))):
            return cur
        prev = cur
    raise AssertionError("the screen never settled with the expected content")


def _BLUE(a):
    out = np.zeros_like(a)
    out[..., 2] = 255
    return out





@pytest.mark.parametrize("font", sorted(FONTS))
def test_system_menu_ignores_the_app_font(sim_factory, test_sd_card, font):
    shots = {}
    for name, fid in (("ref", 0), (font, FONTS[font])):
        sim = sim_factory(test_sd_card)
        _start(sim, "menu", fid)
        sim.keypress("menu")
        shots[name] = _settled(
            sim, lambda a: _count(a, MENU_PANEL) > 3000 and _count(a, MENU_SEL) > 500)
        sim.keypress("esc")      # closes the menu
        time.sleep(0.5)
        sim.clear_log()
        sim.keypress("right")
        sim.wait_for_log(f"SUF:FONT {fid}", timeout=5)  # the app's font is back
    assert not np.array_equal(shots["ref"], _BLUE(shots["ref"])), "menu never drew"
    assert np.array_equal(shots["ref"], shots[font]), \
        "the system menu changed with the app's font"


@pytest.mark.parametrize("font", sorted(FONTS))
def test_header_status_ignores_the_app_font(sim_factory, test_sd_card, font):
    shots = {}
    for name, fid in (("ref", 0), (font, FONTS[font])):
        sim = sim_factory(test_sd_card)
        _start(sim, "header", fid)
        shots[name] = _shot(sim)
    # Status area (clock, battery, wifi): right of the title, header rows.
    ref, got = shots["ref"][0:21, 120:], shots[font][0:21, 120:]
    assert _count(ref, HDR_CLOCK) > 10, "no clock text in the status region"
    assert _count(got, HDR_CLOCK) > 10
    assert np.array_equal(ref, got), "header status changed with the app's font"
    # The title keeps the app's font: the left part differs.
    assert not np.array_equal(shots["ref"][0:21, :60], shots[font][0:21, :60])


@pytest.mark.parametrize("font", sorted(FONTS))
def test_toast_ignores_the_app_font(sim_factory, test_sd_card, font):
    shots = {}
    for name, fid in (("ref", 0), (font, FONTS[font])):
        sim = sim_factory(test_sd_card)
        _start(sim, "toast", fid)
        r = sim.keypress("enter")
        sim.wait_input_consumed(r["input_seq"], timeout=5.0)
        shots[name] = _settled(sim)
    assert not np.array_equal(shots["ref"], _BLUE(shots["ref"])), "no toast"
    assert np.array_equal(shots["ref"], shots[font])


@pytest.mark.parametrize("mode", ["input", "confirm", "browse"])
@pytest.mark.parametrize("font", sorted(FONTS))
def test_modal_ignores_the_app_font(sim_factory, test_sd_card, font, mode):
    shots = {}
    for name, fid in (("ref", 0), (font, FONTS[font])):
        sim = sim_factory(test_sd_card)
        _start(sim, mode, fid)
        sim.keypress("enter")
        sim.wait_for_log("SUF:CALL", timeout=5)
        time.sleep(0.6)   # past confirm's grace period; textInput drops earlier input
        if mode == "input":
            for ch in "xyz":
                sim.keypress(ch)
                time.sleep(0.1)
        shots[name] = _settled(sim, lambda a: _count(a, DLG_BG) > 3000)
        sim.keypress("esc")
        sim.wait_for_log("SUF:RESULT", timeout=5)
        lines = [l if isinstance(l, str) else l.get("text", "")
                 for l in sim.get_log_buffer()["lines"]]
        assert any(l.endswith(f"FONT {fid}") for l in lines
                   if l.startswith("SUF:RESULT")), lines
    diff = np.argwhere((shots["ref"] != shots[font]).any(axis=-1))
    assert not len(diff), (f"{mode} dialog changed with the app's font: "
                           f"{len(diff)} px, rows {diff[:,0].min() if len(diff) else 0}-"
                           f"{diff[:,0].max() if len(diff) else 0}")


@pytest.mark.parametrize("font", sorted(FONTS))
def test_menu_item_callback_sees_the_app_font(sim_factory, test_sd_card, font):
    sim = sim_factory(test_sd_card)
    _start(sim, "menuitem", FONTS[font])
    sim.keypress("menu")
    time.sleep(0.5)
    sim.keypress("enter")    # the app's item is the first one
    line = sim.wait_for_log("SUF:CB", timeout=5)
    text = line if isinstance(line, str) else str(line)
    assert f"SUF:CB {FONTS[font]}" in text, text


@pytest.mark.parametrize("font", sorted(FONTS))
def test_draw_header_restores_the_app_font(sim_factory, test_sd_card, font):
    sim = sim_factory(test_sd_card)
    _start(sim, "header", FONTS[font])
    lines = [l if isinstance(l, str) else l.get("text", "")
             for l in sim.get_log_buffer()["lines"]]
    assert f"SUF:HDRFONT {FONTS[font]}" in lines, lines


@pytest.mark.parametrize("font", sorted(FONTS))
def test_screenshot_notice_ignores_the_app_font(sim_factory, test_sd_card, font):
    """The "Screenshot: shotNNN.bmp" box is sized for 6x8. It is up for 400 ms,
    so poll for it."""
    def notice(arr):
        top = arr[0:12, 0:160]
        return _count(top, (255, 255, 255)) > 20   # white text on the box

    shots = {}
    for name, fid in (("ref", 0), (font, FONTS[font])):
        # Same file name in both runs: the notice names the slot it used.
        shutil.rmtree(Path(test_sd_card) / "screenshots", ignore_errors=True)
        sim = sim_factory(test_sd_card)
        _start(sim, "plain", fid)
        sim.call("inject_button", {"button": "f9", "action": "click"})
        deadline = time.time() + 5
        got = None
        while time.time() < deadline:
            a = _shot(sim)
            if notice(a):
                got = a
                break
        assert got is not None, "no screenshot notice"
        shots[name] = got[0:12, 0:160]
    assert np.array_equal(shots["ref"], shots[font])

