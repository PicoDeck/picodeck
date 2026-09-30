-- E2E fixture for picocalc.graphics.
--
-- Tilemap: builds a 64x64 tileset (four 32x32 tiles: red, green, blue,
-- yellow) as an in-memory 16bpp top-down BMP, puts it in a 2x2 tilemap in
-- REVERSED order (4,3 / 2,1) and draws it at the screen origin. The test
-- probes the centre of each on-screen tile. Every tile except tile 1 has a
-- non-zero source offset, so a swapped image/region argument in
-- tilemap_draw shows up as missing (black) tiles.
--
-- Blinker: blinker:start(on, off) must take its durations from arguments
-- 2 and 3 (argument 1 is self).

local pc  = picocalc
local d   = pc.display
local gfx = pc.graphics
local log = pc.sys.log

local RED, GREEN, BLUE, YELLOW = 0xF800, 0x07E0, 0x001F, 0xFFE0
local TS, TILE = 64, 32

local function tileset_bmp()
    local px = {}
    for y = 0, TS - 1 do
        for x = 0, TS - 1 do
            local c
            if y < TILE then c = (x < TILE) and RED or GREEN
            else             c = (x < TILE) and BLUE or YELLOW end
            px[#px + 1] = string.pack("<I2", c)
        end
    end
    local pixels = table.concat(px)
    local header = string.pack("<c2I4I2I2I4", "BM", 54 + #pixels, 0, 0, 54)
    local info = string.pack("<I4i4i4I2I2I4I4i4i4I4I4",
        40, TS, -TS, 1, 16, 0, #pixels, 2835, 2835, 0, 0)
    return header .. info .. pixels
end

d.clear(d.BLACK)

local ok, err = pcall(function()
    local tileset = gfx.image.loadFromBuffer(tileset_bmp())
    local tm = gfx.tilemap.new(tileset, TILE, TILE)
    tm:setSize(2, 2)
    tm:setTileAtPosition(0, 0, 4)
    tm:setTileAtPosition(1, 0, 3)
    tm:setTileAtPosition(0, 1, 2)
    tm:setTileAtPosition(1, 1, 1)
    tm:draw(0, 0)
    d.flush()
    _G.__keep = { tileset, tm }   -- tilemap holds a raw pointer to the image
end)
if ok then log("GT TILEMAP_READY") else log("GT TILEMAP_ERR " .. tostring(err)) end

-- blinker:start(on, off): with on=50ms/off=5000ms the blinker is in its OFF
-- phase 200ms after start. Reading the durations from the wrong stack slots
-- either errors (self is not an integer) or leaves the 500ms default "on".
local bok, berr = pcall(function()
    local b = gfx.animation.blinker.new()
    b:start(50, 5000)
    pc.sys.sleep(200)
    local state = b:update()
    b:stop()
    b:start()              -- no-argument form keeps the current durations
    b:remove()
    return state
end)
if bok then
    log("GT BLINKER_OK state=" .. tostring(berr))
else
    log("GT BLINKER_ERR " .. tostring(berr))
end

-- animator.new(duration, from, to [, easing [, delay]]): the userdata is not an
-- argument, a nil easing is absent, and a delay holds the start value.
local function try(name, fn)
    local aok, a, b = pcall(fn)
    if aok then log("GT " .. name .. "_OK " .. tostring(a) .. " " .. tostring(b))
    else log("GT " .. name .. "_ERR " .. tostring(a)) end
end
local anim = gfx.animation.animator
try("ANIM4", function()
    local a = anim.new(100, 0, 100, "cubicOut")
    pc.sys.sleep(250)
    return a:currentValue(), a:ended()
end)
try("ANIMNIL", function()
    local a = anim.new(100, 0, 100, nil)
    pc.sys.sleep(250)
    return a:currentValue(), a:ended()
end)
try("ANIMDELAY", function()
    local a = anim.new(200, 10, 90, "linear", 400)
    local held = a:currentValue()
    local p = a:progress()
    pc.sys.sleep(700)
    return held .. "/" .. p, a:currentValue() .. "/" .. tostring(a:ended())
end)

-- An easing name that is not recognised raises and names the valid set; a
-- valid name constructs, and so does an absent one (linear).
try("EASEBAD", function() return anim.new(100, 0, 100, "easeIn") end)
try("EASEEMPTY", function() return anim.new(100, 0, 100, "") end)
try("EASEOK", function() return anim.new(100, 0, 100, "quadInOut") ~= nil end)
try("EASELINEAR", function()
    local a = anim.new(1000, 0, 100, nil)
    local b = anim.new(1000, 0, 100, "linear")
    return a:currentValue() == b:currentValue() and "same", "x"
end)

log("GT DONE")
-- Hold the frame for pixel probes until ESC.
local input = pc.input
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(16)
end
