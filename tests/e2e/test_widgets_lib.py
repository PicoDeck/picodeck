"""system/lib/widgets.lua, against what docs/Library-Widgets.md says."""

import time

import pytest

from helpers import PROJECT_ROOT, stage_lua_app


def doc_block(heading):
    text = (PROJECT_ROOT / "docs" / "Library-Widgets.md").read_text()
    return text.split(heading, 1)[1].split("```lua\n", 1)[1].split("```", 1)[0]


PRELUDE = r"""
local W = picocalc.sys.loadlib("widgets")
local input, d, ui = picocalc.input, picocalc.display, picocalc.ui
local log = picocalc.sys.log
local ENTER, UP, DOWN, LEFT, RIGHT, ESC = input.BTN_ENTER, input.BTN_UP, input.BTN_DOWN,
    input.BTN_LEFT, input.BTN_RIGHT, input.BTN_ESC
local TAB, BS, DEL, CTRL, SHIFT = input.BTN_TAB, input.BTN_BACKSPACE, input.BTN_DEL,
    input.BTN_CTRL, input.BTN_SHIFT
local function check(name, got, want)
    if got ~= want then
        error(name .. ": got " .. tostring(got) .. ", want " .. tostring(want), 2)
    end
end
"""


def run_lua(sim, sd, body, name="w_app"):
    stage_lua_app(sd, name, PRELUDE + body + '\nlog("W:DONE")\n')
    sim.launch_app(name)
    out = sim.wait_for_exit(timeout=20)
    assert out.get("result") == "returned", out
    assert any("W:DONE" in e["text"] for e in sim.get_log_lines(0))


