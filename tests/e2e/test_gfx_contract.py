"""picocalc.graphics contract: fixes from the SDK review (docs/API-Display-and-Graphics.md).

The cases live in tests/e2e/fixtures/gfx_contract (staged hidden: the default
card has no app slot to spare). test_gfx_contract_on_device runs them on a
PicoCalc, where the framebuffer is byte-swapped and TGX, the interpolator and
Core 1's preload are the real ones. The Python tests below cover what one app
cannot: a load that needs an asset (GIF), the heap on the real allocator, and
state crossing from one app to the next.
"""
import io
import shutil
from pathlib import Path

import pytest
from PIL import Image

from helpers import app_rel_dir, lua_case_names, run_lua_app, stage_lua_app

FIXTURE = Path(__file__).parent / "fixtures" / "gfx_contract"
CASES = lua_case_names(FIXTURE)


@pytest.fixture(scope="module")
def gfx_run(lua_suite):
    return lua_suite(
        "gfx_contract",
        setup=lambda sd: shutil.copytree(FIXTURE, Path(sd) / app_rel_dir("gfx_contract")),
        fixtures=[])


@pytest.mark.parametrize("case", CASES)
def test_gfx_contract(gfx_run, case):
    gfx_run.check_case(case)


def test_gfx_contract_complete(gfx_run):
    gfx_run.assert_all_passed(CASES)


@pytest.mark.hardware
@pytest.mark.timeout(300)
def test_gfx_contract_on_device(target):
    target.push_app(FIXTURE, "gfx_contract")
    run = target.run_lua_app("gfx_contract", timeout=120)
    run.assert_all_passed(CASES)


# ── Assets ───────────────────────────────────────────────────────────────────

def _rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _picture():
    """64x48, palette colours only: left half red, right half blue, a green
    bottom row, and a noisy middle band, since AnimatedGIF refuses a file too
    short to hold what it reads ahead (a flat image of this size is)."""
    img = Image.new("P", (64, 48), 0)
    palette = [255, 0, 0,  0, 0, 255,  0, 255, 0] + [40 * (i % 6) for i in range(39)]
    img.putpalette(palette)
    for y in range(48):
        for x in range(64):
            if y == 47:
                c = 2
            elif 16 <= y < 32:
                c = 3 + (x * 7 + y * 13) % 13
            else:
                c = 0 if x < 32 else 1
            img.putpixel((x, y), c)
    return img


def _encoded(fmt):
    out = io.BytesIO()
    pic = _picture()
    (pic if fmt == "GIF" else pic.convert("RGB")).save(out, format=fmt)
    return out.getvalue()


GIF_APP = f"""
local T = picocalc.sys.loadlib("picotest")
local G = picocalc.graphics
local PATH = APP_DIR .. "/photo.gif"

local function check(img)
    local w, h = img:getSize()
    T.eq(w, 64)
    T.eq(h, 48)
    T.eq(img:getPixel(4, 4), {_rgb565(255, 0, 0)})
    T.eq(img:getPixel(60, 4), {_rgb565(0, 0, 255)})
    T.eq(img:getPixel(4, 47), {_rgb565(0, 255, 0)})
end

-- A recycled heap block: the decoder must not read what was left in it.
local function dirty_heap()
    local t = {{}}
    for i = 1, 30 do t[i] = string.rep("\\xAB", 40000) end
    t = nil
    collectgarbage()
    collectgarbage()
end

T.case("load", function() check(G.image.load(PATH)) end)
T.case("load on a dirty heap", function() dirty_heap() check(G.image.load(PATH)) end)
T.case("loadFromBuffer", function()
    check(G.image.loadFromBuffer(picocalc.fs.readFile(PATH)))
end)
T.case("loadFromBuffer on a dirty heap", function()
    local raw = picocalc.fs.readFile(PATH)
    dirty_heap()
    check(G.image.loadFromBuffer(raw))
end)
T.case("getInfo", function()
    local info = G.image.getInfo(PATH)
    T.eq(info.width, 64)
    T.eq(info.height, 48)
    T.eq(info.format, "gif")
end)
T.case("loadRegion and loadScaled", function()
    local r, w, h = G.image.loadRegion(PATH, 30, 0, 10, 10)
    T.eq(w, 10)
    T.eq(r:getPixel(0, 0), {_rgb565(255, 0, 0)})
    T.eq(r:getPixel(9, 0), {_rgb565(0, 0, 255)})
    local s = G.image.loadScaled(PATH, 32, 24)
    T.eq(s:getPixel(2, 2), {_rgb565(255, 0, 0)})
end)
T.case("draws", function()
    local img = G.image.load(PATH)
    img:draw(0, 0)
    T.eq(picocalc.display.getPixel(4, 4), {_rgb565(255, 0, 0)})
    T.eq(picocalc.display.getPixel(60, 4), {_rgb565(0, 0, 255)})
end)
T.done()
"""


