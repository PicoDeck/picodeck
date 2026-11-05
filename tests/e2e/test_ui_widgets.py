"""picocalc.ui: the draw* widgets' argument edges, modal dialogs and wrapText.

A widget must never take the OS down: a layout that collapses to a narrow width,
or a bad scroll offset, is a Lua-side mistake that may draw nothing or nonsense
but must not reach memcpy with a negative length (which pcall cannot catch, and
which is a stack smash on the device).
"""

import time

import pytest

from helpers import stage_lua_app

UI = "local ui, d = picocalc.ui, picocalc.display\n"


def run_ok(sim, sd, name, code):
    """Stage and run `code`; it must return normally."""
    stage_lua_app(sd, name, UI + code)
    sim.launch_app(name)
    out = sim.wait_for_exit(timeout=20)
    assert out.get("result") == "returned", out
    assert sim.is_alive()
    return out


def log_lines(sim, since=0):
    return [e["text"] for e in sim.get_log_lines(since)]


# ── Narrow widths and bad offsets ─────────────────────────────────────────────

WIDTHS = "for _, w in ipairs({{-100, -5, -1, 0, 1, 2, 3, 7, 8, 9}}) do\n{body}\nend\n"

EDGE_CASES = {
    # The confirmed crash: w <= 2 with text made max_chars negative.
    "list_item_narrow": WIDTHS.format(body="ui.drawListItem(1, 1, w, 'text', true, true)"),
    "text_field_narrow": WIDTHS.format(
        body="ui.drawTextField(1, 1, w, 'text', 0, 0, true, true)"),
    # Siblings of the same arithmetic.
    "text_area_narrow": WIDTHS.format(
        body="ui.drawTextArea(1, 1, w, w, 'text', {0}, 0, 0, 0, true, true)"),
    "panel_narrow": WIDTHS.format(
        body="ui.drawPanel(1, 1, w, w, 'title'); ui.drawPanel(1, 1, w, w)"),
    "button_narrow": WIDTHS.format(body="ui.drawButton(1, 1, w, 'label', true, true)"),
    "progress_narrow": WIDTHS.format(body="ui.drawProgress(1, 1, w, w, 0.5)"),
    "divider_narrow": WIDTHS.format(body="ui.drawDivider(1, 1, w)"),
    # A text field's scroll offset is an index into the string.
    "text_field_scroll": (
        "for _, s in ipairs({-100000000, -5, -1, 0, 4, 5, 100000000}) do\n"
        "  ui.drawTextField(1, 1, 200, 'text', 0, s, true, true)\n"
        "end\n"),
    # A text area's segments come from Lua: nothing makes them offsets into text.
    "text_area_segments": (
        "for _, seg in ipairs({ {100000000, 100000010}, {-100000000, -99999990}, {-1, 2},\n"
        "                       {2, 100000000}, {3, 1}, {4, 4} }) do\n"
        "  ui.drawTextArea(1, 1, 200, 100, 'text', seg, 0, 0, 0, true, true)\n"
        "end\n"),
    "text_area_scroll": (
        "for _, s in ipairs({-100000000, -1, 0, 1, 2, 100000000}) do\n"
        "  ui.drawTextArea(1, 1, 200, 100, 'text', {0, 2}, s, 0, 0, true, true)\n"
        "end\n"),
    # The cursor of a text area is only a position, so nothing limits it.
    "text_area_cursor": (
        "for _, c in ipairs({-100000, -1, 0, 1000, 100000}) do\n"
        "  ui.drawTextArea(1, 1, 200, 100, 'text', {0}, 0, c, c, true, true)\n"
        "  ui.drawTextArea(1, 1, 200, 100, 'text', {0}, 0, 0, c, true, true)\n"
        "  ui.drawTextField(1, 1, 200, 'text', c, 0, true, true)\n"
        "end\n"),
    "long_text": (
        "local s = string.rep('x', 2000)\n"
        "ui.drawListItem(1, 1, 300, s, true, true)\n"
        "ui.drawTextField(1, 1, 300, s, 0, 0, true, true)\n"
        "ui.drawTextField(1, 1, 300, s, 0, 1900, true, true)\n"
        "ui.drawTextArea(1, 1, 300, 100, s, {0, 1990}, 0, 0, 0, true, true)\n"
        "ui.drawButton(1, 1, 300, s, true)\n"
        "ui.drawPanel(1, 1, 300, 100, s)\n"
        "ui.drawToast(1, s)\n"
        "ui.drawFooter(s, s)\n"
        "ui.drawHeader(s)\n"),
    "tabs_numeric_labels": (
        # Numbers convert to strings the table does not hold: nothing anchors
        # them, and a GC between two conversions must not leave a dangling name.
        "local tabs = {}\n"
        "for i = 1, 40 do tabs[i] = i * 1000 end\n"
        "for _ = 1, 50 do ui.drawTabs(1, tabs, 1) end\n"),
}


