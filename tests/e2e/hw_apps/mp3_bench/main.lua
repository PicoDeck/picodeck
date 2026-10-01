-- MP3 decoding on Core 1 against a game's frame work on Core 0 (issue #28):
-- a gfx3d track strip (~800 triangles, the gfx3d bench's) drawn every frame,
-- paced by perf.setTargetFPS or not, with or without MP3 music, timing each
-- frame's work. tests/e2e/test_mp3_pacing.py (simulator) and
-- tests/e2e/test_mp3_contention_hw.py (device) run it.
--
-- /data/com.test.mp3_bench/bench.json configures one run:
--   music    file in this app's directory, looping ("" = no music)
--   seconds  how long to run (default 10)
--   fps      perf.setTargetFPS (0 = unpaced; default 30)
--   mode     "game" (default) or "idle" (no drawing: sys.sleep while the
--            music plays, to see Core 1's own XIP traffic)
--   segs     track segments (default 134, ~800 triangles)
--   warm     seconds left out of the statistics at the start (default 2)
-- and it writes bench_result.json: frames, the work per frame (avg, p50,
-- p95, max, in us, the drawing alone, without flush or the pacing wait),
-- `late` (frames whose work overran the target period), the MP3 position
-- and sample rate at the end, and whether the music was playing then.
local pc = picocalc
local g3, disp, sys, fs, json = pc.gfx3d, pc.display, pc.sys, pc.fs, pc.json
local DATA = "/data/" .. APP_ID
local now = sys.getTimeUs

local cfg = {}
local text = fs.readFile(DATA .. "/bench.json")
if text then cfg = json.decode(text) or {} end
local music = cfg.music or ""
local seconds = cfg.seconds or 10
local fps = cfg.fps or 30
local mode = cfg.mode or "game"
local segs = cfg.segs or 134
local warm_us = (cfg.warm or 2) * 1000000

local rgb = disp.rgb
local ROAD1, ROAD2 = rgb(90, 90, 100), rgb(120, 120, 130)
local WALL1, WALL2 = rgb(200, 60, 60), rgb(60, 200, 200)

local function build(nseg)
    local ring = {}
    for i = 0, nseg do
        local cx, cy, cz = math.sin(i * 0.12) * 18, math.sin(i * 0.07) * 5, -i * 6
        ring[i] = { { cx - 6, cy, cz }, { cx + 6, cy, cz }, { cx - 7, cy + 3, cz }, { cx + 7, cy + 3, cz } }
    end
    local chunks = {}
    for s0 = 0, nseg - 1, 8 do
        local verts, tris, cols, nv = {}, {}, {}, 0
        local function V(p)
            nv = nv + 1
            verts[#verts + 1] = p[1]; verts[#verts + 1] = p[2]; verts[#verts + 1] = p[3]
            return nv
        end
        local function Tri(a, b, c, col)
            tris[#tris + 1] = a; tris[#tris + 1] = b; tris[#tris + 1] = c
            cols[#cols + 1] = col
        end
        for i = s0, math.min(s0 + 7, nseg - 1) do
            local a, b = ring[i], ring[i + 1]
            local al, ar, alt, art = V(a[1]), V(a[2]), V(a[3]), V(a[4])
            local bl, br, blt, brt = V(b[1]), V(b[2]), V(b[3]), V(b[4])
            local road = (i % 2 == 0) and ROAD1 or ROAD2
            Tri(al, ar, br, road); Tri(al, br, bl, road)
            Tri(al, bl, blt, WALL1); Tri(al, blt, alt, WALL1)
            Tri(ar, art, brt, WALL2); Tri(ar, brt, br, WALL2)
        end
        chunks[#chunks + 1] = g3.newMesh(verts, tris, cols)
    end
    return chunks
end

local mp3
if music ~= "" then
    mp3 = pc.sound.mp3player()
    assert(mp3:load(APP_DIR .. "/" .. music), "cannot load " .. music)
    mp3:setVolume(50)
    mp3:play(0)  -- loop
end

local works, late = {}, 0
local period = fps > 0 and 1000000 // fps or 0
local t_start = now()
if mode == "idle" then
    while now() - t_start < seconds * 1000000 do sys.sleep(50) end
else
    local chunks = build(segs)
    g3.setViewport(0, 0, 320, 240)
    g3.setProjection(1.0, 0.5, 2000)
    g3.setSky({ { -1.0, rgb(40, 110, 60) }, { 0.0, rgb(250, 190, 130) }, { 0.1, rgb(70, 110, 220) } })
    g3.setFog(150, 900, rgb(250, 190, 130))
    collectgarbage("generational")
    pc.perf.setTargetFPS(fps)
    local f = 0
    while now() - t_start < seconds * 1000000 do
        f = f + 1
        pc.perf.beginFrame()
        local t0 = now()
        local z = -(f % 30) * 0.2
        g3.lookAt(0, 6, 10 + z, 0, 2, -30 + z)
        g3.beginScene()
        for c = 1, #chunks do g3.draw(chunks[c], 0, 0, 0, 0, 0, 0) end
        g3.endScene()
        local dt = now() - t0
        disp.flush()
        if t0 - t_start >= warm_us then
            works[#works + 1] = dt
            if period > 0 and dt > period then late = late + 1 end
        end
        pc.perf.endFrame()
    end
    pc.perf.setTargetFPS(0)
end

local result = { frames = #works, late = late, mode = mode, fps = fps, music = music }
if #works > 0 then
    local sum = 0
    for _, w in ipairs(works) do sum = sum + w end
    table.sort(works)
    result.work_avg = sum // #works
    result.work_p50 = works[math.ceil(#works * 0.50)]
    result.work_p95 = works[math.ceil(#works * 0.95)]
    result.work_max = works[#works]
end
if mp3 then
    result.mp3_playing = mp3:isPlaying()
    result.mp3_position = mp3:getPosition()
    result.mp3_rate = mp3:getSampleRate()
    mp3:stop()
end
local out = fs.open(DATA .. "/bench_result.json", "w")
fs.write(out, json.encode(result))
fs.close(out)
sys.log("mp3_bench done: " .. json.encode(result))