CASES = {
    "no_globals": r"""
check("draw_rect", display_draw_rect, nil)
check("fill_rect", display_fill_rect, nil)
""",
    "every_constructor_takes_an_empty_table": r"""
d.clear(0)
for _, make in ipairs({ W.Label, W.Button, W.Divider, W.ProgressBar, W.Checkbox, W.RadioGroup,
                        W.TextField, W.TextArea, W.Select, W.ListView, W.Panel, W.Loading }) do
    local w = make({})
    w:draw()
end
W.Select({}):drawDropdown()
check("no_opts", pcall(W.Label), false)
""",
    "defaults": r"""
check("button w", W.Button({}).w, 80)
check("button label", W.Button({}).label, "Button")
check("divider w", W.Divider({}).w, 300)
check("bar w", W.ProgressBar({}).w, 200)
check("bar h", W.ProgressBar({}).h, 12)
check("radio spacing", W.RadioGroup({}).spacing, 13)
check("radio selected", W.RadioGroup({}).selected, 1)
check("field w", W.TextField({}).w, 200)
check("field maxlen", W.TextField({}).maxlen, 256)
check("area w", W.TextArea({}).w, 300)
check("area h", W.TextArea({}).h, 100)
check("area wrap", W.TextArea({}).wordWrap, true)
check("area multiline", W.TextArea({}).multiline, true)
check("area maxlen", W.TextArea({}).maxlen, 4096)
check("select w", W.Select({}).w, 200)
check("select visible", W.Select({}).maxVisible, 6)
check("list w", W.ListView({}).w, 300)
check("list h", W.ListView({}).h, 200)
check("panel w", W.Panel({}).w, 310)
check("panel h", W.Panel({}).h, 260)
check("loading", W.Loading({}).text, "Loading...")
""",
    "label_has_no_box": r"""
d.clear(0)
local p = W.Panel({ x = 0, y = 0, w = 200, h = 100 })
p:draw()
local panel = d.getPixel(50, 50)
check("panel not black", panel ~= 0, true)
W.Label({ x = 20, y = 40, text = "M" }):draw()
W.Checkbox({ x = 20, y = 60, label = "M" }):draw()
W.RadioGroup({ x = 60, y = 60, options = { "M" } }):draw()
-- Corners of the three text cells stay the panel's colour.
check("label corner", d.getPixel(25, 47), panel)
check("check corner", d.getPixel(39, 68), panel)
check("radio corner", d.getPixel(79, 68), panel)
""",
    "label_align": r"""
local function first_lit(l)
    d.clear(0); l:draw()
    for x = 0, 319 do if d.getPixel(x, 2) ~= 0 or d.getPixel(x, 3) ~= 0 then return x end end
end
local left = first_lit(W.Label({ x = 10, y = 0, w = 100, text = "MM" }))
local mid = first_lit(W.Label({ x = 10, y = 0, w = 100, text = "MM", align = "center" }))
local right = first_lit(W.Label({ x = 10, y = 0, w = 100, text = "MM", align = "right" }))
check("order", left < mid and mid < right, true)
""",
    "button": r"""
local n = 0
local b = W.Button({ onPress = function() n = n + 1 end })
check("enter", b:handleInput(ENTER), true); check("called", n, 1)
check("other", b:handleInput(UP), false); check("not called", n, 1)
""",
    "checkbox": r"""
local seen
local c = W.Checkbox({ checked = true, onChange = function(v) seen = v end })
check("start", c:isChecked(), true)
c:handleInput(ENTER); check("toggled", c:isChecked(), false); check("cb", seen, false)
seen = "untouched"
c:setChecked(true); check("set", c:isChecked(), true); check("set calls nothing", seen, "untouched")
""",
    "radio": r"""
local i, t
local r = W.RadioGroup({ options = { "a", "b", "c" }, selected = 2,
                         onChange = function(a, b) i, t = a, b end })
check("get", select(2, r:getSelected()), "b")
r:handleInput(DOWN); r:handleInput(DOWN)            -- wraps to a
check("no change yet", i, nil)
r:handleInput(ENTER)
check("index", i, 1); check("text", t, "a"); check("sel", (r:getSelected()), 1)
r:handleInput(UP)
r:handleInput(ENTER)
check("wrap up", i, 3)
""",
    "text_field": r"""
local changed, submitted
local f = W.TextField({ maxlen = 5, onChange = function(t) changed = t end,
                        onSubmit = function(t) submitted = t end })
for c in ("hello!"):gmatch(".") do f:handleInput(0, c) end
check("maxlen", f:getText(), "hello"); check("cb", changed, "hello")
f:handleInput(BS); check("bs", f:getText(), "hell")
f:handleInput(LEFT); f:handleInput(LEFT); f:handleInput(DEL); check("del", f:getText(), "hel")
f:handleInput(0, "\n"); f:handleInput(0, "\t"); check("control ignored", f:getText(), "hel")
f:handleInput(ENTER); check("submit", submitted, "hel")
f:setText("two words here"); f:setCursor(0)
f:handleInput(CTRL | RIGHT); check("word", f.cursor, 4)
f:handleInput(CTRL | input.BTN_F2); check("end", f.cursor, #f:getText())
f:handleInput(CTRL | input.BTN_F1); check("start", f.cursor, 0)
changed = "untouched"; f:setText("x"); check("setText silent", changed, "untouched")
-- A narrow field scrolls and keeps the cursor in view.
local n = W.TextField({ w = 20 })
for c in ("abcdef"):gmatch(".") do n:handleInput(0, c) end
check("scroll", n.scroll > 0, true)
n:draw()
""",
    "text_area": r"""
local submitted
local a = W.TextArea({ w = 60, h = 30, text = "" })
for c in ("ab"):gmatch(".") do a:handleInput(0, c) end
a:handleInput(ENTER)
for c in ("cd"):gmatch(".") do a:handleInput(0, c) end
check("newline", a:getText(), "ab\ncd")
a:handleInput(UP); a:handleInput(LEFT); a:handleInput(BS)
check("edit", a:getText(), "b\ncd")
a:draw()
local one = W.TextArea({ multiline = false, onSubmit = function(t) submitted = t end })
one:handleInput(0, "x"); one:handleInput(ENTER)
check("submit", submitted, "x"); check("no newline", one:getText(), "x")
-- (h - 6) / 8 rows of (w - 12) / 6 characters.
local long = W.TextArea({ w = 72, h = 38, text = ("word "):rep(40) })
check("rows", long:_visibleRows(), 4)
check("cols", long:_maxCols(), 10)
long:draw()
local plain = W.TextArea({ w = 72, wordWrap = false, text = ("word "):rep(40) })
check("no wrap", plain._num_segs, 1)
-- Ctrl+Up/Down page the view, not the cursor.
local cur = long.cursor
long:handleInput(CTRL | UP)
check("page keeps cursor", long.cursor, cur)
""",
    "select": r"""
local i, t
local s = W.Select({ options = { "Red", "Green", "Blue" }, onChange = function(a, b) i, t = a, b end })
d.clear(0); s:draw(); s:drawDropdown()
check("closed", s:isExpanded(), false)
check("up ignored", s:handleInput(UP), false)
check("enter opens", s:handleInput(ENTER), true); check("open", s:isExpanded(), true)
check("esc closes", s:handleInput(ESC), true); check("closed", s:isExpanded(), false)
check("no choice", i, nil)
s:handleInput(ENTER); s:handleInput(0, "G"); s:drawDropdown()
check("filtered", #s.filtered, 1); check("case", s.filtered[1].text, "Green")
s:handleInput(BS); check("unfiltered", #s.filtered, 3)
s:handleInput(DOWN); s:handleInput(ENTER)
check("chosen", i, 2); check("text", t, "Green"); check("closed", s:isExpanded(), false)
check("get", select(2, s:getSelected()), "Green")
s:handleInput(ENTER); s:setFocused(false); check("focus closes", s:isExpanded(), false)
s:setOptions({ "One" }); check("first", (s:getSelected()), 1)
-- The open list follows the highlight when it wraps, both ways.
local many = W.Select({ options = { "a", "b", "c", "d", "e" }, maxVisible = 2 })
many:handleInput(ENTER)
many:handleInput(UP); check("wrap up shows the last", many.scroll, 3)
many:handleInput(DOWN); check("wrap down shows the first", many.scroll, 0)
-- Near the bottom of the screen the list opens upward.
local low = W.Select({ y = 300, options = { "a", "b" } })
low:handleInput(ENTER); low:drawDropdown()
""",
    "list_view": r"""
local moved, hit
local l = W.ListView({ h = 41, items = { "a", "b", "c", "d", "e" },
                       onSelect = function(i, v) moved = v end,
                       onActivate = function(i, v) hit = v end })
check("rows", l:_visibleCount(), 3)
l:handleInput(DOWN); check("moved", moved, "b")
l:handleInput(ENTER); check("activated", hit, "b")
l:handleInput(UP); l:handleInput(UP); check("wrap", moved, "e"); check("scrolled", l.scroll, 2)
l:draw()
l:handleInput(DOWN); check("wrap down", moved, "a"); check("scrolled back", l.scroll, 0)
l:setItems({ "x" }); check("reset", (l:getSelected()), 1)
""",
    "focus_group": r"""
local label = W.Label({ text = "skip" })
local a, b = W.Checkbox({}), W.TextField({})
local g = W.FocusGroup({ label, a, b })
check("first focusable", g:getFocused(), a); check("index", g:getFocusIndex(), 2)
check("a focused", a.focused, true)
check("tab", g:handleInput(TAB), true); check("next", g:getFocused(), b)
check("a off", a.focused, false); check("b on", b.focused, true)
g:handleInput(TAB); check("wraps past label", g:getFocused(), a)
g:handleInput(TAB | SHIFT); check("back", g:getFocused(), b)
g:handleInput(0, "x"); check("delegates", b:getText(), "x")
g:setFocus(2); check("setFocus", g:getFocused(), a)
check("unused", g:handleInput(UP), false)
check("empty", W.FocusGroup({}):getFocused(), nil)
""",
    "toasts": r"""
d.clear(0)
W.toast("first"); W.toast("second", 5)
W.drawToasts()
-- Both showing: 18 px apart, the newest lowest.
check("newest row", d.getPixel(160, 285) ~= 0, true)
check("older row", d.getPixel(160, 267) ~= 0, true)
""",
    "loading": r"""
d.clear(0xFFFF)
local l = W.Loading({ text = "Wait", subtext = "a moment" })
l:draw(); check("cleared", d.getPixel(0, 0), 0)
local before = l.frame; l:draw(); check("advances", l.frame, before + 1)
l:setProgress(0.5); l:setText("x"); d.clear(0); l:draw()
check("bar", d.getPixel(62, 142) ~= 0, true)
""",
    "theme_reads": r"""
W.theme.text = 0xF800
local l = W.Label({ text = "x" })
check("default read at make", l.color, 0xF800)
""",
}