@pytest.mark.sd(fixtures=[])
@pytest.mark.parametrize("case", sorted(EDGE_CASES))
def test_widget_edges_do_not_crash(simulator, test_sd_card, case):
    run_ok(simulator, test_sd_card, "ui_edge", EDGE_CASES[case])


@pytest.mark.sd(fixtures=[])
def test_narrow_widgets_still_draw_their_box(simulator, test_sd_card):
    """Skipping the text must not skip the widget: a 2-wide selected item still
    fills its two columns."""
    code = (
        "d.clear(0)\n"
        "ui.drawListItem(10, 10, 2, 'text', true, false)\n"
        "picocalc.sys.log('LI ' .. d.getPixel(10, 12) .. ' ' .. d.getPixel(11, 12) .. ' ' .. d.getPixel(12, 12))\n")
    run_ok(simulator, test_sd_card, "ui_narrow", code)
    lines = [t for t in log_lines(simulator) if t.startswith("LI ") or " LI " in t]
    assert lines, log_lines(simulator)
    a, b, c = lines[-1].split("LI ")[1].split()
    assert int(a) != 0 and int(b) != 0 and int(c) == 0, lines[-1]


# ── A narrow field keeps its text inside ──────────────────────────────────────

@pytest.mark.sd(fixtures=[])
def test_text_field_scroll_clamps_to_the_text(simulator, test_sd_card):
    """A negative scroll offset reads as 0: the text from its start, not the
    bytes in front of the string."""
    code = (
        "local function row()\n"
        "  local t = {}\n"
        "  for y = 0, 12 do for x = 0, 79 do t[#t + 1] = d.getPixel(x, y) end end\n"
        "  return table.concat(t, ',')\n"
        "end\n"
        "d.clear(0); ui.drawTextField(0, 0, 80, 'hello', 0, 0, false, false)\n"
        "local want = row()\n"
        "d.clear(0); ui.drawTextField(0, 0, 80, 'hello', 0, -3, false, false)\n"
        "picocalc.sys.log('SAME ' .. tostring(row() == want))\n")
    run_ok(simulator, test_sd_card, "ui_scroll", code)
    assert any("SAME true" in t for t in log_lines(simulator)), log_lines(simulator)


# ── Optional arguments take an explicit nil ───────────────────────────────────

@pytest.mark.sd(fixtures=[])
def test_optional_args_accept_nil(simulator, test_sd_card):
    code = (
        "ui.toast('a', nil)\n"
        "ui.drawToast(10, 'b', nil)\n"
        "ui.drawProgress(1, 1, 10, 10, 0.5, nil)\n"
        "ui.drawDivider(1, 1, 10, nil)\n"
        "ui.drawPanel(1, 1, 10, 10, nil)\n"
        "ui.drawFooter(nil, nil)\n"
        "ui.drawSpinner(10, 10, nil, nil)\n"
        "ui.splash(nil, nil)\n"
        "ui.drawTabs(1, {'a'}, 1, nil, nil)\n")
    run_ok(simulator, test_sd_card, "ui_nil", code)


@pytest.mark.sd(fixtures=[])
def test_toast_styles_are_named(simulator, test_sd_card):
    code = (
        "for _, n in ipairs({'TOAST_INFO', 'TOAST_SUCCESS', 'TOAST_WARNING', 'TOAST_ERROR'}) do\n"
        "  picocalc.sys.log(n .. '=' .. tostring(ui[n]))\n"
        "  ui.toast(n, ui[n])\n"
        "end\n"
        "ui.toast('default')\n")
    run_ok(simulator, test_sd_card, "ui_styles", code)
    text = "\n".join(log_lines(simulator))
    for line in ("TOAST_INFO=0", "TOAST_SUCCESS=1", "TOAST_WARNING=2", "TOAST_ERROR=3"):
        assert line in text, text


# ── Sizes ─────────────────────────────────────────────────────────────────────

