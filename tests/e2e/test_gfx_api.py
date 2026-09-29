"""Graphics/display bridge argument checks: img:drawScaled and drawScaledNN
reject scales that cannot be intended (#38), display.loadFont and
graphics.font.new report a full registry and bad names truthfully (#40, #41),
and img:drawStretched takes a positional srcRect (#42).

The gfx_api_test fixture holds the picotest cases; the pixel check for the
positional srcRect needs a live screen, so it runs its own small app."""
import pytest

from helpers import case_params, lua_case_names, stage_lua_app

APP = "gfx_api_test"
CASES = lua_case_names(APP)


@pytest.fixture(scope="module")
def run(lua_suite):
    return lua_suite(APP)


@pytest.mark.parametrize("case", case_params(CASES, {}))
def test_gfx_api(run, case):
    run.check_case(case)


def test_gfx_api_clean_exit(run):
    run.assert_clean_exit()


SRC_RECT_APP = r"""
local pc, gfx, d = picocalc, picocalc.graphics, picocalc.display
local function bmp(w, h, px)
    local pixels = {}
    for i = 1, w * h do pixels[i] = string.pack("<I2", px((i - 1) % w, (i - 1) // w)) end
    pixels = table.concat(pixels)
    local header = string.pack("<c2I4I2I2I4", "BM", 54 + #pixels, 0, 0, 54)
    local info = string.pack("<I4i4i4I2I2I4I4i4i4I4I4",
        40, w, -h, 1, 16, 0, #pixels, 2835, 2835, 0, 0)
    return header .. info .. pixels
end
-- 4x2: left half red, right half blue
local im = gfx.image.loadFromBuffer(bmp(4, 2, function(x) return x < 2 and 0xF800 or 0x001F end))
d.clear(d.BLACK)
im:drawStretched(0, 0, 8, 8, { 2, 0, 2, 2 })                 -- positional: the blue half
im:drawStretched(20, 0, 8, 8, { x = 2, y = 0, w = 2, h = 2 }) -- named: the same
im:drawStretched(40, 0, 8, 8)                                 -- whole image: red | blue
im:draw(60, 0, false, { 2, 0, 2, 2 })                         -- img:draw positional: blue 2x2
im:draw(70, 0, false, { x = 2, y = 0, w = 2, h = 2 })         -- named: the same
d.flush()
pc.sys.log("SRC READY")
while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(16)
end
"""


def _rgb(px):
    return (px["r"], px["g"], px["b"])


def test_positional_src_rect_matches_named(simulator):
    stage_lua_app(simulator.sd_card_path, "gfx_srcrect", SRC_RECT_APP)
    simulator.clear_log()
    simulator.launch_app("gfx_srcrect")
    simulator.wait_for_log("SRC READY", timeout=20)

    def px(x, y):
        return simulator.call("get_pixel", {"x": x, "y": y})

    def is_blue(p):
        return p["b"] > 200 and p["r"] < 40
    def is_red(p):
        return p["r"] > 200 and p["b"] < 40

    for x in (1, 6):                       # positional srcRect: all blue
        assert is_blue(px(x, 4)), f"positional x={x}: {px(x, 4)}"
    for x in (21, 26):                     # named srcRect: all blue
        assert is_blue(px(x, 4)), f"named x={x}: {px(x, 4)}"
    for x in (60, 61, 70, 71):             # img:draw srcRect, both forms
        assert is_blue(px(x, 1)), f"img:draw x={x}: {px(x, 1)}"
    assert is_red(px(41, 4)) and is_blue(px(46, 4))   # no srcRect: whole image
