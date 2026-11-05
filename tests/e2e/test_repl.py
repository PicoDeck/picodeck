"""picocalc.repl (src/os/lua_bridge_repl.c): the input row, the output above it."""

import time

import pytest

from helpers import PROJECT_ROOT, stage_lua_app

INPUT_ROW = 37 * 8   # TERM_INPUT_ROW: the input bar's top pixel row

LOOP = r"""
local repl, input, sys = picocalc.repl, picocalc.input, picocalc.sys
{setup}
sys.log("REPL:READY")
while true do
    local line = repl.readline()
    if line then
        sys.log("REPL:LINE " .. line)
    end
    local p = input.getButtonsPressed()
    if p & input.BTN_ESC ~= 0 then
        sys.log("REPL:ESC")
        {on_esc}
    end
    sys.sleep(5)
end
"""


def start(sim, sd, setup="repl.print()", on_esc="return"):
    stage_lua_app(sd, "repl_app", LOOP.replace("{setup}", setup).replace("{on_esc}", on_esc))
    since = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("repl_app")
    sim.wait_for_log("REPL:READY", timeout=10, since_seq=since)


def type_keys(sim, keys):
    for key in keys:
        seq = sim.keypress(key)["input_seq"]
        sim.wait_input_consumed(seq)


def band(sim, y0, y1):
    """The presented image's pixel rows y0..y1, as bytes."""
    return sim.screenshot_pil().convert("RGB").crop((0, y0, 320, y1)).tobytes()


def wait_band(sim, y0, y1, want, timeout=3.0):
    """Wait until rows y0..y1 equal `want` (a flush lands after the key is read)."""
    deadline = time.time() + timeout
    got = None
    while time.time() < deadline:
        got = band(sim, y0, y1)
        if got == want:
            return True
        time.sleep(0.02)
    return False


def input_bar_after(sim, sd, setup, keys):
    """A fresh app's input bar once `keys` have been typed."""
    start(sim, sd, setup)
    before = band(sim, INPUT_ROW, 320)
    type_keys(sim, keys)
    deadline = time.time() + 3
    while time.time() < deadline and band(sim, INPUT_ROW, 320) == before:
        time.sleep(0.02)
    time.sleep(0.05)
    got = band(sim, INPUT_ROW, 320)
    sim.exit_app()
    sim.wait_for_exit(timeout=10)
    return got


@pytest.mark.sd(fixtures=[])
def test_echo_off_shows_stars_not_the_text(simulator, test_sd_card):
    masked = input_bar_after(simulator, test_sd_card,
                             "repl.echo(false)\nrepl.print()", list("secret"))
    stars = input_bar_after(simulator, test_sd_card, "repl.print()", list("******"))
    plain = input_bar_after(simulator, test_sd_card, "repl.print()", list("secret"))
    assert masked == stars, "echo(false) must draw one * per character"
    assert masked != plain, "echo(false) must not draw the text itself"


@pytest.mark.sd(fixtures=[])
def test_echo_off_line_is_returned_but_not_printed(simulator, test_sd_card):
    start(simulator, test_sd_card, "repl.echo(false)\nrepl.print()")
    empty = band(simulator, 0, INPUT_ROW)
    type_keys(simulator, list("secret") + ["enter"])
    simulator.wait_for_log("REPL:LINE secret", timeout=5)
    time.sleep(0.1)
    assert band(simulator, 0, INPUT_ROW) == empty, "the line went to the output"


@pytest.mark.sd(fixtures=[])
def test_echo_on_line_goes_to_the_output(simulator, test_sd_card):
    start(simulator, test_sd_card)
    empty = band(simulator, 0, INPUT_ROW)
    type_keys(simulator, list("hello") + ["enter"])
    simulator.wait_for_log("REPL:LINE hello", timeout=5)
    time.sleep(0.1)
    assert band(simulator, 0, INPUT_ROW) != empty


