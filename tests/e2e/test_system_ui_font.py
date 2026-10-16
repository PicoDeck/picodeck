"""System-owned UI draws in the built-in 6x8 font whatever the app selected.

The system menu, the header's right-hand status (the title keeps the app's
font), ui.textInput / ui.confirm and toasts are laid out for 6x8. An app on
FONT_8X12 or FONT_SCIENTIFICA must get the same pixels as an app on 6x8, and
its own font back afterwards.

The fixtures draw no text of their own (a flat blue screen), so two runs that
differ only in the app's font must produce identical screenshots (the
header: identical right of the title).
"""

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
if MODE == "header" then ui.drawHeader("Title") end
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
        elseif MODE == "confirm" then
            sys.log("SUF:CALL")
            local r = ui.confirm("Really do this? It is a fairly long question to wrap.")
            sys.log("SUF:RESULT " .. tostring(r) .. " FONT " .. d.getFont())
        end
    end
    if MODE == "toast" then d.clear(0x001F); d.flush() end   -- toasts are drawn at a present
    sys.sleep(16)
end
"""

FONTS = {"8x12": 1, "scientifica": 2}


def _start(sim, mode, font):
    stage_lua_app(Path(sim.sd_card_path), f"suf_{mode}_{font}",
                  FIXTURE.format(mode=mode, font=font))
    sim.clear_log()
    sim.launch_app(f"suf_{mode}_{font}")
    sim.wait_for_log("SUF:READY", timeout=15)


def _shot(sim):
    return np.array(sim.screenshot_pil().convert("RGB")).astype(int)


def _settled(sim, tries=40):
    """A screenshot once two consecutive ones agree (the modal is drawn)."""
    prev = _shot(sim)
    for _ in range(tries):
        time.sleep(0.1)
        cur = _shot(sim)
        if np.array_equal(cur, prev) and not np.array_equal(cur, _BLUE(cur)):
            return cur
        prev = cur
    return prev


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
        shots[name] = _settled(sim)
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
    assert (ref != _BLUE(shots["ref"])[0:21, 120:]).any()
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


@pytest.mark.parametrize("mode", ["input", "confirm"])
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
        shots[name] = _settled(sim)
        sim.keypress("esc")
        sim.wait_for_log("SUF:RESULT", timeout=5)
        lines = [l if isinstance(l, str) else l.get("text", "")
                 for l in sim.get_log_buffer()["lines"]]
        assert any(l.endswith(f"FONT {fid}") for l in lines
                   if l.startswith("SUF:RESULT")), lines
    assert np.array_equal(shots["ref"], shots[font]), \
        f"{mode} dialog changed with the app's font"
