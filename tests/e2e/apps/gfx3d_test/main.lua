local T = picocalc.sys.loadlib("picotest")
local g3, gfx = picocalc.gfx3d, picocalc.graphics

-- One unlit triangle at depth -z facing +z (towards a camera at the origin
-- looking along -z); reversed (clockwise from the camera) when cw is true.
local function tri(z, cw)
    local v = { -1, -1, z, 1, -1, z, 0, 1, z }
    return g3.newMesh(v, cw and { 1, 3, 2 } or { 1, 2, 3 }, 0xF800, g3.UNLIT)
end

T.case("constants", function()
    T.eq(math.type(g3.DOUBLE_SIDED), "integer")
    T.ok(g3.DOUBLE_SIDED ~= g3.UNLIT and g3.UNLIT ~= g3.NO_FOG)
end)

T.case("newMesh_and_getInfo", function()
    local m = g3.newMesh({ 0, 0, 0, 2, 0, 0, 0, 2, 0 }, { 1, 2, 3 }, { 0x07E0 })
    local nv, nt, cx, cy, cz, r = m:getInfo()
    T.eq(nv, 3); T.eq(nt, 1)
    T.eq(cx, 1.0); T.eq(cy, 1.0); T.eq(cz, 0.0)
    T.ok(math.abs(r - math.sqrt(2)) < 1e-5, tostring(r))
end)

T.case("newMesh_argument_errors", function()
    local v = { 0, 0, 0, 1, 0, 0, 0, 1, 0 }
    T.raises(function() g3.newMesh({ 0, 0 }, { 1, 2, 3 }, 0) end, "multiple of 3")
    T.raises(function() g3.newMesh(v, { 1, 2 }, 0) end, "multiple of 3")
    T.raises(function() g3.newMesh(v, { 1, 2, 4 }, 0) end, "out of range")
    T.raises(function() g3.newMesh(v, { 0, 1, 2 }, 0) end, "out of range")
    T.raises(function() g3.newMesh(v, { 1, 2.5, 3 }, 0) end, "integer expected")
    T.raises(function() g3.newMesh({ 0, 0, 0 / 0, 1, 0, 0, 0, 1, 0 }, { 1, 2, 3 }, 0) end, "NaN")
    T.raises(function() g3.newMesh(v, { 1, 2, 3 }, { 0, 0 }) end, "one colour per triangle")
    T.raises(function() g3.newMesh(v, { 1, 2, 3 }, 0, 8) end, "flags")
    T.raises(function() g3.newMesh(v, { 1, 2, 3 }, 0x10000) end, "colour")
end)

T.case("scene_state_errors", function()
    local m = tri(-5)
    T.raises(function() g3.draw(m, 0, 0, 0, 0, 0, 0) end, "beginScene")
    T.raises(function() g3.endScene() end, "beginScene")
    g3.beginScene()
    T.raises(function() g3.draw(m, 0, 0, 0, 0, 0, 0, 0) end, "scale must be positive")
    T.raises(function() g3.drawBasis(m, 0, 0, 0, 0, 0, 0, 0, 1, 0) end, "not parallel")
    T.raises(function() g3.draw(m, 0 / 0, 0, 0, 0, 0, 0) end, "NaN")
    g3.endScene()
    T.raises(function() g3.endScene() end, "beginScene")
end)

T.case("setter_errors", function()
    T.raises(function() g3.setProjection(0, 1, 10) end, "fovY")
    T.raises(function() g3.setProjection(1, 5, 5) end, "near < far")
    T.raises(function() g3.setViewport(400, 400, 10, 10) end, "overlap")
    T.raises(function() g3.lookAt(1, 2, 3, 1, 2, 3) end, "must differ")
    T.raises(function() g3.setLight(0, 0, 0) end, "zero")
    T.raises(function() g3.setFog(10, 5, 0) end, "near < far")
    T.raises(function() g3.setSky({ { 0.5, 1 }, { 0.2, 2 } }) end, "increase")
    T.raises(function() g3.setSky({}) end, "1 to 8")
    g3.setSky(nil)
    g3.setFog(nil)
    g3.setViewport(0, 0, 320, 320)
    g3.setProjection(1.0471976, 0.5, 1000)
end)