@pytest.mark.sd(fixtures=[])
def test_output_stays_on_screen_while_typing(simulator, test_sd_card):
    """A keystroke used to draw the input row into the back buffer and swap, so
    every other keystroke presented the older frame without the newest line."""
    start(simulator, test_sd_card,
          'repl.print("line one")\nrepl.print("line two")\nrepl.print("line three")')
    time.sleep(0.1)
    rows = 3 * 8
    want = band(simulator, 0, rows)
    assert any(want), "the three lines never appeared"
    seen = []
    prev_bar = band(simulator, INPUT_ROW, 320)
    for key in ["a", "b", "c", "d", "backspace", "backspace"]:
        type_keys(simulator, [key])
        deadline = time.time() + 3
        while time.time() < deadline and band(simulator, INPUT_ROW, 320) == prev_bar:
            time.sleep(0.01)
        prev_bar = band(simulator, INPUT_ROW, 320)
        seen.append(band(simulator, 0, rows) == want)
    assert all(seen), f"output intact after each key: {seen}"


@pytest.mark.sd(fixtures=[])
def test_esc_reads_as_nil_and_keeps_the_line(simulator, test_sd_card):
    """What the page says: Esc returns nil, leaves the typed text, and the app
    sees it with getButtonsPressed right after readline."""
    start(simulator, test_sd_card, on_esc='sys.log("REPL:STAY")')
    type_keys(simulator, list("abc") + ["esc"])
    simulator.wait_for_log("REPL:STAY", timeout=5)
    type_keys(simulator, ["enter"])
    simulator.wait_for_log("REPL:LINE abc", timeout=5)


@pytest.mark.sd(fixtures=[])
def test_print_expands_tabs(simulator, test_sd_card):
    """print's tab separators are tab stops, not box glyphs."""
    def first_row(setup):
        start(simulator, test_sd_card, setup)
        time.sleep(0.1)
        got = band(simulator, 0, 8)
        simulator.exit_app()
        simulator.wait_for_exit(timeout=10)
        return got

    tabbed = first_row('repl.print("ab", 42, true)')
    spaced = first_row('repl.print("ab      42      true")')
    assert tabbed == spaced


