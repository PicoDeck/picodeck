-- chargrid shader golden-image fixture (tests/e2e/test_chargrid_shader_scene.py):
-- an 80x40 scene of 4x4 dot cells. An aurora-class sky shader (fbm, noise,
-- exp, sin, smooth, a dithered dot ramp, r,g,b through the nearest palette
-- entry) runs over a land layer it keeps; a torch shader (dist, tone through
-- the colour ramp) runs on the land only. The time is fixed, so the frame is
-- deterministic. Logs CGSH:READY, then redraws that frame until Esc.
local pc = picocalc
local CG, N, disp, input, sys = pc.chargrid, pc.noise, pc.display, pc.input, pc.sys

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
layer land
keep = land.glyph == 0
tone = clamp(1 - dist(x, y, px, py) / 8, 0, 1)
]]

local g, land = CG.new(80, 40, nil, 4, 4), CG.new(80, 40, nil, 4, 4)
g:setRamp("\0\1\2\3\4", true)
g:setColorRamp({ 236, 237, 238, 94, 130, 172, 214, 220 })
for x = 0, 79 do
    local top = math.floor(24 + 9 * N.fbm(x * 0.05, 1.5, 3))
    land:fill(x, top, 1, 40 - top, CG.SHADE2, 236, 0)
end
g:blit(land, 0, 0, 80, 40, 0, 0)
local edge = {}
for x = 0, 79 do edge[x + 1] = 16 + 6 * math.sin(x * 0.07) end
local sky = assert(CG.shader(AURORA))
sky:set("t", 12.5)
sky:setArray("edge", edge)
sky:run(g, land)
local torch = assert(CG.shader(TORCH))
torch:set("px", 52)
torch:set("py", 33)
torch:run(g, land)

local function frame()
    disp.clear(0)
    g:draw(0, 80)
    disp.flush()
end
frame()
sys.log("CGSH:READY")
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then break end
    frame()
    sys.sleep(20)
end
