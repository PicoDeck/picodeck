-- Image pixel access (img:getPixel/setPixel/getPixels/setPixels) and
-- display.getPixel. picotest kit; see tests/e2e/test_image_pixels.py.
--
-- Colours with distinct high and low bytes (0x1234) catch a byte swap: every
-- value must come back exactly as written, and the bulk strings are
-- little-endian RGB565 ("<I2").
--
-- With draw.flag in the app directory the app skips the cases and draws a
-- scene built from generated images every frame until Esc, for the harness
-- to probe with get_pixel.

local pc = picocalc
local disp, gfx, fs = pc.display, pc.graphics, pc.fs
local T = pc.sys.loadlib("picotest")

local function pack(...)
    local t = { ... }
    local s = {}
    for i = 1, #t do s[i] = string.pack("<I2", t[i]) end
    return table.concat(s)
end

local function solid(w, h, colour)
    local img = gfx.image.new(w, h)
    img:setPixels(string.rep(string.pack("<I2", colour), w * h))
    return img
end

-- ── Draw mode ───────────────────────────────────────────────────────────────

if fs.exists(APP_DIR .. "/draw.flag") then
    -- 2x2 by setPixel, drawn at 10x: quadrants 0x1234, 0xF800 / 0x07E0, 0x001F
    local quad = gfx.image.new(2, 2)
    quad:setPixel(0, 0, 0x1234); quad:setPixel(1, 0, 0xF800)
    quad:setPixel(0, 1, 0x07E0); quad:setPixel(1, 1, 0x001F)
    -- 4x1 by setPixels, drawn at 5x from x = 40
    local strip = gfx.image.new(4, 1)
    strip:setPixels(pack(0xABCD, 0x5678, 0x0F0F, 0xF0F0))
    -- 1x2 wall texture for a column at x = 100, rows 0-19
    local wall = gfx.image.new(1, 2)
    wall:setPixels(pack(0x2345, 0x6789))
    -- 8x8 solid floor for a plane below y = 200
    local floor = solid(8, 8, 0x4A69)

    local function draw()
        disp.clear(disp.BLACK)
        quad:drawScaledNN(0, 0, 10)
        strip:drawScaledNN(40, 0, 5)
        disp.drawTexturedColumn(100, 0, 19, wall, 0, 0, 1)
        disp.drawPlane(floor, 0, 0, 16, 0, 200, 1)
        disp.flush()
    end

    draw()
    pc.sys.log("PX:READY")
    while true do
        pc.input.update()
        if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
        draw()
        pc.sys.sleep(20)
    end
end

-- ── Cases ───────────────────────────────────────────────────────────────────

gfx.setTransparentColor(nil)
disp.clearClipRect()

T.case("new_image_is_black", function()
    local img = gfx.image.new(4, 3)
    for y = 0, 2 do
        for x = 0, 3 do T.eq(img:getPixel(x, y), 0) end
    end
    T.eq(img:getPixels(), string.rep("\0", 4 * 3 * 2))
end)

T.case("set_get_roundtrip", function()
    local img = gfx.image.new(8, 1)
    local colours = { 0x1234, 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0001, 0x8000,
                      disp.rgb(200, 100, 50) }
    for i, c in ipairs(colours) do img:setPixel(i - 1, 0, c) end
    for i, c in ipairs(colours) do
        local got = img:getPixel(i - 1, 0)
        T.eq(math.type(got), "integer")
        T.eq(got, c, string.format("pixel %d", i - 1))
    end
end)

T.case("coordinates_round", function()
    local img = gfx.image.new(4, 4)
    img:setPixel(1.4, 0.6, 0x1234)            -- rounds to (1, 1)
    T.eq(img:getPixel(1, 1), 0x1234)
    T.eq(img:getPixel(0.5, 1.49), 0x1234)     -- ties toward +inf: (1, 1)
    T.eq(img:getPixel(0, 0), 0)
end)

T.case("colour_must_be_integer", function()
    local img = gfx.image.new(2, 2)
    T.raises(function() img:setPixel(0, 0, 1.5) end, "integer representation")
    T.raises(function() img:setPixel(0, 0) end, "number expected")
    T.eq(img:getPixel(0, 0), 0)
end)

T.case("pixel_bounds", function()
    local img = gfx.image.new(4, 3)
    for _, p in ipairs({ { -1, 0 }, { 4, 0 }, { 0, 3 }, { 0, -1 }, { 1e6, 0 } }) do
        T.raises(function() img:getPixel(p[1], p[2]) end, "outside the 4x3 image")
        T.raises(function() img:setPixel(p[1], p[2], 1) end, "outside the 4x3 image")
    end
    T.raises(function() img:getPixel(0 / 0, 0) end, "NaN")
    T.eq(img:getPixels(), string.rep("\0", 24))
end)

