-- Deterministic gfx3d scene for the golden-image test. No trigonometry
-- (lookAt / drawBasis only), so libm differences cannot move an edge.
local pc = picocalc
local g3, disp, gfx = pc.gfx3d, pc.display, pc.graphics
local rgb = disp.rgb
local SKY_TOP, SKY_MID = rgb(24, 40, 120), rgb(64, 96, 200)
local HORIZON, GROUND = rgb(250, 180, 120), rgb(30, 90, 50)
local ROAD1, ROAD2, BACKS = rgb(80, 80, 96), rgb(112, 112, 128), rgb(230, 60, 60)
local SHIP_TOP, SHIP_BOTTOM, MOUNTAIN = rgb(240, 240, 250), rgb(60, 200, 230), rgb(70, 60, 110)

g3.setViewport(0, 0, 320, 240)
g3.setProjection(1.0, 0.5, 400)
g3.setSky({ { -1.0, GROUND }, { 0.0, HORIZON }, { 0.08, SKY_MID }, { 0.35, SKY_TOP } })
g3.setFog(60, 200, HORIZON)
g3.setLight(0.4, 1.0, 0.3, 0.3)
g3.lookAt(0, 6, 6, 0, 3, -20)

-- Road over a crest: 12 segments of 8 units along -z, peak at segment 6.
local function h(i) local d = i - 6 return 6 - d * d / 6 end
local verts, tris, cols = {}, {}, {}
for i = 0, 12 do
    for _, x in ipairs({ -5, 5 }) do
        verts[#verts + 1] = x; verts[#verts + 1] = h(i); verts[#verts + 1] = -i * 8
    end
end
for i = 0, 11 do
    local l0, r0, l1, r1 = 2 * i + 1, 2 * i + 2, 2 * i + 3, 2 * i + 4
    local c = (i % 2 == 0) and ROAD1 or ROAD2
    for _, v in ipairs({ l0, r0, r1, l0, r1, l1 }) do tris[#tris + 1] = v end
    cols[#cols + 1] = c; cols[#cols + 1] = c
end
local road = g3.newMesh(verts, tris, cols)

local ground = g3.newMesh({ -400, -2, 50, 400, -2, 50, 400, -2, -400, -400, -2, -400 },
                          { 1, 2, 3, 1, 3, 4 }, GROUND)

-- Ship: a wedge, nose along -z (see the plan for the outward-normal check).
local ship = g3.newMesh(
    { 0, 0, -2,  -1.2, 0, 1,  1.2, 0, 1,  0, 0.6, 0.6,  0, -0.3, 0.8 },
    { 1, 2, 4,  1, 4, 3,  2, 3, 4,  1, 5, 2,  1, 3, 5,  2, 5, 3 },
    { SHIP_TOP, SHIP_TOP, BACKS, SHIP_BOTTOM, SHIP_BOTTOM, BACKS })

-- Mountain ring for drawBackground: an octagon of peaks at radius 300.
local ring_pts = { { 300, 0 }, { 212, 212 }, { 0, 300 }, { -212, 212 },
                   { -300, 0 }, { -212, -212 }, { 0, -300 }, { 212, -212 } }
local mv, mt = {}, {}
for i = 1, 8 do
    local a, b = ring_pts[i], ring_pts[i % 8 + 1]
    local base = #mv // 3
    for _, p in ipairs({ { a[1], -5, a[2] }, { b[1], -5, b[2] },
                         { (a[1] + b[1]) / 2, 40, (a[2] + b[2]) / 2 } }) do
        mv[#mv + 1] = p[1]; mv[#mv + 1] = p[2]; mv[#mv + 1] = p[3]
    end
    mt[#mt + 1] = base + 1; mt[#mt + 1] = base + 2; mt[#mt + 1] = base + 3
end
local mountains = g3.newMesh(mv, mt, MOUNTAIN, g3.DOUBLE_SIDED | g3.UNLIT)

-- A 2x2 four-colour marker image for the sprite (24-bit BMP, bottom-up).
local function le(n, b) local s = "" for i = 0, b - 1 do s = s .. string.char((n >> (8 * i)) & 0xFF) end return s end
local function px(r, g, b) return string.char(b, g, r) end
local rows = px(0, 0, 255) .. px(255, 255, 255) .. "\0\0" .. px(255, 0, 0) .. px(0, 255, 0) .. "\0\0"
local marker = gfx.image.loadFromBuffer("BM" .. le(54 + #rows, 4) .. le(0, 4) .. le(54, 4) .. le(40, 4)
    .. le(2, 4) .. le(2, 4) .. le(1, 2) .. le(24, 2) .. le(0, 4) .. le(#rows, 4) .. le(2835, 4)
    .. le(2835, 4) .. le(0, 4) .. le(0, 4) .. rows)

local SHIP_Z = -16
local ship_y = h(2) + 0.5
local function frame()
    g3.beginScene()
    g3.drawBackground(mountains)
    g3.draw(ground, 0, 0, 0, 0, 0, 0, 1, 1000)
    g3.draw(road, 0, 0, 0, 0, 0, 0)
    g3.drawBasis(ship, 0, ship_y, SHIP_Z, 0, h(3) - h(1), -16, 0, 1, 0, 1, -0.5, true)
    g3.drawSprite(marker, 4, h(9) + 2, -72, 3)
    g3.endScene()
    disp.fillRect(0, 240, 320, 80, disp.BLACK)
    disp.flush()
end

frame()
local sx, sy = g3.project(0, ship_y, SHIP_Z)
pc.sys.log(string.format("G3S:SHIP %d %d", math.floor(sx + 0.5), math.floor(sy + 0.5)))
while true do
    frame()  -- both buffers hold the same frame
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(30)
end
