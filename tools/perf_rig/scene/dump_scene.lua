-- Dumps the reference game's scene for the perf rig: runs its real render.lua
-- (and trackgen, trackmesh, models, camera) under host Lua with picocalc.gfx3d
-- stubbed, records every gfx3d call, and writes them as little-endian words.
--
--   lua dump_scene.lua <game dir> <out.bin> [frames] [lap seconds]
--
-- Ships are not simulated: the player runs the lap at a constant speed, and
-- the five rivals keep fixed offsets from it (RIVALS below). So the scene is
-- real, its motion an approximation. Format: see guest/gfx3d_main.c.
local root, out_path = arg[1], arg[2]
local nframes = tonumber(arg[3] or 60)
local lap_s = tonumber(arg[4] or 71)
package.path = root .. "/?.lua;" .. package.path

-- Metres ahead of the player, and lateral offset.
local RIVALS = { { 14, -2 }, { 32, 2 }, { -12, 1 }, { 60, -1 }, { 100, 2 } }

local function rgb(r, g, b)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
end

local meshes, lists, setup, ops = {}, {}, {}, nil
local g3 = { DOUBLE_SIDED = 1, UNLIT = 2, NO_FOG = 4 }
function g3.newMesh(v, t, c, f)
    local m = { index = #meshes, v = v, t = t, c = c, f = f }
    meshes[#meshes + 1] = m
    return m
end
function g3.setViewport(...) setup.viewport = { ... } end
function g3.setLight(...) setup.light = { ... } end
function g3.setSky(b) setup.sky = b end
function g3.setFog(...) setup.fog = { ... } end
function g3.setProjection(...) ops.proj = { ... } end
function g3.lookAt(...) ops.look = { ... } end
function g3.beginScene() end
function g3.endScene() end
function g3.drawBackground(m) ops[#ops + 1] = { 0, m.index } end
function g3.drawBasis(m, ...) ops[#ops + 1] = { 1, m.index, ... } end
function g3.drawList(l, bias, start)
    if not lists[l] then
        lists[#lists + 1] = l
        lists[l] = #lists - 1
    end
    ops[#ops + 1] = { 2, lists[l], bias or 0, start or 1 }
end
picocalc = { gfx3d = g3, display = { rgb = rgb } }

local game_len = 8 -- src/game.lua M.SEG_LEN
local trackgen = require("src.trackgen")
local track = require("src.track")
local camera = require("src.camera")
local physics = require("src.physics")
local render = require("src.render")
local tr = trackgen.generate(require("data.track1"), game_len)
local scene = render.init(tr)

-- One lap at 30 fps; `nframes` of them, evenly spaced, go in the file.
local total_steps = math.floor(lap_s * 30)
local every = math.max(1, total_steps // nframes)
local me = physics.new_ship(1, 0, 0)
me.v = tr.total / lap_s
local ships = { me }
for i = 2, 6 do ships[i] = physics.new_ship(1, 0, RIVALS[i - 1][2]) end
local cam = camera.new()
local frames = {}
for step = 0, total_steps - 1 do
    camera.update(cam, tr, me, 1 / 30)
    if step % every == 0 and #frames < nframes then
        for i = 2, 6 do
            local s = ships[i]
            local d = RIVALS[i - 1][1]
            s.seg, s.t = track.advance(tr, me.seg, me.t, d)
        end
        ops = {}
        render.draw(scene, tr, ships, cam)
        frames[#frames + 1] = ops
    end
    me.seg, me.t = track.advance(tr, me.seg, me.t, me.v / 30)
end

local buf = {}
local function u32(v) buf[#buf + 1] = string.pack("<I4", v) end
local function f32(v) buf[#buf + 1] = string.pack("<f", v) end

u32(0x3153524E) -- "NRS1"
u32(#meshes)
u32(#lists)
u32(#frames)
for _, v in ipairs(setup.viewport) do u32(v) end
for _, v in ipairs(setup.light) do f32(v) end
u32(#setup.sky)
for _, b in ipairs(setup.sky) do f32(b[1]); u32(b[2]) end
f32(setup.fog[1]); f32(setup.fog[2]); u32(setup.fog[3])

for _, m in ipairs(meshes) do
    local nv, nt = #m.v // 3, #m.t // 3
    u32(nv); u32(nt)
    for i = 1, #m.v do f32(m.v[i]) end
    for i = 1, #m.t do u32(m.t[i] - 1) end
    for t = 1, nt do
        u32(type(m.c) == "table" and m.c[t] or m.c)
        local fl = m.f
        if type(fl) == "table" then fl = fl[t] end
        u32(fl or 0)
    end
end
for i = 1, #lists do
    local l = lists[i]
    u32(#l)
    for k = 1, #l do u32(l[k].index) end
end
for _, fr in ipairs(frames) do
    for _, v in ipairs(fr.proj) do f32(v) end
    for _, v in ipairs(fr.look) do f32(v) end
    u32(#fr)
    for _, op in ipairs(fr) do
        u32(op[1])
        u32(op[2])
        if op[1] == 1 then
            -- x y z  fx fy fz  ux uy uz  scale bias sortAsOne
            for k = 3, 11 do f32(op[k]) end
            f32(op[12] or 1)
            f32(op[13] or 0)
            u32(op[14] and 1 or 0)
        elseif op[1] == 2 then
            f32(op[3])
            u32(op[4])
        end
    end
end
local f = assert(io.open(out_path, "wb"))
f:write(table.concat(buf))
f:close()

local nt_all = 0
for _, m in ipairs(meshes) do nt_all = nt_all + #m.t // 3 end
io.stderr:write(string.format("scene: %d meshes, %d tris in all, %d frames, lap %.0f m\n",
    #meshes, nt_all, #frames, tr.total))
