"""Option rows in the system menu (picocalc.sys.addMenuOption).

The fixture registers two option rows (Focus, Mode), an action row (Item3)
and the widest option row the checks accept, after probing the argument
checks. Left/Right/Enter change a row's value in the menu; the callback of
every changed row runs after the menu closes, in row order, before an action
row's callback.

The fixtures clear the screen blue and never draw again, so a test sees the
menu open (the pixel left of its panel darkens) and close (blue again) instead
of sleeping and hoping the keys reached it.

Callback results are the lines each fixture appends to mo.txt in its data
directory (note() also logs them). The serial log is not evidence here: on
the device, a screenshot and the log monitor share the port and swallow each
other's bytes, and the callbacks run just as _close() polls screenshots.

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
local function note(s)
    sys.log(s)
    local f = pc.fs.open(pc.fs.appPath("mo.txt"), "a")
    pc.fs.write(f, s .. "\n")
    pc.fs.close(f)
end
sys.resetIdleTimer()
d.clear(d.BLUE)
d.flush()
"""

MAIN_LOOP = r"""
note("MO:READY")
while true do
    sys.resetIdleTimer()
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    sys.sleep(20)
end
"""

FIXTURE = PROLOGUE + r"""
local function try(tag, f) local ok, err = pcall(f); note("MO:" .. tag .. "=" .. (ok and "ok" or tostring(err))) end
try("empty", function() sys.addMenuOption("X", {}, "a", function() end) end)
try("notin", function() sys.addMenuOption("X", {"a", "b"}, "c", function() end) end)
try("longlabel", function() sys.addMenuOption("ThirteenChars", {"a"}, "a", function() end) end)
try("longvalue", function() sys.addMenuOption("X", {"FifteenCharsXYZ"}, "FifteenCharsXYZ", function() end) end)
local many = {} for i = 1, 17 do many[i] = tostring(i) end
try("many", function() sys.addMenuOption("X", many, "1", function() end) end)
sys.addMenuOption("Focus", {"15 min", "20 min", "25 min"}, "20 min",
    function(v, i) note("MO:focus=" .. v .. "," .. i) end)
-- The callback gets the value as registered: changing the table afterwards
-- changes nothing.
local modes = {"A", "B"}
sys.addMenuOption("Mode", modes, "A", function(v, i) note("MO:mode=" .. v .. "," .. i) end)
modes[2] = "Z"
sys.addMenuItem("Item3", function() note("MO:item3") end)
-- The widest row the checks accept: a 12-byte label, 14-byte values.
sys.addMenuOption("TwelveBytesX", {"FourteenBytesA", "FourteenBytesB"}, "FourteenBytesA",
    function(v, i) note("MO:wide=" .. v .. "," .. i) end)
try("fifth", function() sys.addMenuOption("X", {"a"}, "a", function() end) end)
""" + MAIN_LOOP

BLUE = (0, 0, 255)
# Left of the 200 px menu panel, which is centred (as in test_menu_restore).
OUTSIDE = (20, 40)
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


def _mo_path(app_id):
    return f"/data/{app_id}/mo.txt"


def _lines(target, app_id):
    """The lines the fixture has noted so far."""
    try:
        data = target.read_file(_mo_path(app_id))
    except FileNotFoundError:
        return []
    return data.decode("utf-8", errors="replace").splitlines()


def _mark(target, app_id) -> int:
    """How many lines are noted so far (the `since` of the helpers below)."""
    return len(_lines(target, app_id))


def _wait_line(target, app_id, pattern, since=0, timeout=10.0):
    """Poll the results file until a line past the first `since` matches."""
    import re
    deadline = time.monotonic() + timeout
    while True:
        lines = _lines(target, app_id)
        for l in lines[since:]:
            if re.search(pattern, l):
                return l
        if time.monotonic() >= deadline:
            pytest.fail(f"no line matching {pattern!r} in {lines[since:]}")
        time.sleep(0.1)


def _stage_and_launch(target, name, code, app_id):
    """Start the fixture with an empty results file; returns the line count
    (READY included) to count from."""
    target.stage_lua_app(name, code, id=app_id)
    target.delete_file(_mo_path(app_id))
    r = target.launch_app(name)
    assert r["launched"], r
    _wait_line(target, app_id, r"^MO:READY$", timeout=15)
    return _mark(target, app_id)


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