GIF_CASES = ["load", "load on a dirty heap", "loadFromBuffer",
             "loadFromBuffer on a dirty heap", "getInfo", "loadRegion and loadScaled",
             "draws"]


@pytest.mark.parametrize("real_umm", [False, True], ids=["counting", "real_umm"])
def test_gif_loads(sim_factory, test_sd_card, real_umm):
    """AnimatedGIF was never begin()'d: its state was whatever the heap held,
    so a load segfaulted here and could fault on the device."""
    stage_lua_app(test_sd_card, "gif_load", GIF_APP,
                  files={"photo.gif": _encoded("GIF")})
    sim = sim_factory(test_sd_card, extra_args=["--real-umm"] if real_umm else [])
    run = run_lua_app(sim, "gif_load", timeout=30)
    run.assert_all_passed(GIF_CASES)


@pytest.mark.hardware
@pytest.mark.timeout(300)
def test_gif_loads_on_device(target):
    """On the device an uninitialised decoder is a HardFault risk, not a segfault:
    a pass here is the proof, and a reboot during it fails the run."""
    target.stage_lua_app("gif_load", GIF_APP, files={"photo.gif": _encoded("GIF")})
    run = target.run_lua_app("gif_load", timeout=120)
    run.assert_all_passed(GIF_CASES)


# ── Image memory and the collector ───────────────────────────────────────────

OOM_APP = """
local T = picocalc.sys.loadlib("picotest")
local G = picocalc.graphics
local S = G.sprite
local PNG = APP_DIR .. "/big.png"
local BMP = APP_DIR .. "/big.bmp"
local RAW = picocalc.fs.readFile(BMP)

-- Each call makes a 256 x 256 image (128 KB) and drops it. The collector does
-- not see image memory, so with garbage piling up the heap runs out long before
-- 200 rounds unless the failing allocation collects and tries again.
local function churn(make)
    collectgarbage()
    collectgarbage("stop")
    for i = 1, 200 do make() end
    collectgarbage("restart")
end

local base = G.image.new(256, 256)

T.case("image.new", function() churn(function() G.image.new(256, 256) end) end)
T.case("image.load bmp", function() churn(function() G.image.load(BMP) end) end)
T.case("image.load png", function() churn(function() G.image.load(PNG) end) end)
T.case("loadFromBuffer", function() churn(function() G.image.loadFromBuffer(RAW) end) end)
T.case("copy", function() churn(function() base:copy() end) end)
T.case("loadRegion", function() churn(function() G.image.loadRegion(BMP, 0, 0, 256, 256) end) end)
T.case("loadScaled", function() churn(function() G.image.loadScaled(BMP, 256, 256) end) end)
T.case("imageWithText", function()
    churn(function() T.ok(G.imageWithText("x", 256, 256, 0)) end)
end)
T.case("spriteWithText", function()
    churn(function() T.ok(S.spriteWithText("x", 256, 256, 0)) end)
end)
T.case("sprite setSourceRect", function()
    churn(function()
        local s = S.new(base)
        s:setSourceRect(0, 0, 256, 256)
        T.eq(s:getSize() ~= nil, true)
    end)
end)
T.case("a load that fails for another reason still raises", function()
    local ok, err = pcall(G.image.load, APP_DIR .. "/missing.png")
    T.eq(ok, false)
    T.ok(tostring(err):find("failed to load", 1, true), err)
end)
T.done()
"""

OOM_CASES = ["image.new", "image.load bmp", "image.load png", "loadFromBuffer", "copy",
             "loadRegion", "loadScaled", "imageWithText", "spriteWithText",
             "sprite setSourceRect", "a load that fails for another reason still raises"]


def _big(fmt):
    out = io.BytesIO()
    Image.new("RGB", (256, 256), (10, 20, 30)).save(out, format=fmt)
    return out.getvalue()


