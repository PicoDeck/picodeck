-- gfx3d frame cost on the device: a curving, rolling track strip in world
-- coordinates, split into 8-segment chunk meshes (6 triangles per segment),
-- with sky and fog, at ~200/400/800/1600 triangles. Writes bench.json.
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc
local g3, disp, sys = pc.gfx3d, pc.display, pc.sys
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
            Tri(al, ar, br, road); Tri(al, br, bl, road)            -- faces +y
            Tri(al, bl, blt, WALL1); Tri(al, blt, alt, WALL1)       -- faces +x (inward)
            Tri(ar, art, brt, WALL2); Tri(ar, brt, br, WALL2)       -- faces -x (inward)
        end
        chunks[#chunks + 1] = g3.newMesh(verts, tris, cols)
    end
    return chunks
end

g3.setViewport(0, 0, 320, 240)
g3.setProjection(1.0, 0.5, 2000)
g3.setSky({ { -1.0, rgb(40, 110, 60) }, { 0.0, rgb(250, 190, 130) }, { 0.1, rgb(70, 110, 220) } })
g3.setFog(150, 900, rgb(250, 190, 130))
collectgarbage("generational")

local results = {}
local now = sys.getTimeUs
for _, nseg in ipairs({ 34, 67, 134, 267 }) do
    local chunks = build(nseg)
    local frames, warm = 60, 5
    local cpu, cpu_max, geom, raster, drawn, culled, tris = 0, 0, 0, 0, 0, 0, 0
    local dts = {}
    for f = 1, frames + warm do
        local z = -(f % 30) * 0.2
        local t0 = now()
        g3.lookAt(0, 6, 10 + z, 0, 2, -30 + z)
        g3.beginScene()
        for c = 1, #chunks do g3.draw(chunks[c], 0, 0, 0, 0, 0, 0) end
        g3.endScene()
        local dt = now() - t0
        disp.flush()
        if f > warm then
            local s = g3.getStats()
            cpu = cpu + dt
            if dt > cpu_max then cpu_max = dt end
            dts[#dts + 1] = dt
            geom = geom + s.us_geom
            raster = raster + s.us_raster
            drawn = drawn + s.drawn
            culled = culled + s.culled
            tris = s.tris_in
        end
    end
    table.sort(dts)
    local cpu_median = dts[math.ceil(#dts / 2)]
    results[#results + 1] = string.format(
        '{"tris":%d,"cpu_us_avg":%d,"cpu_us_median":%d,"cpu_us_max":%d,"geom_us_avg":%d,"raster_us_avg":%d,"drawn_avg":%d,"culled_avg":%d}',
        tris, cpu // frames, cpu_median, cpu_max, geom // frames, raster // frames, drawn // frames, culled // frames)
    T.case("bench_" .. nseg, function() T.ok(tris > 0) end)
end
local f = pc.fs.open("/data/" .. APP_ID .. "/bench.json", "w")
pc.fs.write(f, "[" .. table.concat(results, ",") .. "]")
pc.fs.close(f)
T.done()
