-- chargrid draw cost on the device (tests/e2e/test_chargrid_hw.py): random
-- glyphs and colours at five cell sizes, full-window draws, 5 warm-up and 40
-- timed frames each (getStats().us around the draw, flush not counted), plus
-- the XIP cache hit rate over the timed loop. The @px1 row draws at x = 1.
-- Writes bench.json.
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc
local CG, disp = pc.chargrid, pc.display

local CONFIGS = {
    { name = "53x40@6x8", cols = 53, rows = 40, font = 0 },
    { name = "40x26@8x12", cols = 40, rows = 26, font = 1 },
    { name = "64x40@5x8", cols = 64, rows = 40, cw = 5, ch = 8 },
    { name = "80x40@4x4", cols = 80, rows = 40, cw = 4, ch = 4 },
    { name = "80x80@4x4", cols = 80, rows = 80, cw = 4, ch = 4 },
    { name = "53x40@6x8@px1", cols = 53, rows = 40, font = 0, px = 1 },
}
math.randomseed(1)
local rows = {}
for _, c in ipairs(CONFIGS) do
    local g = c.font and CG.new(c.cols, c.rows, c.font) or CG.new(c.cols, c.rows, nil, c.cw, c.ch)
    local lo, hi = c.font and 0x20 or 1, c.font and 0x7E or 27
    local gl, fg, bg = {}, {}, {}
    for y = 0, c.rows - 1 do
        for x = 1, c.cols do
            gl[x] = math.random(lo, hi); fg[x] = math.random(0, 255); bg[x] = math.random(0, 255)
        end
        g:setRow(y, gl, fg, bg)
    end
    local dts = {}
    pc.sys.getMemInfo()  -- resets the XIP hit-rate window
    for f = 1, 45 do
        g:draw(c.px or 0, 0)
        if f > 5 then dts[#dts + 1] = g:getStats().us end
        disp.flush()
    end
    local xip = pc.sys.getMemInfo().xip_cache_hit_rate
    table.sort(dts)
    rows[#rows + 1] = string.format('{"name":"%s","us_median":%d,"us_max":%d,"cells":%d,"xip":%d}',
        c.name, dts[#dts // 2 + 1], dts[#dts], g:getStats().cells, math.floor(xip + 0.5))
    T.case("bench_" .. c.name, function() T.eq(#dts, 40) end)
end
local f = pc.fs.open("/data/" .. APP_ID .. "/bench.json", "w")
pc.fs.write(f, "[" .. table.concat(rows, ",") .. "]")
pc.fs.close(f)
T.done()
