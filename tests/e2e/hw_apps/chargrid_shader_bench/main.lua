-- chargrid shader cost on the device (tests/e2e/test_chargrid_hw.py):
-- - an aurora-class program (3 fbm(3), 2 noise, 3 exp, 1 sin, 2 smooth, r,g,b
--   through the nearest palette entry) at 80x40, 80x80 and 64x28;
-- - the RPG torch at 53x40;
-- - each built-in's cost per call: 8 calls per cell over 80x40, minus the
--   same sum without them (approximate: a few arguments add one add).
-- 3 warm-up and 15 timed runs each (getStats().us), and the XIP cache hit
-- rate over the timed runs. Writes shader_bench.json.
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc
local CG = pc.chargrid

local AURORA = [[
uniform t
array edge
layer land
keep = land.glyph ~= 0
d = y - edge[x]
a = fbm(x * 0.04 + t * 0.03, y * 0.07, 3) * smooth(-4, 30, d) * exp(-max(d, 0) / 12)
e = sin(x * 0.035 + t * 0.3) * 4 + 14 - y
c = fbm(x * 0.03, y * 0.05 + t * 0.03, 3) * exp(-max(e, 0) / 9)
hz = fbm(x * 0.04, y * 0.07, 3) * 0.3 + 0.85
st = noise(x * 0.5, y * 1.4 - t, 9)
tw = smooth(0.25, 0.75, noise(x * 0.04, t * 0.4, 9))
gl = exp(-max(d, 0) / 6)
r = a * 0.18 + c * 0.4 + hz * 0.025
g = c * 0.75 + a + gl * 0.062 + hz * 0.04
b = a * 0.48 + c * 0.7 + st * 0.1 + tw * 0.05
level = clamp(max(max(r, g), b) * 0.9 + 0.1, 0, 1)
]]
local TORCH = [[
uniform px, py
keep = glyph == 0
tone = clamp(1 - dist(x, y, px, py) / 8, 0, 1)
]]

local rows = {}
local function bench(name, src, cols, nrows, setup)
    local g = CG.new(cols, nrows, nil, 4, 4)
    local sh = assert(CG.shader(src))
    local layers = setup and setup(g, sh) or {}
    local dts = {}
    for i = 1, 18 do
        if i == 4 then pc.sys.getMemInfo() end  -- resets the XIP hit-rate window
        sh:run(g, 0, 0, cols, nrows, table.unpack(layers))
        if i > 3 then dts[#dts + 1] = sh:getStats().us end
    end
    local xip = pc.sys.getMemInfo().xip_cache_hit_rate
    table.sort(dts)
    local st = sh:getStats()
    rows[#rows + 1] = string.format(
        '{"name":"%s","us_median":%d,"us_max":%d,"cells":%d,"instructions":%d,"xip":%d}',
        name, dts[#dts // 2 + 1], dts[#dts], st.cells, st.instructions, math.floor(xip + 0.5))
    T.case("bench_" .. name, function() T.eq(#dts, 15) end)
    return dts[#dts // 2 + 1]
end

local function aurora_setup(g, sh)
    local cols, nrows = g:getSize()
    local land, edge = CG.new(cols, nrows, nil, 4, 4), {}
    for x = 0, cols - 1 do
        edge[x + 1] = 16 + 6 * math.sin(x * 0.07)
        local top = math.floor(nrows * 0.6 + 6 * math.sin(x * 0.11))
        land:fill(x, top, 1, nrows - top, CG.SHADE2, 236, 0)
    end
    g:setRamp("\0\1\2\3\4", true)
    sh:set("t", 12.5)
    sh:setArray("edge", edge)
    return { land }
end
bench("aurora 80x40", AURORA, 80, 40, aurora_setup)
bench("aurora 80x80", AURORA, 80, 80, aurora_setup)
bench("aurora 64x28", AURORA, 64, 28, aurora_setup)
bench("torch 53x40", TORCH, 53, 40, function(g, sh)
    g:clear("#", 245, 0)
    g:setColorRamp({ 17, 18, 24, 60, 136, 178, 214, 228 })
    sh:set("px", 26)
    sh:set("py", 20)
    return {}
end)

local ARGS = { "x * 0.1", "x * 0.1 + 1.3", "y * 0.2 + 0.7", "x * 0.05 + y * 0.03",
               "x * 0.07 + 2.1", "y * 0.13 + 0.4", "x * 0.11 + 5", "y * 0.17 + 3.3" }
local function sum_of(fmt)
    local t = {}
    for i, a in ipairs(ARGS) do t[i] = string.format(fmt, a) end
    return "level = " .. table.concat(t, " + ")
end
local base = bench("base 80x40", sum_of("(%s)"), 80, 40)
local BUILTINS = {
    { "sin", "sin(%s)" }, { "exp", "exp(%s)" }, { "log", "log(%s + 1)" }, { "sqrt", "sqrt(%s)" },
    { "pow", "pow(%s + 1, 1.7)" }, { "atan2", "atan2(%s, 3)" }, { "noise", "noise(%s, y)" },
    { "fbm3", "fbm(%s, y, 3)" },
}
for _, b in ipairs(BUILTINS) do
    local us = bench("builtin " .. b[1], sum_of(b[2]), 80, 40)
    rows[#rows + 1] = string.format('{"name":"per-call %s","ns":%d}', b[1],
                                    math.floor((us - base) * 1000 / (8 * 3200) + 0.5))
end
local f = pc.fs.open("/data/" .. APP_ID .. "/shader_bench.json", "w")
pc.fs.write(f, "[" .. table.concat(rows, ",") .. "]")
pc.fs.close(f)
T.done()
