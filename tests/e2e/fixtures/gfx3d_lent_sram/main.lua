-- A scene's records live in SRAM lent by the MP3 decoder until an MP3 session
-- starts, which can be between two draw calls of a scene. The records must move
-- out first: on the device Core 1 is decoding into that block by the time the
-- scene is rasterised. Every scene here must come out the same, pixel for pixel.
-- The harness stages music.mp3. See tests/e2e/test_gfx3d_lent_sram.py.

local pc = picocalc
local g3, sound, sys = pc.gfx3d, pc.sound, pc.sys
local T = sys.loadlib("picotest")
local fb = pc.display.getBackBuffer()

-- A slanted grid of 800 triangles, more than the lent block holds on either
-- target, so the scene has records in both homes.
local N <const> = 20
local verts, tris, cols, flags = {}, {}, {}, {}
for r = 0, N do
    for c = 0, N do
        verts[#verts + 1] = -4 + 8 * c / N
        verts[#verts + 1] = -3 + 6 * r / N
        verts[#verts + 1] = -5 - 6 * r / N
    end
end
for r = 0, N - 1 do
    for c = 0, N - 1 do
        local a = r * (N + 1) + c + 1
        local d = a + N + 1
        for _, i in ipairs({ a, a + 1, d + 1, a, d + 1, d }) do tris[#tris + 1] = i end
        for k = 0, 1 do
            cols[#cols + 1] = (0x1234 + 77 * (2 * (r * N + c) + k)) & 0xFFFF
            flags[#flags + 1] = g3.DOUBLE_SIDED | g3.UNLIT | g3.NO_FOG
        end
    end
end
local mesh = g3.newMesh(verts, tris, cols, flags)

g3.setViewport(0, 0, 320, 240)
g3.setProjection(1.0, 0.5, 100)
g3.lookAt(0, 0, 0, 0, 0, -1)

local function scene(between)
    g3.beginScene(0x0841)
    g3.draw(mesh, 0, 0, 0, 0, 0, 0)
    if between then between() end
    g3.draw(mesh, 0, 0.5, -1, 0, 0, 0)
    g3.endScene()
    return fb:getPixels(0, 0, 320, 240)
end

local want = scene()

T.case("the_scene_is_repeatable", function()
    T.ok(scene() == want, "the same scene drew different pixels")
    local st = g3.getStats()
    T.ok(st.drawn > 700, "drawn " .. tostring(st.drawn))
    T.eq(st.overflow, 0)
end)

local mp3 = sound.mp3player()

T.case("an_mp3_session_started_mid_scene", function()
    local got = scene(function()
        T.ok(mp3:load(APP_DIR .. "/music.mp3"), "load")
        T.ok(mp3:play(0), "play")
        sys.sleep(300) -- Core 1 decodes into the block meanwhile
    end)
    T.ok(mp3:isPlaying(), "it plays")
    T.ok(got == want, "the scene changed when the MP3 session took its SRAM")
end)

T.case("scenes_while_it_plays", function()
    for _ = 1, 3 do
        T.ok(scene() == want, "a scene changed beside the decoder")
        sys.sleep(50)
    end
end)

T.case("scenes_after_it_stops", function()
    mp3:stop()
    T.ok(scene() == want, "a scene changed after the session")
end)

T.done()