MEASURE = r"""
local function rows_lit(x)    -- the lit rows of column x, as first and count
    local first, n = nil, 0
    for y = 0, 319 do
        if d.getPixel(x, y) ~= 0 then first = first or y; n = n + 1 end
    end
    return first, n
end
local function cols_lit(y)
    local first, n = nil, 0
    for x = 0, 319 do
        if d.getPixel(x, y) ~= 0 then first = first or x; n = n + 1 end
    end
    return first, n
end
local function check(name, got, want_first, measured)
    picocalc.sys.log(string.format("SZ %s ret=%s first=%s h=%s", name, tostring(got),
                                   tostring(want_first), tostring(measured)))
end

d.clear(0); local h = ui.drawFooter("l", "r"); local f, n = rows_lit(2)
check("footer", h, f, n)

d.clear(0); h = ui.drawButton(20, 40, 100, "B", false, false); f, n = rows_lit(21)
check("button", h, f, n)

d.clear(0); h = ui.drawTextField(20, 40, 100, "T", 0, 0, false, false); f, n = rows_lit(21)
check("textfield", h, f, n)

d.clear(0); h = ui.drawListItem(20, 40, 100, "", true, false); f, n = rows_lit(21)
check("listitem", h, f, n)

d.clear(0); h = ui.drawCheckbox(20, 40, false, false); f, n = rows_lit(20)
check("checkbox", h, f, n)

d.clear(0); h = ui.drawRadio(20, 40, false, false)
local top, bottom
for y = 0, 319 do for x = 0, 319 do
    if d.getPixel(x, y) ~= 0 then top = top or y; bottom = y end
end end
check("radio", h, top, bottom - top + 1)

d.clear(0); h = ui.drawDivider(20, 40, 100); f, n = rows_lit(21)
check("divider", h, f, n)

d.clear(0); h = ui.drawToast(40, "T"); f, n = rows_lit(160)
check("toast", h, f, n)

-- A panel's content area starts below its title bar.
d.clear(0); local cy = ui.drawPanel(20, 40, 100, 100, "Title")
picocalc.sys.log("SZ panel_title cy=" .. tostring(cy))
d.clear(0); cy = ui.drawPanel(20, 40, 100, 100)
picocalc.sys.log("SZ panel_plain cy=" .. tostring(cy))

-- A text area reports the rows it shows.
local text = {}
for i = 1, 30 do text[#text + 1] = "line" .. i end
text = table.concat(text, "\n")
local segs = ui.wrapText(text, 20)
local rows = ui.drawTextArea(20, 40, 100, 100, text, segs, 0, 0, 0, false, false)
picocalc.sys.log("SZ textarea rows=" .. tostring(rows))
"""

SIZES = {"footer": 18, "button": 15, "textfield": 13, "listitem": 13,
         "checkbox": 10, "radio": 11, "divider": 1, "toast": 14}