def _mo_lines(target, since, app_id=APP_ID):
    return [l for l in _lines(target, app_id)[since:]
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
    lines = _lines(target, APP_ID)

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
    _wait_line(target, APP_ID, r"MO:focus=25 min,3", since=cur)
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
    _wait_line(target, APP_ID, r"MO:focus=25 min,3", since=cur)
    _quit(target)


def test_right_and_left_stop_at_the_ends(target):
    cur = _launch(target)
    _open(target)
    _keys(target, *["right"] * 5)
    target.keypress("esc")
    _wait_line(target, APP_ID, r"MO:focus=25 min,3", since=cur)
    cur = _mark(target, APP_ID)
    _open(target)
    _keys(target, *["left"] * 5)
    target.keypress("esc")
    _wait_line(target, APP_ID, r"MO:focus=15 min,1", since=cur)
    _quit(target)


def test_enter_steps_forward_and_wraps(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "enter", "enter")
    target.keypress("esc")
    _wait_line(target, APP_ID, r"MO:focus=15 min,1", since=cur)
    _quit(target)


def test_row_keeps_its_value_between_visits(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right")
    _close(target)
    _wait_line(target, APP_ID, r"MO:focus=25 min,3", since=cur)
    cur = _mark(target, APP_ID)
    _open(target)
    _close(target)
    time.sleep(1.0)
    assert not _mo_lines(target, cur)
    # The row kept 25 min: Left steps it to 20 min, and calls back.
    _open(target)
    _keys(target, "left")
    _close(target)
    _wait_line(target, APP_ID, r"MO:focus=20 min,2", since=cur)
    _quit(target)


def test_option_callbacks_run_before_the_action_callback(target):
    cur = _launch(target)
    _open(target)
    _keys(target, "right", "down", "right", "down")
    target.keypress("enter")
    _wait_line(target, APP_ID, r"MO:item3", since=cur)
    got = [l for l in _mo_lines(target, cur)]
    # Mode's table was changed to {"A", "Z"} after it was registered: the
    # callback still gets "B".
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
    _wait_line(target, APP_ID, r"MO:wide=FourteenBytesB,2", since=cur)
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
    _wait_line(target, APP_ID, r"MO:focus=25 min,3", since=cur)
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
        function(v, i) note("MO:focus=" .. v .. "," .. i) end)
    sys.addMenuOption("Quit", {"stay", "go"}, "stay",
        function(v, i) note("MO:quit=" .. v); if v == "go" then sys.exit() end end)
    sys.addMenuItem("Reset rows", function()
        sys.clearMenuItems()
        rows("15 min")
        note("MO:RESET")
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
    _wait_line(target, APP2_ID, r"MO:RESET", since=cur)
    cur = _mark(target, APP2_ID)
    _open(target)
    _keys(target, "right")  # the new Focus row starts at 15 min
    target.keypress("esc")
    _wait_line(target, APP2_ID, r"MO:focus=20 min,2", since=cur)
    _quit(target)


def test_option_callback_that_exits_leaves_cleanly(target):
    """The callback leaves by longjmp (sys.exit()). Nothing of the menu's or
    the rows' leaks: the next run starts with as much PSRAM free."""
    cur = _launch2(target)
    before = _read_mem(target, MEM2_FILE)
    _open(target)
    _keys(target, "down", "right")  # Quit: stay -> go
    target.keypress("esc")
    _wait_line(target, APP2_ID, r"MO:quit=go", since=cur)
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


APP3 = "menu_option_rebuild"
APP3_ID = "net.picodeck.menu_option_rebuild"

# Rows, top to bottom: Rebuild (its callback clears the rows and adds them
# again without Act), Wait (its callback services the app with sys.sleep
# until Act has run, so the menu can open again from inside it), Focus, Act.
# Every callback logs what it was called with, so a call through a replaced
# slot shows (as "nil" arguments, or the wrong row).
FIXTURE3 = PROLOGUE + r"""
local acted = false
local rows
rows = function(full)
    sys.addMenuOption("Rebuild", {"no", "yes"}, "no", function(v, i)
        note("MO:rebuild=" .. tostring(v) .. "," .. tostring(i))
        if v == "yes" then
            sys.clearMenuItems()
            rows(false)
            note("MO:REBUILT")
        end
    end)
    sys.addMenuOption("Wait", {"no", "yes"}, "no", function(v, i)
        note("MO:wait=" .. tostring(v) .. "," .. tostring(i))
        if v == "yes" then
            acted = false
            note("MO:WAITING")
            while not acted do sys.resetIdleTimer(); sys.sleep(20) end
            note("MO:WAITED")
        end
    end)
    sys.addMenuOption("Focus", {"15 min", "20 min", "25 min"}, "20 min",
        function(v, i) note("MO:focus=" .. tostring(v) .. "," .. tostring(i)) end)
    if full then
        sys.addMenuItem("Act", function() acted = true; note("MO:act") end)
    end
end
rows(true)
""" + MAIN_LOOP


def _mo3_lines(target, since):
    return [l for l in _lines(target, APP3_ID)[since:] if l != "MO:READY"]


def test_callback_that_rebuilds_the_rows_ends_the_visit(target):
    """An option callback clears and re-adds the rows. The callbacks still
    pending from that visit (a later changed row, the chosen action row)
    would run on slots that now hold other rows, so none of them runs, and
    the app goes on with its new rows."""
    cur = _stage_and_launch(target, APP3, FIXTURE3, APP3_ID)
    _open(target)
    # Rebuild no -> yes, Focus 20 -> 25 min, then Enter on Act.
    _keys(target, "right", "down", "down", "right", "down")
    target.keypress("enter")
    _wait_pixel(target, OUTSIDE, lambda px: _near(px, BLUE),
                "the system menu did not close")
    _wait_line(target, APP3_ID, r"MO:REBUILT", since=cur)
    time.sleep(1.0)
    assert _mo3_lines(target, cur) == ["MO:rebuild=yes,2", "MO:REBUILT"], (
        "a callback ran after the rows were rebuilt")
    assert target.status()["app"] != "launcher", (
        "the app did not survive the rebuild")

    # The rebuilt rows work: Focus (third row) at 20 min steps to 25 min.
    cur = _mark(target, APP3_ID)
    _open(target)
    _keys(target, "down", "down", "right")
    _close(target)
    _wait_line(target, APP3_ID, r"MO:focus=25 min,3", since=cur)
    _quit(target)


def test_menu_opened_from_a_callback_keeps_the_pending_callbacks(target):
    """An option callback services the app and the menu opens again from
    inside it. That inner visit takes its own opening values; the outer
    visit's changed rows still call back once it has closed."""
    cur = _stage_and_launch(target, APP3, FIXTURE3, APP3_ID)
    _open(target)
    # Wait no -> yes, Focus 20 -> 25 min, Esc.
    _keys(target, "down", "right", "down", "right")
    target.keypress("esc")
    _wait_line(target, APP3_ID, r"MO:WAITING", since=cur)
    _open(target)  # from Wait's callback (its sys.sleep)
    _keys(target, "down", "down", "down")
    target.keypress("enter")  # Act: ends the wait
    _wait_line(target, APP3_ID, r"MO:WAITED", since=cur)
    _wait_line(target, APP3_ID, r"MO:focus=25 min,3", since=cur)
    assert _mo3_lines(target, cur) == [
        "MO:wait=yes,2", "MO:WAITING", "MO:act", "MO:WAITED",
        "MO:focus=25 min,3"], "callbacks ran out of order or twice"
    _quit(target)


APP4 = "menu_option_raise"
APP4_ID = "net.picodeck.menu_option_raise"

FIXTURE4 = PROLOGUE + r"""
sys.addMenuOption("Boom", {"ok", "raise"}, "ok", function(v)
    if v == "raise" then error("MO option boom") end
end)
sys.addMenuItem("Bang", function() error("MO action bang") end)
""" + MAIN_LOOP


def _error_log(target) -> str:
    try:
        return target.read_file("/system/error.log").decode(
            "utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def _wait_error_log(target, before: str, message: str, timeout: float = 10.0):
    """Poll /system/error.log until an entry past `before` holds `message`."""
    deadline = time.monotonic() + timeout
    while True:
        now = _error_log(target)
        new = now[len(before):] if now.startswith(before) else now
        if message in new:
            return new
        if time.monotonic() >= deadline:
            pytest.fail(f"{message!r} not in /system/error.log: {new!r}")
        time.sleep(0.2)


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
    errlog0 = _error_log(target)
    _open(target)
    _keys(target, *keys)
    # The error text in /system/error.log is the evidence that the callback
    # raised: the serial log can lose lines, and a screenshot of the device's
    # error screen shares the port with the log monitor.
    _wait_error_log(target, errlog0, message)
    if target.kind == "hw":
        # The device's error screen waits for Esc; the simulator's test mode
        # returns from it at once.
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
local pc = picocalc
local sys = pc.sys
local function note(s)
    sys.log(s)
    local f = pc.fs.open(pc.fs.appPath("mo.txt"), "a")
    pc.fs.write(f, s .. "\n")
    pc.fs.close(f)
end
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
    note("MO:REFS held=" .. tostring(held) ..
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
    note("MO:UMM before=" .. before .. " after=" .. after)
end
"""


def _probe(target, mode, pattern):
    name = f"{APP5}_{mode}"
    target.stage_lua_app(name, FIXTURE5.replace("@MODE@", mode),
                         id=f"{APP5_ID}_{mode}")
    app_id = f"{APP5_ID}_{mode}"
    target.delete_file(_mo_path(app_id))
    r = target.launch_app(name)
    assert r["launched"], r
    line = _wait_line(target, app_id, pattern, timeout=30)
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
