"""Option rows in the system menu (picocalc.sys.addMenuOption).

The fixture registers two option rows (Focus, Mode) and two action rows
(Item3, Item4), after probing the argument checks. Left/Right/Enter change a
row's value in the menu; the callback of every changed row runs after the menu
closes, in row order, before an action row's callback.

Runs on the simulator and on the device. The fixture resets the idle timer on
every pass (see test_menu_restore.py for why).
"""

from __future__ import annotations

import time

import pytest

pytestmark = [pytest.mark.both, pytest.mark.timeout(300),
              pytest.mark.sd(fixtures=[], reserve=1)]

APP = "menu_option"
APP_ID = "net.picodeck.menu_option"

FIXTURE = r"""
local sys, input = picocalc.sys, picocalc.input
sys.resetIdleTimer()
local function try(tag, f) local ok, err = pcall(f); sys.log("MO:" .. tag .. "=" .. (ok and "ok" or tostring(err))) end
try("empty", function() sys.addMenuOption("X", {}, "a", function() end) end)
try("notin", function() sys.addMenuOption("X", {"a", "b"}, "c", function() end) end)
try("longlabel", function() sys.addMenuOption("ThirteenChars", {"a"}, "a", function() end) end)
try("longvalue", function() sys.addMenuOption("X", {"FifteenCharsXYZ"}, "FifteenCharsXYZ", function() end) end)
local many = {} for i = 1, 17 do many[i] = tostring(i) end
try("many", function() sys.addMenuOption("X", many, "1", function() end) end)
sys.addMenuOption("Focus", {"15 min", "20 min", "25 min"}, "20 min",
    function(v, i) sys.log("MO:focus=" .. v .. "," .. i) end)
sys.addMenuOption("Mode", {"A", "B"}, "A", function(v, i) sys.log("MO:mode=" .. v .. "," .. i) end)
sys.addMenuItem("Item3", function() sys.log("MO:item3") end)
sys.addMenuItem("Item4", function() end)
try("fifth", function() sys.addMenuOption("X", {"a"}, "a", function() end) end)
sys.log("MO:READY")
while true do
    sys.resetIdleTimer()
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    sys.sleep(20)
end
"""


def _launch(target):
    target.stage_lua_app(APP, FIXTURE, id=APP_ID)
    cur = target.log_cursor()
    r = target.launch_app(APP)
    assert r["launched"], r
    target.wait_for_log(r"MO:READY", timeout=15, since=cur)
    return cur


def _open(target):
    target.keypress("menu")
    time.sleep(0.4)


def _mo_lines(target, since):
    return [l for l in target.get_log_lines(since)
            if "MO:focus" in l or "MO:mode" in l or "MO:item3" in l]


def _keys(target, *keys):
    for k in keys:
        target.keypress(k)
        time.sleep(0.1)


def _quit(target):
    target.keypress("esc")
    target.wait_for_exit(timeout=15)


def test_bad_arguments_raise(target):
    cur = _launch(target)
    lines = target.get_log_lines(cur)

    def line(tag):
        return next(l for l in lines if f"MO:{tag}=" in l)

    assert "at least one option" in line("empty")
    assert "not in options" in line("notin")
    assert "label too long (max 12)" in line("longlabel")
    assert "option too long (max 14)" in line("longvalue")
    assert "too many options (max 16)" in line("many")
    assert "too many menu items (max 4)" in line("fifth")
    _quit(target)


