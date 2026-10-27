"""Option rows in the system menu (picocalc.sys.addMenuOption).

The fixture registers two option rows (Focus, Mode), an action row (Item3)
and the widest option row the checks accept, after probing the argument
checks. Left/Right/Enter change a row's value in the menu; the callback of
every changed row runs after the menu closes, in row order, before an action
row's callback.

The fixtures clear the screen blue and never draw again, so a test sees the
menu open (the pixel left of its panel darkens) and close (blue again) instead
of sleeping and hoping the keys reached it.

Runs on the simulator and on the device. The fixture resets the idle timer on
every pass (see test_menu_restore.py for why).
"""

from __future__ import annotations

import io
import time

import pytest

pytestmark = [pytest.mark.both, pytest.mark.timeout(300),
              pytest.mark.sd(fixtures=[], reserve=1)]

APP = "menu_option"
APP_ID = "net.picodeck.menu_option"

# Every fixture starts like this: a blue screen and no more drawing.
PROLOGUE = r"""
local pc = picocalc
local sys, input, d = pc.sys, pc.input, pc.display
sys.resetIdleTimer()
d.clear(d.BLUE)
d.flush()
"""

MAIN_LOOP = r"""
sys.log("MO:READY")
while true do
    sys.resetIdleTimer()
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    sys.sleep(20)
end
"""

FIXTURE = PROLOGUE + r"""
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
-- The widest row the checks accept: a 12-byte label, 14-byte values.
sys.addMenuOption("TwelveBytesX", {"FourteenBytesA", "FourteenBytesB"}, "FourteenBytesA",
    function(v, i) sys.log("MO:wide=" .. v .. "," .. i) end)
try("fifth", function() sys.addMenuOption("X", {"a"}, "a", function() end) end)
""" + MAIN_LOOP

BLUE = (0, 0, 255)
# Left of the 200 px menu panel, which is centred (as in test_menu_restore).
OUTSIDE = (20, 40)
# Below any text of the Lua error screen, right of its footer.
LOWER_RIGHT = (300, 250)
TOLERANCE = 40

# An earlier run that leaked the menu's framebuffer copy (2 x 200 KB) would
# leave the next run that much less PSRAM; run-to-run noise is far smaller
# (test_menu_restore.py).
LEAK_TOLERANCE = 64 * 1024


def _pixel(target, xy):
    from PIL import Image
    return Image.open(io.BytesIO(target.screenshot())).convert("RGB").getpixel(xy)


def _near(rgb, want) -> bool:
    return all(abs(a - b) <= TOLERANCE for a, b in zip(rgb, want))


def _wait_pixel(target, xy, pred, what: str, timeout: float = 10.0):
    """Poll screenshots until pred(pixel at xy) holds."""
    deadline = time.monotonic() + timeout
    while True:
        rgb = _pixel(target, xy)
        if pred(rgb):
            return rgb
        if time.monotonic() >= deadline:
            pytest.fail(f"{what}: pixel {xy} is {rgb}")
        time.sleep(0.05)


def _stage_and_launch(target, name, code, app_id):
    target.stage_lua_app(name, code, id=app_id)
    cur = target.log_cursor()
    r = target.launch_app(name)
    assert r["launched"], r
    target.wait_for_log(r"MO:READY", timeout=15, since=cur)
    return cur


def _launch(target):
    return _stage_and_launch(target, APP, FIXTURE, APP_ID)


def _open(target):
    """Open the menu and wait until it shows (the app's blue darkens)."""
    target.keypress("menu")
    _wait_pixel(target, OUTSIDE, lambda px: not _near(px, BLUE),
                "the system menu did not appear")


def _close(target):
    """Esc closes the menu; wait until the app's blue screen is back."""
    target.keypress("esc")
    _wait_pixel(target, OUTSIDE, lambda px: _near(px, BLUE),
                "the system menu did not close")


def _mo_lines(target, since):
    return [l for l in target.get_log_lines(since)
            if any(k in l for k in ("MO:focus", "MO:mode", "MO:item3",
                                    "MO:wide"))]


def _keys(target, *keys):
    for k in keys:
        target.keypress(k)
        time.sleep(0.1)


def _quit(target):
    target.keypress("esc")
    out = target.wait_for_exit(timeout=15)
    assert out["result"] == "returned", out


