"""Tests for terminal buffer dump via simulator RPC.

These tests launch a Lua app that creates a terminal, writes known text,
then verify the buffer contents match via get_terminal_buffer.
"""

import re
import time

import pytest

from helpers import PROJECT_ROOT, stage_lua_app


class TestTerminalBuffer:
    """Test terminal buffer dump functionality."""

    def test_terminal_buffer_returns_text(self, simulator):
        """Test that get_terminal_buffer returns written text."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        assert buf["cols"] == 53
        assert buf["rows"] == 26
        assert len(buf["lines"]) == 26

        # First line should contain "Hello, World!"
        assert buf["lines"][0] == "Hello, World!"
        # Second line should contain "Line 2 here"
        assert buf["lines"][1] == "Line 2 here"

    def test_terminal_cursor_position(self, simulator):
        """Test that cursor position is reported correctly."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        # After writing "Line 2 here" (no trailing newline), cursor at end of that text
        # "Line 2 here" is 11 chars, on row 1 (row 0 = "Hello, World!")
        assert buf["cursor_x"] == 11
        assert buf["cursor_y"] == 1

    def test_empty_lines_are_trimmed(self, simulator):
        """Test that trailing spaces on empty lines are trimmed."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        # Lines after the text should be empty strings (trimmed)
        for i in range(2, 26):
            assert buf["lines"][i] == "", f"Line {i} should be empty, got: '{buf['lines'][i]}'"

    def test_no_terminal_returns_error(self, simulator):
        """Test that querying without an active terminal gives an error."""
        # Don't launch any app — no terminal active
        with pytest.raises(RuntimeError, match="no active terminal"):
            simulator.get_terminal_buffer()


# ── Where a terminal draws ───────────────────────────────────────────────────

PROBE = r"""
local d, term_new = picocalc.display, picocalc.terminal.new
local log = picocalc.sys.log
local function extent()       -- lit rows of the frame drawn so far
    local top, bottom
    for y = 0, 319 do
        for x = 0, 319 do
            if d.getPixel(x, y) ~= 0 then top = top or y; bottom = y; break end
        end
    end
    return top, bottom
end
local function report(name)
    local t, b = extent()
    log(string.format("EXT %s %s %s", name, tostring(t), tostring(b)))
    d.clear(0)