@pytest.mark.sd(fixtures=[])
def test_print_keeps_its_line_rules(simulator, test_sd_card):
    """Tab expansion left these alone: a newline ends a line, a full line wraps
    with no blank row after it, and an empty print adds nothing."""
    code = r"""
local repl, sys = picocalc.repl, picocalc.sys
repl.print("one\ntwo")
repl.print(string.rep("x", 53))
repl.print(string.rep("y", 53) .. "\nz")
repl.print(string.rep("w", 54))
repl.print()
repl.print("\tq")
repl.print("end\n")
sys.log("REPL:DONE")
while true do repl.readline(); sys.sleep(5) end
"""
    stage_lua_app(test_sd_card, "repl_lines", code)
    simulator.launch_app("repl_lines")
    simulator.wait_for_log("REPL:DONE", timeout=10)
    time.sleep(0.05)
    img = simulator.screenshot_pil().convert("RGB")
    rows = []
    for row in range(37):
        lit = [x for x in range(320) if any(img.getpixel((x, row * 8 + dy)) != (0, 0, 0)
                                            for dy in range(8))]
        rows.append((lit[0] // 6, lit[-1] // 6) if lit else None)
    # one / two / 53 x / 53 y / z / 53 w / w / <tab>q / end, then nothing
    assert rows[0][1] <= 3 and rows[1][1] <= 3, rows[:3]
    assert rows[2] == (0, 52) and rows[3] == (0, 52), rows[:5]
    assert rows[4][1] == 0, rows[:6]
    assert rows[5] == (0, 52), rows[:8]
    assert rows[6] == (0, 0), rows[:8]
    assert rows[7] == (8, 8), rows[:9]      # the tab went to column 8
    assert rows[8][1] <= 3 and rows[9] is None, rows[:11]


# ── The page's examples, verbatim ────────────────────────────────────────────

def doc_block(heading):
    text = (PROJECT_ROOT / "docs" / "API-Repl.md").read_text()
    return text.split(heading, 1)[1].split("```lua\n", 1)[1].split("```", 1)[0]


@pytest.mark.sd(fixtures=[])
def test_doc_readline_example(simulator, test_sd_card):
    stage_lua_app(test_sd_card, "repl_doc", doc_block("#### `picocalc.repl.readline()`"))
    simulator.launch_app("repl_doc")
    simulator.wait_frames(1)
    time.sleep(0.1)
    first = band(simulator, 0, 8)
    assert any(first), "the greeting never appeared"
    bar = band(simulator, INPUT_ROW, 320)
    assert any(bar), "the input bar never appeared"
    type_keys(simulator, list("1+2") + ["enter"])
    time.sleep(0.2)
    # Row 1 is the echoed line, row 2 the result.
    assert any(band(simulator, 8, 16)) and any(band(simulator, 16, 24))
    simulator.keypress("esc")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out


@pytest.mark.sd(fixtures=[])
def test_doc_print_and_clear_examples(simulator, test_sd_card):
    code = (doc_block("#### `picocalc.repl.print(...)`") +
            doc_block("#### `picocalc.repl.clear()`"))
    stage_lua_app(test_sd_card, "repl_doc", code)
    simulator.launch_app("repl_doc")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out


@pytest.mark.sd(fixtures=[])
def test_doc_echo_example(simulator, test_sd_card):
    code = doc_block("#### `picocalc.repl.echo(enabled)`") + \
        '\npicocalc.sys.log("LEN " .. #password)\n'
    stage_lua_app(test_sd_card, "repl_doc", code)
    simulator.launch_app("repl_doc")
    time.sleep(0.3)
    type_keys(simulator, list("hunter2") + ["enter"])
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    assert any("LEN 7" in e["text"] for e in simulator.get_log_lines(0))


@pytest.mark.sd(fixtures=[])
def test_readline_alone_draws_nothing(simulator, test_sd_card):
    """What the page warns about: no bar until the first key."""
    code = r"""
local d, repl, sys = picocalc.display, picocalc.repl, picocalc.sys
d.clear(0xF800); d.flush(); d.clear(0xF800); d.flush()
local t0 = sys.getTimeMs()
while sys.getTimeMs() - t0 < 300 do repl.readline(); sys.sleep(5) end
sys.log("WAITED")
sys.sleep(400)
repl.print()
sys.log("PRINTED")
sys.sleep(300)
"""
    stage_lua_app(test_sd_card, "repl_draw", code)
    simulator.launch_app("repl_draw")
    simulator.wait_for_log("WAITED", timeout=10)
    # The red frame is up: nothing of the REPL has drawn.
    assert simulator.screenshot_pil().convert("RGB").getpixel((100, 300)) == (255, 0, 0)
    simulator.wait_for_log("PRINTED", timeout=10)
    time.sleep(0.05)
    assert simulator.screenshot_pil().convert("RGB").getpixel((100, 300)) != (255, 0, 0)
    simulator.wait_for_exit(timeout=10)


@pytest.mark.sd(fixtures=[])
def test_clear_forgets_typed_text(simulator, test_sd_card):
    code = r"""
local repl, sys = picocalc.repl, picocalc.sys
repl.print()
sys.log("READY")
while true do
    local line = repl.readline()
    if line then sys.log("LINE [" .. line .. "]"); return end
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_F1 ~= 0 then repl.clear() end
    sys.sleep(5)
end
"""
    stage_lua_app(test_sd_card, "repl_clear", code)
    simulator.launch_app("repl_clear")
    simulator.wait_for_log("READY", timeout=10)
    type_keys(simulator, ["a", "b", "f1", "c", "enter"])
    simulator.wait_for_log(r"LINE \[c\]", timeout=5)
    simulator.wait_for_exit(timeout=10)