def _stays_at_launcher(target, what: str):
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        assert target.status()["app"] == "launcher", what
        time.sleep(0.1)


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
    _close(target)
    time.sleep(1.0)
    assert not _mo_lines(target, cur)
    # The menu had the keys: one more step does call back.
    _open(target)
    _keys(target, "right")
    _close(target)
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
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
    _close(target)
    target.wait_for_log(r"MO:focus=25 min,3", timeout=10, since=cur)
    cur = target.log_cursor()
    _open(target)
    _close(target)
    time.sleep(1.0)
    assert not _mo_lines(target, cur)
    # The row kept 25 min: Left steps it to 20 min, and calls back.
    _open(target)
    _keys(target, "left")
    _close(target)
    target.wait_for_log(r"MO:focus=20 min,2", timeout=10, since=cur)
    _quit(target)


def test_option_callbacks_run_before_the_action_callback(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right", "down", "right", "down")
    target.keypress("enter")
    target.wait_for_log(r"MO:item3", timeout=10, since=cur)
    got = [l for l in _mo_lines(target, cur)]
    order = [next((i for i, l in enumerate(got) if k in l), None)
             for k in ("MO:focus=25 min,3", "MO:mode=B,2", "MO:item3")]
    assert None not in order and order == sorted(order), got
    _quit(target)


def test_widest_row_steps_and_calls_back(target):
    """A 12-byte label with a 14-byte value is the widest row accepted
    ("TwelveBytesX: FourteenBytesB <>", 31 characters)."""
    cur = _launch(target)
    _open(target)
    _keys(target, "down", "down", "down", "right")
    _close(target)
    target.wait_for_log(r"MO:wide=FourteenBytesB,2", timeout=10, since=cur)
    got = _mo_lines(target, cur)
    assert len(got) == 1 and "MO:wide=FourteenBytesB,2" in got[0], got
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
    _stays_at_launcher(target, "the Enter that chose Exit App reached the "
                       "launcher")


APP2 = "menu_option_reset"
APP2_ID = "net.picodeck.menu_option_reset"
MEM2_FILE = f"/data/{APP2_ID}/mem.txt"

# Rows, top to bottom: Focus, Quit (an option whose callback calls sys.exit()
# on "go"), "Reset rows" (clears and re-adds them, Focus at 15 min). mem.txt
# records the PSRAM heap's free bytes at start, so a test can tell whether an
# earlier run leaked.
FIXTURE2 = PROLOGUE + r"""
collectgarbage("collect")
local f = pc.fs.open(pc.fs.appPath("mem.txt"), "w")
pc.fs.write(f, tostring(sys.getMemInfo().psram_free))
pc.fs.close(f)
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
""" + MAIN_LOOP


def _launch2(target):
    target.delete_file(MEM2_FILE)
    return _stage_and_launch(target, APP2, FIXTURE2, APP2_ID)


def _read_mem(target, path, timeout: float = 10.0) -> int:
    deadline = time.monotonic() + timeout
    while True:
        try:
            return int(target.read_file(path).decode().strip())
        except (FileNotFoundError, ValueError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


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
    """The callback leaves by longjmp (sys.exit()). Nothing of the menu's or
    the rows' leaks: the next run starts with as much PSRAM free."""
    cur = _launch2(target)
    before = _read_mem(target, MEM2_FILE)
    _open(target)
    _keys(target, "down", "right")  # Quit: stay -> go
    target.keypress("esc")
    target.wait_for_log(r"MO:quit=go", timeout=10, since=cur)
    out = target.wait_for_exit(timeout=15)
    assert out["result"] in ("exit_sentinel", "returned"), out
    _stays_at_launcher(target, "the app did not leave, or a key reached the "
                       "launcher")

    _launch2(target)
    after = _read_mem(target, MEM2_FILE)
    print(f"psram_free at start: before={before} after={after}")
    assert after >= before - LEAK_TOLERANCE, (
        f"PSRAM free fell from {before} to {after} bytes across an option "
        "callback that called sys.exit()")
    _quit(target)


APP4 = "menu_option_raise"
APP4_ID = "net.picodeck.menu_option_raise"

FIXTURE4 = PROLOGUE + r"""
sys.addMenuOption("Boom", {"ok", "raise"}, "ok", function(v)
    if v == "raise" then error("MO option boom") end
end)
sys.addMenuItem("Bang", function() error("MO action bang") end)
""" + MAIN_LOOP


@pytest.mark.parametrize("row, keys, message", [
    ("option", ("right", "esc"), "MO option boom"),
    ("action", ("down", "enter"), "MO action bang"),
])
def test_callback_that_raises_shows_the_error_and_ends_the_app(
        target, row, keys, message):
    """An option row's callback that raises does what an action row's does:
    the Lua error ends the app (the error screen), and the launcher comes
    back and starts apps."""
    _stage_and_launch(target, APP4, FIXTURE4, APP4_ID)
    _open(target)
    _keys(target, *keys)
    if target.kind == "hw":
        # The device's error screen (black) waits for Esc; the simulator's
        # test mode returns from it at once.
        _wait_pixel(target, LOWER_RIGHT, lambda px: _near(px, (0, 0, 0)),
                    "the error screen did not show")
        target.keypress("esc")
    out = target.wait_for_exit(timeout=15)
    assert out["result"] == "error", out
    assert message in out["error"], out
    _stays_at_launcher(target, "the app did not end")
    # The launcher still starts apps.
    _stage_and_launch(target, APP4, FIXTURE4, APP4_ID)
    _quit(target)


APP5 = "menu_option_probe"
APP5_ID = "net.picodeck.menu_option_probe"

# Runs one probe and returns.
#   refs: a row's options table and callback live only in a weak table, so
#         once clearMenuItems has dropped the row, a full collection frees
#         both, unless the OS still references them. Before the clear the
#         row must hold its callback (the probe can see a reference).
#   umm:  after one warm-up round, 200 rounds of four 16-value rows added
#         and cleared; the PSRAM heap must end where it started. A leaked
#         240-byte value block per row would be ~190 KB; what the loop costs
#         once (a 4 KB small-object slab and the like, ~9 KB on the
#         simulator) does not grow with the rounds.
FIXTURE5 = r"""
local sys = picocalc.sys
local MODE = "@MODE@"
local function full_gc() collectgarbage("collect"); collectgarbage("collect") end
if MODE == "refs" then
    local weak = setmetatable({}, {__mode = "v"})
    local function register()
        local calls = 0
        local t = {"a", "b"}
        local f = function() calls = calls + 1 end
        weak.t, weak.f = t, f
        sys.addMenuOption("Probe", t, "a", f)
    end
    register()
    full_gc()
    local held = weak.f ~= nil
    sys.clearMenuItems()
    full_gc()
    sys.log("MO:REFS held=" .. tostring(held) ..
            " f=" .. (weak.f == nil and "freed" or "kept") ..
            " t=" .. (weak.t == nil and "freed" or "kept"))
else
    local opts = {}
    for i = 1, 16 do opts[i] = string.format("option %02d xxxx", i) end
    local function cb() end
    local function round()
        for row = 1, 4 do sys.addMenuOption("Row" .. row, opts, opts[1], cb) end
        sys.clearMenuItems()
    end
    -- One round first, so what a first call allocates for good (the
    -- registry's growth, small-object slabs) is not counted as a leak.
    round()
    full_gc()
    local before = sys.getMemInfo().psram_free
    for _ = 1, 200 do round() end
    full_gc()
    local after = sys.getMemInfo().psram_free
    sys.log("MO:UMM before=" .. before .. " after=" .. after)
end
"""


def _probe(target, mode, pattern):
    name = f"{APP5}_{mode}"
    target.stage_lua_app(name, FIXTURE5.replace("@MODE@", mode),
                         id=f"{APP5_ID}_{mode}")
    cur = target.log_cursor()
    r = target.launch_app(name)
    assert r["launched"], r
    line = target.wait_for_log(pattern, timeout=30, since=cur)
    out = target.wait_for_exit(timeout=30)
    assert out["result"] == "returned", out
    print(line)
    return line


def test_cleared_rows_keep_no_references(target):
    line = _probe(target, "refs", r"MO:REFS ")
    assert "held=true" in line, f"the probe saw no reference at all: {line}"
    assert "f=freed" in line and "t=freed" in line, (
        f"clearMenuItems left the row's callback or options referenced: {line}")


def test_cleared_rows_give_their_memory_back(target):
    import re
    line = _probe(target, "umm", r"MO:UMM ")
    m = re.search(r"before=(\d+) after=(\d+)", line)
    assert m, line
    before, after = int(m.group(1)), int(m.group(2))
    assert before - after < 16 * 1024, (
        f"200 rounds of adding and clearing four option rows cost "
        f"{before - after} bytes of PSRAM ({line})")
