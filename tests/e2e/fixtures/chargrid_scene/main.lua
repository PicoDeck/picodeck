-- chargrid golden-image fixture (tests/e2e/test_chargrid_scene.py): a 53x40
-- RPG-style screen at 6x8 with text, boxes in every style, every generated
-- shape, palette swatches, a dot ramp from shadeRow and stamped art. The
-- water palette-cycles for a few frames, settles on its first colour, logs
-- CGS:READY and then redraws that same frame every loop until Esc.
local pc = picocalc
local CG, disp, input, sys = pc.chargrid, pc.display, pc.input, pc.sys

local g = CG.new(53, 40, 0)
g:clear(" ", 7, 0)
g:box(0, 0, 53, 40, CG.BOX_SINGLE, 14, 0)
g:print(2, 1, "chargrid golden scene", 15)
for code = 1, 27 do
    local row, col = code <= 14 and 3 or 5, ((code - 1) % 14) * 3 + 2
    g:set(col, row, code, 11, 0)
end
for i = 0, 47 do g:set(2 + i, 7, CG.SHADE4, 16 + i * 4) end
g:print(2, 9, "╔══╗ ┌─┬─┐ ╭─╮ ▲▶▼◀ ░▒▓█ ·•●⬤ é", 10)
g:printWrapped(2, 11, 30, "The quick brown fox jumps over the lazy dog, twice: once on the left and once more for luck.", 13)
g:box(34, 11, 17, 6, CG.BOX_ASCII, 9, 0)
g:box(36, 12, 13, 4, CG.BOX_BLOCK, 12, 0)
g:fill(2, 18, 49, 4, "~", 250, 17)
g:setRamp("\0\1\2\3\4")
g:setColorRamp({ 17, 18, 19, 20, 21, 27, 33, 39, 45, 51, 87, 123, 159, 195, 231 })
local lv = {}
for x = 1, 49 do lv[x] = (x - 1) / 48 end
for y = 23, 27 do g:shadeRow(y, lv, nil, 2) end
g:setRamp(" .:-=+*#%@", true)
for y = 28, 30 do g:shadeRow(y, lv, nil, 2) end
g:stamp(2, 32, "  /\\_/\\\n ( o.o )\n  > ^ <", 11, nil, " ")
g:print(30, 33, "HP ##########", 9)
g:print(30, 34, "MP ######", 12)
g:print(2, 38, "Esc quits", 8)

local CYCLE = { disp.rgb(20, 60, 160), disp.rgb(30, 90, 200), disp.rgb(60, 140, 230), disp.rgb(30, 90, 200) }
local function frame()
    disp.clear(0)
    g:draw(1, 0)
    disp.flush()
end
for f = 1, 24 do
    g:setColor(250, CYCLE[f % 4 + 1])
    frame()
end
g:setColor(250, CYCLE[1])
frame()
sys.log("CGS:READY")
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then break end
    frame()
    sys.sleep(20)
end
