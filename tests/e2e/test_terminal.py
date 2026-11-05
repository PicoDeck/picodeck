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


# ── The doc's complete example, verbatim ─────────────────────────────────────

# ── renderDirty and the two framebuffers ─────────────────────────────────────