T.case("culling_and_double_sided", function()
    g3.lookAt(0, 0, 0, 0, 0, -1)
    g3.beginScene(0)
    g3.draw(tri(-5), 0, 0, 0, 0, 0, 0)
    g3.draw(tri(-5, true), 0, 0, 0, 0, 0, 0)
    g3.endScene()
    local s = g3.getStats()
    T.eq(s.tris_in, 2); T.eq(s.drawn, 1); T.eq(s.culled, 1)
    local ds = g3.newMesh({ -1, -1, -5, 1, -1, -5, 0, 1, -5 }, { 1, 3, 2 }, 0xF800, g3.DOUBLE_SIDED)
    g3.beginScene(0)
    g3.draw(ds, 0, 0, 0, 0, 0, 0)
    g3.endScene()
    T.eq(g3.getStats().drawn, 1)
end)

T.case("mesh_collected_between_draw_and_endScene", function()
    g3.beginScene(0)
    do
        local m = tri(-5)
        g3.draw(m, 0, 0, 0, 0, 0, 0)
    end
    collectgarbage("collect")
    collectgarbage("collect")
    g3.endScene()
    T.eq(g3.getStats().drawn, 1)
end)

T.case("sprite_image_collected_before_endScene", function()
    g3.beginScene(0)
    do
        g3.drawSprite(gfx.image.new(4, 4), 0, 0, -10, 2)
    end
    collectgarbage("collect")
    collectgarbage("collect")
    g3.endScene()
    T.eq(g3.getStats().sprites, 1)
end)

T.case("error_mid_scene_then_next_frame", function()
    local ok = pcall(function() g3.beginScene(0); error("boom") end)
    T.eq(ok, false)
    g3.beginScene(0)
    g3.draw(tri(-5), 0, 0, 0, 0, 0, 0)
    g3.endScene()
    T.eq(g3.getStats().drawn, 1)
end)

T.case("project", function()
    g3.lookAt(0, 0, 0, 0, 0, -1)
    local x, y, d = g3.project(0, 0, -10)
    T.ok(math.abs(x - 160) < 0.01 and math.abs(y - 160) < 0.01, x .. "," .. y)
    T.ok(math.abs(d - 10) < 1e-4, tostring(d))
    T.eq(g3.project(0, 0, 10), nil)
end)

T.case("stats_fields", function()
    local s = g3.getStats()
    for _, k in ipairs({ "tris_in", "culled", "clipped", "drawn", "sprites", "overflow", "us_geom", "us_raster" }) do
        T.eq(math.type(s[k]), "integer", k)
    end
end)

T.case("getStats_fills_a_given_table", function()
    g3.lookAt(0, 0, 0, 0, 0, -1)
    g3.beginScene(0)
    g3.draw(tri(-5), 0, 0, 0, 0, 0, 0)
    g3.endScene()
    local t = { mine = 7 }
    local r = g3.getStats(t)
    T.ok(r == t, "returns the table it was given")
    T.eq(t.tris_in, 1); T.eq(t.mine, 7)
    T.eq(math.type(t.us_raster), "integer")
    T.ok(g3.getStats(nil) ~= t, "nil: a new table")
    T.ok(not pcall(g3.getStats, 5), "a non-table is an error")
end)

T.case("drawList_draws_every_mesh", function()
    g3.lookAt(0, 0, 0, 0, 0, -1)
    local a, b = tri(-5), tri(-6)
    g3.beginScene(0)
    g3.draw(a, 0, 0, 0, 0, 0, 0); g3.draw(b, 0, 0, 0, 0, 0, 0)
    g3.endScene()
    local want = g3.getStats()
    g3.beginScene(0)
    g3.drawList({ a, b })
    g3.drawList({ a, b }, 50, 2)   -- a bias and a start index (wraps)
    g3.drawList({})
    g3.endScene()
    local got = g3.getStats()
    T.eq(got.tris_in, 2 * want.tris_in); T.eq(got.drawn, 2 * want.drawn)
end)