@pytest.mark.sd(fixtures=[])
def test_widgets_return_the_rows_they_cover(simulator, test_sd_card):
    run_ok(simulator, test_sd_card, "ui_sizes", MEASURE)
    lines = [t.split("SZ ", 1)[1] for t in log_lines(simulator) if "SZ " in t]
    got = {}
    for line in lines:
        parts = line.split()
        got[parts[0]] = dict(p.split("=") for p in parts[1:])
    for name, want in SIZES.items():
        assert got[name]["ret"] == str(want), (name, got[name])
        assert got[name]["h"] == str(want), (name, got[name])
    assert got["footer"]["first"] == "302"
    # title bar: 1 border + 16 bar + 1 divider below y=40
    assert got["panel_title"]["cy"] == str(40 + 18)
    assert got["panel_plain"]["cy"] == str(40 + 1)
    assert got["textarea"]["rows"] == str((100 - 6) // 8)


# ── wrapText ─────────────────────────────────────────────────────────────────

WRAP = r"""
local function show(name, text, cols)
    local segs, rows = ui.wrapText(text, cols)
    picocalc.sys.log("WRAP " .. name .. " rows=" .. rows .. " #=" .. #segs
                     .. " segs=" .. table.concat(segs, ",", 1, math.min(#segs, 8)))
end
show("newlines", "a\nb\nc", 10)
show("trailing", "a\n", 10)
show("blank", "a\n\nb", 10)
show("words", ("abcdef "):rep(40), 10)
show("long_nl", ("x"):rep(25) .. "\n" .. ("y"):rep(5), 10)
show("many", ("ab\n"):rep(600), 10)
show("empty", "", 10)
show("badcols", "abc", 0)
"""


@pytest.mark.sd(fixtures=[])
def test_wraptext_keeps_every_row_and_breaks_at_newlines(simulator, test_sd_card):
    run_ok(simulator, test_sd_card, "ui_wrap", WRAP)
    got = {}
    for t in log_lines(simulator):
        if "WRAP " in t:
            parts = t.split("WRAP ", 1)[1].split()
            got[parts[0]] = dict(p.split("=", 1) for p in parts[1:])
    assert got["newlines"]["segs"] == "0,2,4" and got["newlines"]["rows"] == "3"
    assert got["trailing"]["segs"] == "0,2" and got["trailing"]["rows"] == "2"
    assert got["blank"]["segs"] == "0,2,3" and got["blank"]["rows"] == "3"
    assert got["words"]["rows"] == "40"                # was 30: rows were dropped
    assert got["long_nl"]["segs"] == "0,10,20,26"
    assert got["many"]["rows"] == "601", got["many"]   # no 512-row cap
    assert got["many"]["#"] == "601"
    assert got["empty"]["segs"] == "0" and got["empty"]["rows"] == "1"
    assert got["badcols"]["segs"] == "0" and got["badcols"]["rows"] == "1"


# ── Modal dialogs ─────────────────────────────────────────────────────────────

# Blue under a clip rect in the top-left corner: everything else is black, so a
# dialog outside the clip shows only if the OS lifted the clip.
CLIP_MODAL = r"""
d.clear(0)
d.setClipRect(0, 0, 40, 40)
d.fillRect(0, 0, 320, 320, 0x001F)
d.flush()
picocalc.sys.log("MODAL:CALL")
local r = ui.{call}
local x, y, w, h = d.getClipRect()
picocalc.sys.log(string.format("MODAL:RET %s clip=%d,%d,%d,%d", tostring(r), x, y, w, h))
"""


def dialog_visible(sim, x=160, y=160, timeout=3.0):
    """Whether the dialog's box colour comes up at the screen's centre."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sim.screenshot_pil().convert("RGB").getpixel((x, y)) != (0, 0, 0):
            return True
        time.sleep(0.05)
    return False


@pytest.mark.sd(fixtures=[])
def test_text_input_ignores_the_apps_clip_rect(simulator, test_sd_card):
    """ui.confirm lifts the clip rect and puts it back; ui.textInput did not, so
    the dialog was invisible while the app blocked on it."""
    stage_lua_app(test_sd_card, "ui_clip",
                  UI + CLIP_MODAL.replace("{call}", 'textInput("Name:", "abc")'))
    simulator.launch_app("ui_clip")
    simulator.wait_for_log("MODAL:CALL", timeout=10)
    assert dialog_visible(simulator), "text input drew nothing outside the clip"
    seq = simulator.keypress("esc")["input_seq"]
    simulator.wait_input_consumed(seq)
    simulator.wait_for_log("MODAL:RET", timeout=10)
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    assert any("clip=0,0,40,40" in t for t in log_lines(simulator)), log_lines(simulator)


@pytest.mark.sd(fixtures=[])
def test_text_input_keeps_127_characters(simulator, test_sd_card):
    stage_lua_app(test_sd_card, "ui_len",
                  UI + 'picocalc.sys.log("LEN " .. #ui.textInput("p", string.rep("x", 400)))\n')
    simulator.launch_app("ui_len")
    assert dialog_visible(simulator)
    simulator.keypress("\n")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    assert any("LEN 127" in t for t in log_lines(simulator)), log_lines(simulator)


LONG = "Really delete it? " * 20   # 360 chars: more than the box's two lines


def text_rows(img, x0, x1, fg=(255, 255, 255)):
    """Rows holding at least one pixel of colour `fg`, between columns x0..x1."""
    px = img.load()
    return [y for y in range(320) if any(px[x, y] == fg for x in range(x0, x1))]


@pytest.mark.sd(fixtures=[])
def test_confirm_box_fits_a_long_message(simulator, test_sd_card):
    """The box was sized for at most two lines, so a longer message ran past its
    bottom edge and over the Enter/Esc hint."""
    code = f'd.clear(0x07E0)\nd.flush()\nlocal r = ui.confirm("{LONG}")\npicocalc.sys.log("OK")\n'
    stage_lua_app(test_sd_card, "ui_confirm", UI + code)
    simulator.launch_app("ui_confirm")
    assert dialog_visible(simulator)
    img = simulator.screenshot_pil().convert("RGB")
    # DLG_X = 20, DLG_W = 280: the white text sits inside the box's columns.
    rows = text_rows(img, 20, 300)
    assert rows, "no message text"
    # The box: the dialog colour, down a column inside its left edge.
    px = img.load()
    box = [y for y in range(320) if px[24, y] == px[24, 160]]
    assert rows[0] > box[0] and rows[-1] < box[-1], (rows[0], rows[-1], box[0], box[-1])
    # The hint's own rows are not white, and the last message line stays above them.
    hint_top = box[-1] - 12
    assert rows[-1] < hint_top, (rows[-1], hint_top)
    # Seven lines of text at least: it is not cut off at two.
    assert rows[-1] - rows[0] > 6 * 12, (rows[0], rows[-1])
    simulator.keypress("esc")


@pytest.mark.sd(fixtures=[])
def test_confirm_short_message_keeps_its_box(simulator, test_sd_card):
    """One- and two-line dialogs are the size they always were."""
    code = 'd.clear(0x07E0)\nd.flush()\nui.confirm("Delete this file?")\n'
    stage_lua_app(test_sd_card, "ui_confirm", UI + code)
    simulator.launch_app("ui_confirm")
    assert dialog_visible(simulator)
    img = simulator.screenshot_pil().convert("RGB")
    px = img.load()
    box = [y for y in range(320) if px[24, y] == px[24, 160]]
    assert box[-1] - box[0] + 1 == 44 + 12 + 18 - 2, (box[0], box[-1])
    simulator.keypress("esc")


@pytest.mark.sd(fixtures=[])
def test_confirm_breaks_at_newlines(simulator, test_sd_card):
    code = 'd.clear(0x07E0)\nd.flush()\nui.confirm("one\\ntwo")\n'
    stage_lua_app(test_sd_card, "ui_confirm", UI + code)
    simulator.launch_app("ui_confirm")
    assert dialog_visible(simulator)
    img = simulator.screenshot_pil().convert("RGB")
    px = img.load()
    box = [y for y in range(320) if px[24, y] == px[24, 160]]
    assert box[-1] - box[0] + 1 == 44 + 2 * 12 + 18 - 2, (box[0], box[-1])
    simulator.keypress("esc")


@pytest.mark.sd(fixtures=[])
def test_confirm_message_stops_at_twenty_lines(simulator, test_sd_card):
    """The tallest box still fits on the panel."""
    code = 'd.clear(0x07E0)\nd.flush()\nui.confirm(string.rep("x\\n", 200))\n'
    stage_lua_app(test_sd_card, "ui_confirm", UI + code)
    simulator.launch_app("ui_confirm")
    assert dialog_visible(simulator)
    img = simulator.screenshot_pil().convert("RGB")
    px = img.load()
    box = [y for y in range(320) if px[24, y] == px[24, 160]]
    assert box[-1] - box[0] + 1 == 44 + 20 * 12 + 18 - 2, (box[0], box[-1])
    simulator.keypress("esc")


# ── Toasts ────────────────────────────────────────────────────────────────────

TOAST_QUEUE = r"""
local d, ui, sys = picocalc.display, picocalc.ui, picocalc.sys
for _, n in ipairs({3, 6, 9, 12, 15, 18}) do ui.toast(string.rep("x", n)) end
ui.toast("")                     -- ignored
ui.toast(string.rep("y", 100), ui.TOAST_ERROR)
local seen, last = {}, nil
local t0 = sys.getTimeMs()
while sys.getTimeMs() - t0 < 16000 do
    -- The back buffer holds the frame before last, toast and all.
    local left, right
    for x = 0, 319 do
        if d.getPixel(x, 285) ~= 0 then left = left or x; right = x end
    end
    local w = left and (right - left + 1) or 0
    if w ~= last then
        seen[#seen + 1] = w
        last = w
    end
    d.clear(0)
    d.flush()
    sys.sleep(100)
end
sys.log("WIDTHS " .. table.concat(seen, " "))
"""


@pytest.mark.sd(fixtures=[])
def test_toast_queue_keeps_the_newest_three(sim_factory, test_sd_card):
    """Pushed before the first present: only the last three are still waiting,
    in order, one at a time, then nothing."""
    stage_lua_app(test_sd_card, "ui_toasts", TOAST_QUEUE)
    sim = sim_factory(test_sd_card, virtual_time=True)
    sim.launch_app("ui_toasts")
    out = sim.wait_for_exit(timeout=30)
    assert out.get("result") == "returned", out
    line = next(e["text"] for e in sim.get_log_lines(0) if "WIDTHS " in e["text"])
    widths = [int(v) for v in line.split("WIDTHS ")[1].split()]
    # 15 and 18 x's (16 px of padding), then 63 y's, wider than the panel.
    assert [w for w in widths if w] == [15 * 6 + 16, 18 * 6 + 16, 320], widths
    assert widths[-1] == 0, widths