def test_image_allocation_collects_and_retries(sim_factory, test_sd_card):
    """Pixels sit outside Lua's accounting, so dropped images put no pressure on
    the collector: --real-umm, the device's heap."""
    stage_lua_app(test_sd_card, "img_oom", OOM_APP,
                  files={"big.png": _big("PNG"), "big.bmp": _big("BMP")})
    sim = sim_factory(test_sd_card, extra_args=["--real-umm"])
    run = run_lua_app(sim, "img_oom", timeout=55)
    run.assert_all_passed(OOM_CASES)


# ── State between apps ───────────────────────────────────────────────────────

LEAK_A = """
local G = picocalc.graphics
G.setColor(0x001F)
G.setBackgroundColor(0xF800)
G.setTransparentColor(0xF81F)
picocalc.sys.log("LEAK_A done")
"""

LEAK_B = """
local T = picocalc.sys.loadlib("picotest")
local G = picocalc.graphics
local D = picocalc.display
T.case("colours are the defaults", function()
    D.clear(0x07E0)
    G.drawText("X", 10, 10)
    local fg, bg, other = 0, 0, 0
    for y = 10, 17 do
        for x = 10, 15 do
            local p = D.getPixel(x, y)
            if p == 0xFFFF then fg = fg + 1
            elseif p == 0 then bg = bg + 1
            else other = other + 1 end
        end
    end
    T.ok(fg > 0, "white text")
    T.ok(bg > 0, "black background")
    T.eq(other, 0)
end)
T.case("no global key", function() T.eq(G.getTransparentColor(), nil) end)
T.done()
"""


def test_graphics_state_does_not_reach_the_next_app(simulator, test_sd_card):
    stage_lua_app(test_sd_card, "leak_a", LEAK_A)
    stage_lua_app(test_sd_card, "leak_b", LEAK_B)
    simulator.launch_app("leak_a")
    simulator.wait_for_exit(timeout=15)
    run = run_lua_app(simulator, "leak_b", timeout=15)
    run.assert_all_passed(["colours are the defaults", "no global key"])


# ── Preload ──────────────────────────────────────────────────────────────────

PRELOAD_APP = """
local T = picocalc.sys.loadlib("picotest")
local G = picocalc.graphics
local PATH = APP_DIR .. "/art.png"

local function wait()
    for _ = 1, 500 do
        local img, ready = G.image.pollPreload()
        if ready then return img, true end
        picocalc.sys.sleep(5)
    end
    return nil, false
end

T.case("a preload delivers the image", function()
    T.eq(G.image.preload(PATH), true)
    local img, ready = wait()
    T.eq(ready, true)
    T.eq((img:getSize()), 64)
    T.eq(img:getPixel(4, 4), 0xF800)
end)
T.case("after delivery nothing is pending", function()
    local img, ready = G.image.pollPreload()
    T.eq(img, nil)
    T.eq(ready, false)
end)
T.case("a missing file reports ready with no image", function()
    T.eq(G.image.preload(APP_DIR .. "/missing.png"), true)
    local img, ready = wait()
    T.eq(img, nil)
    T.eq(ready, true)
end)
T.case("cancel drops the pending load", function()
    T.eq(G.image.preload(PATH), true)
    G.image.cancelPreload()
    local img, ready = G.image.pollPreload()
    T.eq(img, nil)
    T.eq(ready, false)
    T.eq(G.image.preload(PATH), true)
    local again = wait()
    T.ok(again)
end)
T.done()
"""


PRELOAD_CASES = ["a preload delivers the image", "after delivery nothing is pending",
                 "a missing file reports ready with no image",
                 "cancel drops the pending load"]


def test_preload(simulator, test_sd_card):
    stage_lua_app(test_sd_card, "preload_app", PRELOAD_APP,
                  files={"art.png": _encoded("PNG")})
    run = run_lua_app(simulator, "preload_app", timeout=20)
    run.assert_all_passed(PRELOAD_CASES)


@pytest.mark.hardware
@pytest.mark.timeout(300)
def test_preload_on_device(target):
    """The same cases with Core 1 really decoding."""
    target.stage_lua_app("preload_app", PRELOAD_APP, files={"art.png": _encoded("PNG")})
    run = target.run_lua_app("preload_app", timeout=60)
    run.assert_all_passed(PRELOAD_CASES)
