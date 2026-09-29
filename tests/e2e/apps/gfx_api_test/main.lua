-- Graphics/display bridge argument checks (issues #38, #40, #41, #42).
-- picotest kit; tests/e2e/test_gfx_api.py runs it.

local pc = picocalc
local gfx, disp = pc.graphics, pc.display
local T = pc.sys.loadlib("picotest")
local PFN = APP_DIR .. "/demo_prop.pfn"

local function img(w, h) return gfx.image.new(w, h) end

-- Every font this app loads is released between cases.
local held = {}
local function release()
    for _, id in ipairs(held) do disp.unloadFont(id) end
    held = {}
    collectgarbage("collect")
end
-- Runs fn, then releases the fonts it held (also when it fails).
local function guarded(fn)
    return function() local ok, e = pcall(fn); release(); if not ok then error(e, 0) end end
end

-- ── #38 drawScaled / drawScaledNN ───────────────────────────────────────────
T.case("drawScaled_rejects_bad_scales", guarded(function()
    local i = img(8, 8)
    T.raises(function() i:drawScaled(0, 0, 0) end, "scale multiplier")
    T.raises(function() i:drawScaled(0, 0, -2) end, "scale multiplier")
    T.raises(function() i:drawScaled(0, 0, 0/0) end, "scale multiplier")
    T.raises(function() i:drawScaled(0, 0, math.huge) end, "scale multiplier")
    T.raises(function() i:drawScaled(0, 0, 1e6) end, "scale multiplier")
    -- past TGX's 4096 px rasteriser limit
    T.raises(function() img(96, 96):drawScaled(0, 0, 50) end, "scale multiplier")
    T.raises(function() img(64, 64):drawScaled(0, 0, 65) end, "scale multiplier")
    T.raises(function() i:drawScaled(0, 0, 1, math.huge) end, "NaN or infinite")
end))

T.case("drawScaled_dst_size_style_call", guarded(function()
    -- the #38 call: a 96x96 image "scaled to 182x90" is a 182x multiplier.
    local i = img(96, 96)
    local e = T.raises(function() i:drawScaled(10, 10, 182, 90) end, "scale multiplier")
    T.ok(tostring(e):find("dst_w"), "message should say it is not dst_w/dst_h: " .. tostring(e))
end))

T.case("drawScaled_large_zoom_still_works", guarded(function()
    local i = img(8, 8)
    i:drawScaled(-1000, -1000, 200)      -- 1600 px, nearly all off-screen
    i:drawScaled(0, 0, 0.5, 0.3)
    i:drawScaled(0, 0, 1e-3)
    img(64, 64):drawScaled(-100, -100, 64)    -- 4096 px, exactly the cap
end))

T.case("drawScaledNN_rejects_bad_scales", guarded(function()
    local i = img(8, 8)
    T.raises(function() i:drawScaledNN(0, 0, 0) end, "scale multiplier")
    T.raises(function() i:drawScaledNN(0, 0, -1) end, "scale multiplier")
    T.raises(function() i:drawScaledNN(0, 0, 20000) end, "scale multiplier")
    i:drawScaledNN(0, 0, 3)
    i:drawScaledNN(-5000, -5000, 2000)     -- 16000 px, clipped span only
end))

-- ── #42 drawStretched srcRect ───────────────────────────────────────────────
T.case("drawStretched_srcRect_forms", guarded(function()
    local i = img(8, 8)
    i:drawStretched(0, 0, 16, 16, { x = 2, y = 2, w = 4, h = 4 })
    i:drawStretched(0, 0, 16, 16, { 2, 2, 4, 4 })
    i:drawStretched(0, 0, 16, 16, { x = 2 })       -- omitted fields default
    i:drawStretched(0, 0, 16, 16, { 2, 2 })        -- positional, w/h default
    i:drawStretched(0, 0, 16, 16)
    i:draw(0, 0, false, { 2, 2, 4, 4 })    -- img:draw shares the parser
    i:draw(0, 0, false, { x = 2, y = 2, w = 4, h = 4 })
end))

T.case("drawStretched_srcRect_errors_name_the_parameter", guarded(function()
    local i = img(8, 8)
    T.raises(function() i:drawStretched(0, 0, 16, 16, { x = "a" }) end, "srcRect%.x")
    T.raises(function() i:drawStretched(0, 0, 16, 16, { 0, 0, "b" }) end, "srcRect%.w")
    T.raises(function() i:drawStretched(0, 0, 16, 16, { y = 0/0 }) end, "srcRect%.y")
    T.raises(function() i:drawStretched(0, 0, 16, 16, { x = 0, h = {} }) end, "srcRect%.h")
    T.raises(function() i:drawStretched(0, 0, 16, 16, 5) end, "table expected")
    T.raises(function() i:draw(0, 0, false, { x = "a" }) end, "srcRect%.x")
end))

-- ── #41 font.new messages ───────────────────────────────────────────────────
T.case("font_new_bare_name", guarded(function()
    local e = tostring(T.raises(function() gfx.font.new("nosuchfont") end, "nosuchfont"))
    T.ok(not e:find("access denied"), e)
    T.ok(e:find("built%-in"), e)
    T.ok(e:find("scientifica%-bold"), e)
end))

T.case("font_new_path_like", guarded(function()
    local e = tostring(T.raises(function() gfx.font.new(APP_DIR .. "/missing.pfn") end, "missing%.pfn"))
    T.ok(not e:find("built%-in"), e)
    T.ok(e:find("load"), e)
    local e2 = tostring(T.raises(function() gfx.font.new("/system/config.json") end, "config%.json"))
    T.ok(e2:find("access denied"), e2)       -- a real path outside the sandbox
    local e3 = tostring(T.raises(function() gfx.font.new("thing.pfn") end, "thing%.pfn"))
    T.ok(not e3:find("built%-in"), e3)       -- ends in .pfn: treated as a path
end))

T.case("font_new_builtin_names", guarded(function()
    for _, n in ipairs({ "6x8", "8x12", "scientifica", "scientifica-bold" }) do
        T.eq(gfx.font.new(n):getName(), n)
    end
end))

-- ── #40 registry ────────────────────────────────────────────────────────────
T.case("loadFont_failure_is_nil_and_message", guarded(function()
    local id, err = disp.loadFont(APP_DIR .. "/missing.pfn")
    T.eq(id, nil)
    T.ok(type(err) == "string" and err:find("missing%.pfn"), tostring(err))
    local id2, err2 = disp.loadFont("/system/config.json")
    T.eq(id2, nil)
    T.ok(type(err2) == "string" and err2:find("access denied"), tostring(err2))
end))

T.case("registry_full_is_nil_errstr_and_shared", guarded(function()
    -- 4 through loadFont and 4 through font.new use all 8 slots.
    local fonts = {}
    for _ = 1, 4 do
        local id, err = disp.loadFont(PFN)
        T.ok(id, tostring(err))
        held[#held + 1] = id
        fonts[#fonts + 1] = gfx.font.new(PFN)
    end
    local id, err = disp.loadFont(PFN)
    T.eq(id, nil)
    T.ok(type(err) == "string" and err:find("full") and err:find("8"), tostring(err))
    T.ok(err:find("font%.new") or err:find("graphics"), "should say the slots are shared: " .. err)
    local e = tostring(T.raises(function() gfx.font.new(PFN) end, "full"))
    T.ok(not e:find("access denied"), e)
    -- freeing one slot makes room again
    disp.unloadFont(table.remove(held))
    local id2, err2 = disp.loadFont(PFN)
    T.ok(id2, tostring(err2))
    held[#held + 1] = id2
    fonts = nil
end))

T.done()