T.case("drawList_rejects_bad_input", function()
    g3.beginScene(0)
    local ok, err = pcall(g3.drawList, { tri(-5), 7 })
    T.ok(not ok and tostring(err):find("list[2]", 1, true), "non-mesh element: " .. tostring(err))
    T.ok(not pcall(g3.drawList, { tri(-5) }, 0, 2), "start past the end")
    T.ok(not pcall(g3.drawList, { tri(-5) }, 0, 0), "start below 1")
    T.ok(not pcall(g3.drawList, "x"), "not a table")
    g3.endScene()
    T.ok(not pcall(g3.drawList, { tri(-5) }), "outside a scene")
end)

-- ── Textures, textured meshes, sky image, mirror ────────────────────────────
-- The camera is at the origin looking along -z with the default 60 degree fovY
-- over the whole 320x320 screen, so a point at depth d lands focal / d = 277.1 / d
-- pixels per unit from the centre (160, 160): 55.4 px a unit at depth 5.

local disp = picocalc.display
local RED, BLUE, BG = 0xF800, 0x001F, 0x0841

-- Texels as addTexture takes them: little-endian RGB565, row by row.
local function px(...)
    local t = {}
    for i, c in ipairs({ ... }) do t[i] = string.pack("<I2", c) end
    return table.concat(t)
end

-- A quad facing the camera, x -1..1 and y y0..y1 at depth -z, u running 0..1
-- left to right. A white vertex colour leaves a fully saturated texel as it is.
local function quad(slot, z, y0, y1, flags)
    return g3.newTexturedMesh {
        xyz = { -1, y0, z, 1, y0, z, 1, y1, z, -1, y1, z },
        uv = { 0, 1, 1, 1, 1, 0, 0, 0 },
        color = 0xFFFF,
        idx = { 1, 2, 3, 1, 3, 4 },
        tex = slot,
        flags = flags,
    }
end

local function near(a, b) return math.abs(a - b) < 1e-3 end

T.case("textured_mesh_draws", function()
    g3.setCamera(0, 0, 0, 0, 0, 0)
    local tex = g3.addTexture(1, 0, 1, px(RED, BLUE))  -- 2 x 1: red, then blue
    T.eq(math.type(tex), "integer")
    local m = quad(tex, -5, -1, 1)
    local nv, nt, cx, cy, cz, r = m:getInfo()
    T.eq(nv, 4); T.eq(nt, 2)
    T.eq(cx, 0.0); T.eq(cy, 0.0); T.eq(cz, -5.0)
    T.ok(near(r, math.sqrt(2)), tostring(r))

    g3.beginScene(BG)
    g3.drawTextured(m, 0, 0, 0, 0, 0, 0)
    g3.endScene()
    local s = g3.getStats()
    T.eq(s.tris_in, 2); T.eq(s.drawn, 2)
    T.eq(disp.getPixel(133, 160), RED, "u 0.25: texel 0")
    T.eq(disp.getPixel(187, 160), BLUE, "u 0.75: texel 1")
    T.eq(disp.getPixel(160, 100), BG, "above the quad: the clear colour")

    -- Half a repeat along u: the halves swap. An offset, not a step: 0 puts it back.
    g3.scrollTexture(tex, 0.5, 0)
    g3.beginScene(BG)
    g3.drawTextured(m, 0, 0, 0, 0, 0, 0)
    g3.endScene()
    T.eq(disp.getPixel(133, 160), BLUE, "scrolled")
    T.eq(disp.getPixel(187, 160), RED, "scrolled")
    g3.scrollTexture(tex, 0, 0)

    -- Position, rotation and scale: half a turn about y shows the quad's back,
    -- culled; at twice the size and 5 to the right it reaches past the screen.
    g3.beginScene(BG)
    g3.drawTextured(m, 0, 0, -10, math.pi, 0, 0)
    g3.endScene()
    T.eq(g3.getStats().culled, 2, "back face")
    g3.beginScene(BG)
    g3.drawTextured(m, 1, 0, 5, 0, 0, 0, 2)  -- centre (1, 0, -5): x 160 +- 110.9 + 55.4
    g3.endScene()
    T.eq(disp.getPixel(160, 160), RED, "scaled about the model's origin")
    T.eq(disp.getPixel(300, 160), BLUE)
end)