end
d.clear(0)
{body}
"""


def extents(sim, sd, body):
    stage_lua_app(sd, "term_probe", PROBE.replace("{body}", body))
    sim.launch_app("term_probe")
    out = sim.wait_for_exit(timeout=15)
    assert out.get("result") == "returned", out
    got = {}
    for entry in sim.get_log_lines(0):
        m = re.search(r"EXT (\S+) (\S+) (\S+)", entry["text"])
        if m:
            got[m.group(1)] = (m.group(2), m.group(3))
    return got


def fill(rows):
    """Fill every row of a terminal called t, with no cursor."""
    return ("t:setCursorVisible(false)\n"
            f"for i = 1, {rows} do t:write('####' .. (i < {rows} and '\\n' or '')) end\n")


@pytest.mark.sd(fixtures=[])
def test_a_terminal_fits_its_rows_on_the_panel(simulator, test_sd_card):
    """The default bounds are y 28..302, 22 rows: a taller terminal lost its
    bottom rows, the newest output and the cursor among them."""
    body = (
        "local t = term_new()\n" + fill(26) + "t:render(); report('default')\n"
        "t = term_new(53, 26)\n" + fill(26) + "t:render(); report('r26')\n"
        "t = term_new(53, 25)\n" + fill(25) + "t:render(); report('r25')\n"
        "t = term_new(53, 23)\n" + fill(23) + "t:render(); report('r23')\n"
        "t = term_new(53, 22)\n" + fill(22) + "t:render(); report('r22')\n"
        "t = term_new(10, 5)\n" + fill(5) + "t:render(); report('r5')\n")
    got = extents(simulator, test_sd_card, body)
    def top_bottom(name):
        return int(got[name][0]), int(got[name][1])
    # 12 px cells. A tall terminal moves up just far enough, and its last row shows...
    for name, rows in (("default", 26), ("r26", 26), ("r25", 25), ("r23", 23)):
        top, bottom = top_bottom(name)
        height = rows * 12
        y0 = min(28, 320 - height)
        assert y0 <= top < y0 + 12, (name, got[name])
        assert y0 + height - 12 <= bottom < y0 + height, (name, got[name])
    # ...and one that always fitted stays exactly where it was, under y 28.
    assert 28 <= top_bottom("r22")[0] < 28 + 12
    assert top_bottom("r22")[1] < 302
    assert 28 <= top_bottom("r5")[0] < 28 + 12
    assert top_bottom("r5")[1] < 28 + 5 * 12


@pytest.mark.sd(fixtures=[])
def test_set_render_bounds_still_wins(simulator, test_sd_card):
    body = (
        "local t = term_new()\n" + fill(26) +
        "t:setRenderBounds(21, 302); t:render(); report('app')\n")
    got = extents(simulator, test_sd_card, body)
    assert 21 <= int(got["app"][0]) < 21 + 12
    assert int(got["app"][1]) < 302


@pytest.mark.sd(fixtures=[])
def test_every_draw_path_stops_at_the_bounds_end(simulator, test_sd_card):
    """The end of setRenderBounds held for render() only: the partial redraw,
    the gutter, the cursor and the scrollback view ran down to the panel's end."""
    body = r"""
local function new()
    local t = term_new(40, 26, 100)
    t:setRenderBounds(16, 60)
    t:setCursorVisible(false)
    t:setCursorBlink(false)
    return t
end

local t = new()
t:render()                       -- clean, then one dirty row below the end
t:setCell(0, 20, "#"); t:setCell(0, 1, "#")
t:renderDirty(); report("dirty")

t = new(); t:setLineNumbers(true); t:render(); report("gutter")

t = new(); t:render(); t:setLineNumbers(true)
t:setCell(0, 1, "#"); t:renderDirty(); report("gutter_dirty")

t = new(); t:setCursorVisible(true); t:setCursor(0, 20); t:render(); report("cursor")

t = new(); t:render(); t:setCursorVisible(true); t:setCursor(0, 20)
t:setCell(0, 1, "#"); t:renderDirty(); report("cursor_dirty")

t = new()
for i = 1, 40 do t:write("#\n") end
t:setScrollbackOffset(5); t:render(); report("scrollback")

t = new()
for i = 1, 40 do t:write("#\n") end
t:setScrollbackOffset(5); t:renderDirty(); report("scrollback_dirty")
"""
    got = extents(simulator, test_sd_card, body)
    for name in ("dirty", "gutter", "gutter_dirty", "cursor", "cursor_dirty",
                 "scrollback", "scrollback_dirty"):
        assert name in got, got
        assert got[name][1] == "nil" or int(got[name][1]) < 60, (name, got[name])
    assert got["dirty"][0] != "nil" and got["gutter"][0] != "nil"


@pytest.mark.sd(fixtures=[])
def test_a_dirty_row_below_the_end_still_comes_clean(simulator, test_sd_card):
    code = r"""
local t = picocalc.terminal.new(40, 26, 100)
t:setRenderBounds(16, 60)
t:setCursorVisible(false)
t:render()
t:setCell(0, 20, "#")
t:renderDirty()
local a, b = t:getDirtyRange()
picocalc.sys.log("RANGE " .. a .. " " .. b)
"""
    stage_lua_app(test_sd_card, "term_clean", code)
    simulator.launch_app("term_clean")
    simulator.wait_for_exit(timeout=10)
    assert any("RANGE -1 -1" in e["text"] for e in simulator.get_log_lines(0))


@pytest.mark.sd(fixtures=[])
def test_a_full_width_terminal_shows_its_last_column(simulator, test_sd_card):
    """x starts at 4, so column 53 of 53 ended at pixel 322: 2 px off the panel."""
    code = r"""
local d = picocalc.display
d.clear(0)
local t = picocalc.terminal.new()
t:setCursorVisible(false)
t:setCell(51, 0, "#"); t:setCell(52, 0, "#")
t:render()
local function column_cells()
    -- The two cells' pixels, left to right, by where the glyphs land.
    local lit = {}
    for x = 300, 319 do
        local n = 0
        for y = 0, 40 do
            if d.getPixel(x, y) ~= 0 then n = n + 1 end
        end
        lit[#lit + 1] = n
    end
    return table.concat(lit, " ")
end
picocalc.sys.log("COLS " .. column_cells())
"""
    stage_lua_app(test_sd_card, "term_cols", code)
    simulator.launch_app("term_cols")
    simulator.wait_for_exit(timeout=10)
    line = next(e["text"] for e in simulator.get_log_lines(0) if "COLS " in e["text"])
    lit = [int(v) for v in line.split("COLS ")[1].split()]
    # Find where the first glyph starts: the second is the same 6 px later.
    first = next(i for i, v in enumerate(lit) if v)
    assert lit[first:first + 6] == lit[first + 6:first + 12], lit