T.case("getPixels_layout", function()
    local img = gfx.image.new(3, 2)
    for y = 0, 1 do
        for x = 0, 2 do img:setPixel(x, y, 0x1234 + x + y * 0x100) end
    end
    local s = img:getPixels()
    T.eq(#s, 3 * 2 * 2)
    T.eq(s:byte(1), 0x34, "low byte first")
    T.eq(s:byte(2), 0x12, "high byte second")
    for y = 0, 1 do
        for x = 0, 2 do
            T.eq(string.unpack("<I2", s, 1 + 2 * (y * 3 + x)), img:getPixel(x, y))
        end
    end
end)

T.case("setPixels_whole", function()
    local img = gfx.image.new(3, 2)
    local s = pack(0x1234, 0xF800, 0x07E0, 0x001F, 0xFFFF, 0xABCD)
    img:setPixels(s)
    T.eq(img:getPixel(0, 0), 0x1234)
    T.eq(img:getPixel(2, 0), 0x07E0)
    T.eq(img:getPixel(0, 1), 0x001F)
    T.eq(img:getPixel(2, 1), 0xABCD)
    T.eq(img:getPixels(), s)
end)

T.case("sub_rect", function()
    local img = solid(4, 4, 0x0101)
    local inner = pack(0x1234, 0x5678, 0x9ABC, 0xDEF0)
    img:setPixels(inner, 1, 1, 2, 2)
    T.eq(img:getPixels(1, 1, 2, 2), inner)
    T.eq(img:getPixel(1, 1), 0x1234)
    T.eq(img:getPixel(2, 1), 0x5678)
    T.eq(img:getPixel(1, 2), 0x9ABC)
    T.eq(img:getPixel(2, 2), 0xDEF0)
    T.eq(img:getPixels(0, 0, 4, 1), string.rep(pack(0x0101), 4), "top row untouched")
    T.eq(img:getPixels(0, 1, 4, 1), pack(0x0101, 0x1234, 0x5678, 0x0101))
    T.eq(img:getPixels(3, 0, 1, 4), string.rep(pack(0x0101), 4), "right column untouched")
    img:setPixels(pack(0xFFFF, 0xEEEE), 0, 3, 2, 1)
    T.eq(img:getPixels(0, 3, 4, 1), pack(0xFFFF, 0xEEEE, 0x0101, 0x0101))
end)

T.case("empty_rect", function()
    local img = gfx.image.new(4, 4)
    T.eq(img:getPixels(0, 0, 0, 0), "")
    T.eq(img:getPixels(4, 4, 0, 0), "")
    T.eq(img:getPixels(2, 1, 0, 3), "")
    img:setPixels("", 4, 0, 0, 4)
    T.eq(img:getPixels(), string.rep("\0", 32))
end)

T.case("bulk_errors", function()
    local img = gfx.image.new(4, 4)
    T.raises(function() img:setPixels(string.rep("\0", 31)) end, "31 bytes, expected 32")
    T.raises(function() img:setPixels(string.rep("\0", 33)) end, "33 bytes, expected 32")
    T.raises(function() img:setPixels(pack(1, 2), 0, 0, 2, 2) end, "4 bytes, expected 8")
    T.raises(function() img:setPixels(pack(1), 4, 0, 1, 1) end, "outside the 4x4 image")
    T.raises(function() img:setPixels(pack(1, 2), 3, 0, 2, 1) end, "outside the 4x4 image")
    T.raises(function() img:getPixels(0, 0, 5, 1) end, "outside the 4x4 image")
    T.raises(function() img:getPixels(-1, 0, 1, 1) end, "outside the 4x4 image")
    T.raises(function() img:getPixels(0, 0, -1, 1) end, "outside the 4x4 image")
    T.raises(function() img:getPixels(0, 0, 1) end, "number expected")
    T.raises(function() img:setPixels(pack(1), 0, 0) end, "number expected")
    T.raises(function() img:setPixels({}) end, "string expected")
    T.eq(img:getPixels(), string.rep("\0", 32), "a refused write changed nothing")
end)

T.case("copy_is_independent", function()
    local img = gfx.image.new(2, 1)
    img:setPixel(0, 0, 0x1234)
    local c = img:copy()
    c:setPixel(0, 0, 0x4321)
    T.eq(img:getPixel(0, 0), 0x1234)
    T.eq(c:getPixel(0, 0), 0x4321)
end)

T.case("transparent_colour_is_stored", function()
    local img = gfx.image.new(2, 1)
    img:setTransparentColor(0xF81F)
    img:setPixel(1, 0, 0xF81F)
    T.eq(img:getPixel(1, 0), 0xF81F)
    T.eq(img:getTransparentColor(), 0xF81F)
end)

T.case("freed_image_raises", function()
    -- The holder is created before the image, so the image is finalised
    -- first and the holder's finaliser sees it with its pixels freed.
    local R = {}
    local holder = setmetatable({}, { __gc = function(h)
        R.ran = true
        R.get = { pcall(h.img.getPixel, h.img, 0, 0) }
        R.set = { pcall(h.img.setPixel, h.img, 0, 0, 1) }
        R.gets = { pcall(h.img.getPixels, h.img) }
        R.sets = { pcall(h.img.setPixels, h.img, "\0\0\0\0") }
    end })
    holder.img = gfx.image.new(2, 1)
    holder = nil
    for _ = 1, 3 do collectgarbage("collect") end
    T.ok(R.ran, "the holder finaliser never ran")
    for _, k in ipairs({ "get", "set", "gets", "sets" }) do
        T.eq(R[k][1], false, k .. " on a freed image succeeded")
        T.ok(tostring(R[k][2]):find("freed"), k .. ": " .. tostring(R[k][2]))
    end
end)

T.case("display_getPixel", function()
    disp.clear(disp.BLACK)
    disp.fillRect(10, 10, 2, 2, 0x1234)
    disp.setPixel(319, 319, 0xABCD)
    T.eq(disp.getPixel(10, 10), 0x1234)
    T.eq(disp.getPixel(11.4, 10.6), 0x1234)
    T.eq(disp.getPixel(12, 10), 0)
    T.eq(disp.getPixel(319, 319), 0xABCD)
    for _, p in ipairs({ { -1, 0 }, { 320, 0 }, { 0, 320 }, { 0, -1 } }) do
        T.raises(function() disp.getPixel(p[1], p[2]) end, "outside the screen")
    end
    -- Reads ignore the clip rect.
    disp.setClipRect(0, 0, 5, 5)
    T.eq(disp.getPixel(10, 10), 0x1234)
    disp.clearClipRect()
end)

T.case("draw_generated_image", function()
    disp.clear(disp.BLACK)
    local img = gfx.image.new(2, 2)
    img:setPixels(pack(0x1234, 0xF800, 0x07E0, 0xABCD))
    img:draw(20, 20)
    T.eq(disp.getPixel(20, 20), 0x1234)
    T.eq(disp.getPixel(21, 20), 0xF800)
    T.eq(disp.getPixel(20, 21), 0x07E0)
    T.eq(disp.getPixel(21, 21), 0xABCD)
    -- A pixel set to the image's transparent colour is skipped when drawn.
    disp.fillRect(30, 30, 2, 1, 0x0841)
    local keyed = gfx.image.new(2, 1)
    keyed:setTransparentColor(0xF81F)
    keyed:setPixels(pack(0x1234, 0xF81F))
    keyed:draw(30, 30)
    T.eq(disp.getPixel(30, 30), 0x1234)
    T.eq(disp.getPixel(31, 30), 0x0841)
end)

T.case("drawTexturedColumn_generated", function()
    disp.clear(disp.BLACK)
    local tex = gfx.image.new(2, 4)
    local col = { 0x1234, 0x5678, 0x9ABC, 0xDEF0 }
    for y = 0, 3 do
        tex:setPixel(0, y, 0xFFFF)
        tex:setPixel(1, y, col[y + 1])
    end
    disp.drawTexturedColumn(50, 100, 103, tex, 1, 0, 3)
    for y = 0, 3 do T.eq(disp.getPixel(50, 100 + y), col[y + 1]) end
    T.eq(disp.getPixel(50, 104), 0)
end)

T.case("drawPlane_generated", function()
    -- A two-colour 8x8 checkerboard (4x4 squares): every ground pixel is one
    -- of the two, both appear, and the sky above the horizon is untouched.
    local A, B = 0x1234, 0xABCD
    local tex = gfx.image.new(8, 8)
    local rows = {}
    for y = 0, 7 do
        local r = {}
        for x = 0, 7 do
            r[#r + 1] = ((x // 4 + y // 4) % 2 == 0) and A or B
        end
        rows[#rows + 1] = pack(table.unpack(r))
    end
    tex:setPixels(table.concat(rows))
    T.eq(tex:getPixel(0, 0), A); T.eq(tex:getPixel(4, 0), B)
    disp.clear(disp.BLACK)
    disp.drawPlane(tex, 4, 4, 8, 0, 160, 4)
    local seen = { [A] = 0, [B] = 0 }
    for y = 161, 319, 6 do
        for x = 0, 319, 5 do
            local c = disp.getPixel(x, y)
            T.ok(seen[c], string.format("ground pixel (%d, %d) is 0x%04X", x, y, c))
            seen[c] = seen[c] + 1
        end
    end
    T.ok(seen[A] > 0 and seen[B] > 0, "both texture colours drawn")
    T.eq(disp.getPixel(160, 100), 0, "sky untouched")
end)

T.done()