T.case("sky_image_draws", function()
    -- 2 x 4 texels, each its own colour.
    local function tx(c, r) return 0x0800 * (r + 1) + c + 1 end
    local t = {}
    for r = 0, 3 do for c = 0, 1 do t[#t + 1] = tx(c, r) end end
    local sky = g3.addTexture(1, 2, 1, px(table.unpack(t)))
    g3.setCamera(0, 0, 0, 0, 0, 0)  -- level: the horizon between rows 159 and 160
    g3.setSkyImage(sky, 2)          -- texture row 2 is the first below it
    g3.beginScene()
    g3.endScene()
    T.eq(disp.getPixel(0, 160), tx(0, 2)); T.eq(disp.getPixel(1, 160), tx(1, 2))
    T.eq(disp.getPixel(2, 160), tx(0, 2), "the columns repeat")
    T.eq(disp.getPixel(0, 159), tx(0, 1)); T.eq(disp.getPixel(0, 158), tx(0, 0))
    T.eq(disp.getPixel(0, 161), tx(0, 3))
    T.eq(disp.getPixel(0, 0), tx(0, 0), "the edge rows repeat")
    T.eq(disp.getPixel(319, 319), tx(1, 3))

    -- A turn moves it a texel per pixel's worth of yaw.
    local focal = 160 / math.tan(1.0471976 / 2)
    g3.setCamera(0, 0, 0, 1.2 / focal, 0, 0)
    g3.beginScene()
    g3.endScene()
    T.eq(disp.getPixel(0, 160), tx(1, 2), "turned one texel")

    -- Sky rows: the fill touches only rows 100-199.
    g3.setCamera(0, 0, 0, 0, 0, 0)
    disp.clear(0x1234)
    g3.setSkyRows(100, 199)
    g3.beginScene()
    g3.endScene()
    T.eq(disp.getPixel(0, 99), 0x1234); T.eq(disp.getPixel(0, 100), tx(0, 0))
    T.eq(disp.getPixel(0, 199), tx(0, 3)); T.eq(disp.getPixel(0, 200), 0x1234)

    -- Rows that hold the viewport are the default; with no image, the clear colour.
    g3.setSkyRows(0, 319)
    g3.setSkyImage(nil)
    g3.beginScene(0x2345)
    g3.endScene()
    T.eq(disp.getPixel(0, 160), 0x2345); T.eq(disp.getPixel(0, 0), 0x2345)
end)

T.case("mirror_toggles", function()
    g3.setCamera(0, 0, 0, 0, 0, 0)
    local x0, y0, d0 = g3.project(0, 2, -10)
    g3.setMirror(true, 0)
    local x1, y1, d1 = g3.project(0, 2, -10)  -- where its reflection lands
    g3.setMirror(false)
    local x2, y2, d2 = g3.project(0, -2, -10)
    T.ok(near(x1, x2) and near(y1, y2) and near(d1, d2),
         string.format("mirrored %g,%g,%g against %g,%g,%g", x1, y1, d1, x2, y2, d2))
    T.ok(y0 < 160 and y1 > 160, y0 .. " " .. y1)
    local x3, y3, d3 = g3.project(0, 2, -10)
    T.eq(x3, x0); T.eq(y3, y0); T.eq(d3, d0)  -- off again: the real camera, to the bit

    g3.setMirror(true, 1)  -- y = 2 reflected in y = 1 is y = 0
    local mx, my = g3.project(0, 2, -10)
    g3.setMirror(false)
    local rx, ry = g3.project(0, 0, -10)
    T.ok(near(mx, rx) and near(my, ry), my .. " " .. ry)

    -- A quad above the plane (rows 77-132) draws below it (188-243) while
    -- mirrored, still facing the eye.
    local m = quad(g3.addTexture(0, 0, 1, px(RED)), -5, 0.5, 1.5)
    for _, on in ipairs({ true, false }) do
        g3.setMirror(on, 0)
        g3.beginScene(BG)
        g3.drawTextured(m, 0, 0, 0, 0, 0, 0)
        g3.endScene()
        T.eq(g3.getStats().drawn, 2, "mirror " .. tostring(on))
        T.eq(disp.getPixel(160, 105), on and BG or RED, "mirror " .. tostring(on))
        T.eq(disp.getPixel(160, 215), on and RED or BG, "mirror " .. tostring(on))
    end
    T.raises(function() g3.setMirror(true, 0 / 0) end, "NaN")
    T.raises(function() g3.setMirror() end, "value expected")
end)

T.case("mesh_after_gc_rejected", function()
    local tex = g3.addTexture(0, 0, 1, px(RED))
    local R = {}
    do
        -- The holder is marked for finalisation first, so the mesh is finalised
        -- first: the holder's finaliser then uses a mesh whose own has run.
        local holder = setmetatable({}, { __gc = function(h)
            R.ran = true
            R.ok_info, R.err_info = pcall(h.m.getInfo, h.m)
            g3.beginScene(0)
            R.ok_draw, R.err_draw = pcall(g3.drawTextured, h.m, 0, 0, 0, 0, 0, 0)
            g3.endScene()
        end })
        holder.m = quad(tex, -5, -1, 1)
    end
    for _ = 1, 3 do collectgarbage("collect") end
    T.ok(R.ran, "the holder's finaliser never ran")
    T.eq(R.ok_info, false, "getInfo on a finalised mesh")
    T.ok(tostring(R.err_info):find("freed"), tostring(R.err_info))
    T.eq(R.ok_draw, false, "drawTextured with a finalised mesh")
    T.ok(tostring(R.err_draw):find("freed"), tostring(R.err_draw))
end)

T.case("textured_mesh_kept_until_endScene", function()
    -- The scene reads a textured mesh's uv at endScene, so it holds the mesh
    -- until the next beginScene.
    local tex = g3.addTexture(0, 0, 1, px(BLUE))
    g3.setCamera(0, 0, 0, 0, 0, 0)
    local held = setmetatable({}, { __mode = "k" })
    g3.beginScene(BG)
    do
        local m = quad(tex, -5, -1, 1)
        held[m] = true
        g3.drawTextured(m, 0, 0, 0, 0, 0, 0)
    end
    for _ = 1, 3 do collectgarbage("collect") end
    T.ok(next(held) ~= nil, "collected inside its scene")
    g3.endScene()
    T.eq(g3.getStats().drawn, 2)
    T.eq(disp.getPixel(160, 160), BLUE)
    g3.beginScene(BG)
    g3.endScene()
    for _ = 1, 3 do collectgarbage("collect") end
    T.eq(next(held), nil, "still held after the next beginScene")
end)

T.case("sprite_fog_toggles", function()
    g3.setCamera(0, 0, 0, 0, 0, 0)
    local img = gfx.image.new(4, 4)
    img:setPixels(string.rep(string.pack("<I2", 0xFFFF), 16))
    g3.setFog(0, 10, 0x0000)
    local function centre()
        g3.beginScene(BG)
        g3.drawSprite(img, 0, 0, -5, 2)  -- depth 5: half way into the fog
        g3.endScene()
        return disp.getPixel(160, 160)
    end
    T.eq(centre(), 0xFFFF, "no sprite fog by default")
    g3.setSpriteFog(true)
    local c = centre()
    local r = c >> 11
    T.ok(r >= 12 and r <= 19 and c & 0x1F == r, string.format("fogged half way: %04x", c))
    g3.setSpriteFog(false)
    T.eq(centre(), 0xFFFF, "off again")
    g3.setFog(nil)
    T.raises(function() g3.setSpriteFog() end, "value expected")
end)

-- Last: it fills the context's 64 texture slots.
T.case("texture_args_validated", function()
    local two = px(RED, BLUE)
    T.raises(function() g3.addTexture(11, 0, 1, two) end, "lgW")
    T.raises(function() g3.addTexture(-1, 0, 1, two) end, "lgW")
    T.raises(function() g3.addTexture(1, 11, 1, two) end, "lgH")
    T.raises(function() g3.addTexture(1, 0, 0, two) end, "nmips")
    T.raises(function() g3.addTexture(1, 0, 2, two) end, "nmips")
    T.raises(function() g3.addTexture(1.5, 0, 1, two) end, "integer")
    T.raises(function() g3.addTexture(1, 0, 1, px(RED)) end, "expected 4 bytes")
    T.raises(function() g3.addTexture(1, 0, 1, {}) end, "string expected")
    -- Mip levels follow level 0, each half the last: 2 x 2 then 1 x 1.
    local t = g3.addTexture(1, 1, 2, px(RED, RED, RED, RED, BLUE))
    T.raises(function() g3.scrollTexture(t + 1, 0, 0) end, "no texture")
    T.raises(function() g3.scrollTexture(-1, 0, 0) end, "no texture")
    T.raises(function() g3.scrollTexture(0.5, 0, 0) end, "integer")
    T.raises(function() g3.scrollTexture(t, 0 / 0, 0) end, "NaN")
    T.raises(function() g3.setSkyImage(t + 1, 0) end, "no texture")
    T.raises(function() g3.setSkyImage(t, 3) end, "horizonRow")  -- 2 rows: 0..2
    g3.setSkyImage(t, 2)
    g3.setSkyImage(nil)

    local good = { xyz = { 0, 0, 0, 1, 0, 0, 0, 1, 0 }, uv = { 0, 0, 1, 0, 0, 1 },
                   color = 0xFFFF, idx = { 1, 2, 3 }, tex = t }
    local function with(k, v)
        local d = {}
        for a, b in pairs(good) do d[a] = b end
        d[k] = v
        return d
    end
    T.ok(g3.newTexturedMesh(good))
    T.ok(g3.newTexturedMesh(with("color", { 1, 2, 3 })))
    T.ok(g3.newTexturedMesh(with("tex", { t })))
    T.ok(g3.newTexturedMesh(with("flags", { g3.BLEND + g3.DOUBLE_SIDED + g3.NO_FOG })))
    T.raises(function() g3.newTexturedMesh(5) end, "table expected")
    T.raises(function() g3.newTexturedMesh(with("xyz", nil)) end, "xyz")
    T.raises(function() g3.newTexturedMesh(with("xyz", { 0, 0 })) end, "multiple of 3")
    T.raises(function() g3.newTexturedMesh(with("xyz", { 0, 0, 0, 1, 0, 0, 0, 1, 1 / 0 })) end, "NaN")
    T.raises(function() g3.newTexturedMesh(with("uv", { 0, 0, 1, 0 })) end, "two per vertex")
    T.raises(function() g3.newTexturedMesh(with("uv", { 0, 0, 1, "x", 0, 1 })) end, "number expected")
    T.raises(function() g3.newTexturedMesh(with("idx", { 1, 2 })) end, "multiple of 3")
    T.raises(function() g3.newTexturedMesh(with("idx", { 1, 2, 4 })) end, "out of range")
    T.raises(function() g3.newTexturedMesh(with("idx", { 0, 1, 2 })) end, "out of range")
    T.raises(function() g3.newTexturedMesh(with("color", { 0xFFFF })) end, "one colour per vertex")
    T.raises(function() g3.newTexturedMesh(with("color", 0x10000)) end, "colour")
    T.raises(function() g3.newTexturedMesh(with("tex", t + 1)) end, "no texture")
    T.raises(function() g3.newTexturedMesh(with("tex", { t, t })) end, "one texture per triangle")
    T.raises(function() g3.newTexturedMesh(with("flags", g3.UNLIT)) end, "flags")
    T.raises(function() g3.newTexturedMesh(with("flags", 16)) end, "flags")
    T.raises(function() g3.newTexturedMesh(with("flags", { 0, 0 })) end, "one flags value per triangle")

    local m = g3.newTexturedMesh(good)
    T.raises(function() g3.drawTextured(m, 0, 0, 0, 0, 0, 0) end, "beginScene")
    g3.beginScene(0)
    T.raises(function() g3.drawTextured(tri(-5), 0, 0, 0, 0, 0, 0) end, "texturedmesh expected")
    T.raises(function() g3.draw(m, 0, 0, 0, 0, 0, 0) end, "mesh expected")
    T.raises(function() g3.drawTextured(m, 0, 0, 0, 0, 0, 0, 0) end, "scale must be positive")
    T.raises(function() g3.drawTextured(m, 0, 0 / 0, 0, 0, 0, 0) end, "NaN")
    g3.endScene()

    -- A context holds 64 textures, slots 0 to 63.
    local last
    while true do
        local ok, slot = pcall(g3.addTexture, 0, 0, 1, px(RED))
        if not ok then
            T.ok(tostring(slot):find("64 textures"), tostring(slot))
            break
        end
        last = slot
    end
    T.eq(last, 63)
end)

T.done()