@pytest.mark.sd(fixtures=[])
@pytest.mark.parametrize("case", sorted(CASES))
def test_widgets_behave_as_documented(simulator, test_sd_card, case):
    run_lua(simulator, test_sd_card, CASES[case])


def _pressed(sim, key):
    seq = sim.keypress(key)["input_seq"]
    sim.wait_input_consumed(seq)
    time.sleep(0.12)


@pytest.mark.sd(fixtures=[])
def test_doc_quick_start_verbatim(simulator, test_sd_card):
    stage_lua_app(test_sd_card, "w_doc", doc_block("## Quick start"))
    simulator.launch_app("w_doc")
    simulator.wait_frames(2)
    start = simulator.screenshot_pil().convert("RGB")
    for ch in "Ann":
        _pressed(simulator, ch)
    typed = simulator.screenshot_pil().convert("RGB")
    assert typed != start, "typing did not reach the focused field"
    # Tab through: sound, level, theme, list, notes, save, quit.
    _pressed(simulator, "tab")
    _pressed(simulator, "enter")                 # sound off
    _pressed(simulator, "tab")
    _pressed(simulator, "down")
    _pressed(simulator, "enter")                 # level: Hard
    _pressed(simulator, "tab")
    _pressed(simulator, "enter")                 # theme opens
    opened = simulator.screenshot_pil().convert("RGB")
    _pressed(simulator, "esc")                   # the dropdown takes it; the app runs on
    assert simulator.call("get_running_app").get("running")
    closed = simulator.screenshot_pil().convert("RGB")
    assert opened != closed
    for _ in range(3):
        _pressed(simulator, "tab")               # list, notes, save
    _pressed(simulator, "enter")                 # Save: a toast
    simulator.wait_frames(2)
    _pressed(simulator, "tab")
    simulator.keypress("enter")                  # Quit
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out


@pytest.mark.sd(fixtures=[])
def test_doc_poll_event_snippet(simulator, test_sd_card):
    """The page's slow-frames snippet, verbatim, inside a loop."""
    snippet = doc_block("If your frames are slow")
    code = PRELUDE + r"""
local field = W.TextField({})
local group = W.FocusGroup({ field })
local t0 = picocalc.sys.getTimeMs()
log("READY")
while picocalc.sys.getTimeMs() - t0 < 1500 do
    input.update()
""" + snippet + r"""
    picocalc.sys.sleep(100)
end
log("TEXT " .. field:getText())
"""
    stage_lua_app(test_sd_card, "w_poll", code)
    simulator.launch_app("w_poll")
    simulator.wait_for_log("READY", timeout=10)
    for ch in "abcde":            # 15 events: a key is a down, a char and an up
        simulator.keypress(ch)
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    text = next(e["text"] for e in simulator.get_log_lines(0) if "TEXT " in e["text"])
    assert text.endswith("abcde"), text
