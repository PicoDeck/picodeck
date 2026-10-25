-- Lua apps' hot path on the device (tests/e2e/test_lua_perf_hw.py): the code
-- src/hot_text.ld pins at fixed XIP cache sets, where a layout change shows.
-- - interpreter: pure Lua (table loops, branches, calls, closures, string
--   formatting);
-- - C calls: Lua calling small C functions (math, string, sys), the bridge
--   and C API path a layout change hit hardest;
-- - fire 64x40: ascii-scene's fire at its 5x8 cells, a frame of Lua per
--   cell (heat spreading up, math.random) handed to chargrid a row at a time
--   (shadeRow), then drawn; no flush, so it times the work, not the panel.
-- At 300 MHz, like ascii-scene: there a cache miss costs the most cycles.
-- 3 warm-up and 15 timed runs each (sys.getTimeUs), and the XIP cache hit
-- rate over the timed runs. Writes lua_bench.json.
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc
local CG, U = pc.chargrid, pc.sys.getTimeUs

local rows = {}
local function bench(name, fn)
    local dts = {}
    for i = 1, 18 do
        if i == 4 then pc.sys.getMemInfo() end  -- resets the XIP hit-rate window
        local t0 = U()
        fn()
        if i > 3 then dts[#dts + 1] = U() - t0 end
    end
    local xip = pc.sys.getMemInfo().xip_cache_hit_rate
    table.sort(dts)
    rows[#rows + 1] = string.format('{"name":"%s","us_median":%d,"us_max":%d,"xip":%d}',
        name, dts[#dts // 2 + 1], dts[#dts], math.floor(xip + 0.5))
    T.case("bench_" .. name, function() T.eq(#dts, 15) end)
end

local sink = 0
bench("interpreter", function()
    local t, s = {}, 0
    for i = 1, 2000 do t[i] = i * 3 % 17 end
    for r = 1, 5 do
        for i = 1, 2000 do
            local v = t[i]
            if v > 8 then s = s + v * r else s = s - v end
            t[i] = (v + r) % 17
        end
    end
    local function f(a, b) return a + b * 2 end
    for i = 1, 3000 do s = f(s, i) % 100003 end
    local parts = {}
    for i = 1, 200 do parts[#parts + 1] = string.format("%d:%d", i, s % (i + 1)) end
    sink = sink + s + #table.concat(parts, ",")
end)

bench("C calls", function()
    local floor, rnd, byte, us, s = math.floor, math.random, string.byte, pc.sys.getTimeUs, 0
    for i = 1, 4000 do
        s = s + floor(i * 0.37) + rnd(0, 9) + byte("chargrid", i % 8 + 1) + us() % 2
    end
    sink = sink + s
end)

-- 5x8 cells with no font, as ascii-scene's Spleen grid: the ramp is the
-- built-in dots and shades.
local g = CG.new(64, 40, nil, 5, 8)
local W, H = g:getSize()
g:setRamp("\0\1\2\3\4\5\6\7\8")
g:setColorRamp({ 16, 52, 88, 124, 160, 196, 202, 208, 214, 220, 226, 227, 228, 229, 230, 231 })
local heat, row, random = {}, {}, math.random
math.randomseed(1)
for i = 1, W * H do heat[i] = 0 end
for x = 1, W do heat[(H - 1) * W + x] = 36 end
bench("fire 64x40", function()
    for y = 1, H - 1 do
        local base = y * W
        for x = 1, W do
            local src = base + x
            local r = random(0, 3)
            local dst = src - W - r + 1
            if dst >= 1 then
                local v = heat[src] - (r & 1)
                heat[dst] = v > 0 and v or 0
            end
        end
    end
    for y = 0, H - 1 do
        local base = y * W
        for x = 1, W do row[x] = heat[base + x] * (1 / 36) end
        g:shadeRow(y, row)
    end
    g:draw(0, 0)
end)

local f = pc.fs.open("/data/" .. APP_ID .. "/lua_bench.json", "w")
pc.fs.write(f, "[" .. table.concat(rows, ",") .. "]")
pc.fs.close(f)
T.done()