# ── The doc's complete example, verbatim ─────────────────────────────────────

def doc_block(page, heading):
    """The first ```lua block under `heading` in docs/<page>."""
    text = (PROJECT_ROOT / "docs" / page).read_text()
    section = text.split(heading, 1)[1]
    return section.split("```lua\n", 1)[1].split("```", 1)[0]


# ── renderDirty and the two framebuffers ─────────────────────────────────────

def lit(img, y0, y1):
    return sum(1 for y in range(y0, y1) for x in range(320) if img.getpixel((x, y)) != (0, 0, 0))


@pytest.mark.sd(fixtures=[])
def test_render_dirty_with_flush_shows_a_whole_frame(simulator, test_sd_card):
    """flush() swaps the buffers without copying, so rows drawn before the swap
    are not in the new back buffer: a renderDirty + flush loop (apps/ssh) showed
    two incomplete frames in turn."""
    code = r"""
local d, sys = picocalc.display, picocalc.sys
d.clear(0); d.flush(); d.clear(0); d.flush()
local t = picocalc.terminal.new(20, 5, 10)
t:setCursorVisible(false)
t:write("first\n"); t:render(); d.flush()
t:write("second\n"); t:renderDirty(); d.flush()
sys.log("FRAME 0"); sys.sleep(250)
t:renderDirty(); d.flush()
sys.log("FRAME 1"); sys.sleep(250)
t:renderDirty(); d.flush()
sys.log("FRAME 2"); sys.sleep(250)
"""
    stage_lua_app(test_sd_card, "term_swap", code)
    simulator.launch_app("term_swap")
    for frame in ("FRAME 0", "FRAME 1", "FRAME 2"):
        simulator.wait_for_log(frame, timeout=5)
        time.sleep(0.1)
        img = simulator.screenshot_pil().convert("RGB")
        # 5 rows from y 28, 12 px each.
        assert lit(img, 28, 40) > 0 and lit(img, 40, 52) > 0, frame
    simulator.wait_for_exit(timeout=5)


@pytest.mark.sd(fixtures=[])
def test_render_dirty_stays_partial_without_a_swap(simulator, test_sd_card):
    """With flushRows, which does not swap, only the dirty rows are redrawn, as
    the page says: a marker drawn over a clean row survives."""
    code = r"""
local d = picocalc.display
d.clear(0)
local t = picocalc.terminal.new(20, 5, 10)
t:setRenderBounds(0, 60)
t:setCursorVisible(false)
t:write("a\nb\nc")
t:render()
d.fillRect(100, 2, 4, 4, 0xF800)    -- row 0, past its text
t:setCell(0, 3, "#")
local first, last = t:getDirtyRange()
t:renderDirty()
local new = 0
for y = 36, 47 do for x = 4, 9 do
    if d.getPixel(x, y) ~= 0 then new = new + 1 end
end end
picocalc.sys.log(string.format("DIRTY %d %d MARK %d NEW %d", first, last,
                 d.getPixel(101, 3), new))
"""
    stage_lua_app(test_sd_card, "term_partial", code)
    simulator.launch_app("term_partial")
    simulator.wait_for_exit(timeout=5)
    line = next(e["text"] for e in simulator.get_log_lines(0) if "DIRTY " in e["text"])
    assert "DIRTY 3 3" in line and "MARK 63488" in line and "NEW 0" not in line, line


def test_doc_partial_redraw_example(simulator):
    """The page's renderDirty + flushRows loop, verbatim: the frame stays whole
    across many partial presents."""
    block = doc_block("API-Terminal.md", "#### `term:renderDirty()`")
    sd = simulator.sd_card_path
    code = block + "\n"
    stage_lua_app(sd, "term_rows", code)
    simulator.launch_app("term_rows")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    time.sleep(0.05)
    img = simulator.screenshot_pil().convert("RGB")
    # Header row and the last line both on the panel.
    assert lit(img, 0, 12) > 0
    assert lit(img, 12 * 9, 12 * 10) > 0