def test_right_steps_and_calls_back_only_after_close(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right")
    time.sleep(0.5)
    assert not _mo_lines(target, cur)
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
    _quit(target)


def test_stepping_back_to_the_opening_value_calls_back_nothing(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right", "left")
    target.keypress("esc")
    time.sleep(1.0)
    assert not _mo_lines(target, cur)
    _quit(target)


def test_right_and_left_stop_at_the_ends(target):
    cur = _launch(target)
    _open(target)
    _keys(target, *["right"] * 5)
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
    cur = target.log_cursor()
    _open(target)
    _keys(target, *["left"] * 5)
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=15 min,1", timeout=10, since=cur)
    _quit(target)


def test_enter_steps_forward_and_wraps(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "enter", "enter")
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=15 min,1", timeout=10, since=cur)
    _quit(target)


def test_row_keeps_its_value_between_visits(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right")
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
    cur = target.log_cursor()
    _open(target)
    target.keypress("esc")
    time.sleep(1.0)
    assert not _mo_lines(target, cur)
    _open(target)
    _keys(target, "left")
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=20 min,2", timeout=10, since=cur)
    _quit(target)


def test_option_callbacks_run_before_the_action_callback(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right", "down", "right", "down")
    target.keypress("enter")
    target.wait_for_log(r"MO:item3", timeout=10, since=cur)
    got = [l for l in _mo_lines(target, cur)]
    order = [next(i for i, l in enumerate(got) if k in l)
             for k in ("MO:focus=25 min,3", "MO:mode=B,2", "MO:item3")]
    assert order == sorted(order), got
    _quit(target)


def test_exit_app_runs_changed_option_callbacks_first(target):
    """Exit App used to clear the rows inside the menu loop, which dropped
    the changed options before their callbacks could run."""
    cur = _launch(target)
    _open(target)
    target.keypress("right")  # Focus 20 min -> 25 min
    time.sleep(0.1)
    target.keypress("up")  # wraps to Exit App, the last row
    time.sleep(0.1)
    target.keypress("enter")
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
    out = target.wait_for_exit(timeout=15)
    assert out["result"] in ("exit_sentinel", "returned"), out
    # The Enter that chose Exit App does not reach the launcher.
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        assert target.status()["app"] == "launcher"
        time.sleep(0.1)


APP2 = "menu_option_reset"
APP2_ID = "net.picodeck.menu_option_reset"

# Rows, top to bottom: "Reset rows" (clears and re-adds Focus at 15 min plus
# the Reset item), then after the reset Focus and "Reset rows". The Quit row
# is an option whose callback calls sys.exit().
FIXTURE2 = r"""
local sys, input = picocalc.sys, picocalc.input
sys.resetIdleTimer()
local function rows(current)
    sys.addMenuOption("Focus", {"15 min", "20 min", "25 min"}, current,
        function(v, i) sys.log("MO:focus=" .. v .. "," .. i) end)
    sys.addMenuOption("Quit", {"stay", "go"}, "stay",
        function(v, i) sys.log("MO:quit=" .. v); if v == "go" then sys.exit() end end)
    sys.addMenuItem("Reset rows", function()
        sys.clearMenuItems()
        rows("15 min")
        sys.log("MO:RESET")
    end)
end
rows("20 min")
sys.log("MO:READY")
while true do
    sys.resetIdleTimer()
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    sys.sleep(20)
end
"""


def _launch2(target):
    target.stage_lua_app(APP2, FIXTURE2, id=APP2_ID)
    cur = target.log_cursor()
    r = target.launch_app(APP2)
    assert r["launched"], r
    target.wait_for_log(r"MO:READY", timeout=15, since=cur)
    return cur


def test_clear_and_re_add_rows_shows_the_new_values(target):
    cur = _launch2(target)
    _open(target)
    _keys(target, "down", "down", "enter")  # "Reset rows"
    target.wait_for_log(r"MO:RESET", timeout=10, since=cur)
    cur = target.log_cursor()
    _open(target)
    _keys(target, "right")  # the new Focus row starts at 15 min
    target.keypress("esc")
    target.wait_for_log(r"MO:focus=20 min,2", timeout=10, since=cur)
    _quit(target)


def test_option_callback_that_exits_leaves_cleanly(target):
    cur = _launch2(target)
    _open(target)
    _keys(target, "down", "right")  # Quit: stay -> go
    target.keypress("esc")
    target.wait_for_log(r"MO:quit=go", timeout=10, since=cur)
    out = target.wait_for_exit(timeout=15)
    assert out["result"] in ("exit_sentinel", "returned"), out
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        assert target.status()["app"] == "launcher"
        time.sleep(0.1)
