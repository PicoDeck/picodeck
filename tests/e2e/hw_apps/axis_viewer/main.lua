-- What a real controller reads: the six axes as numbers and bars, the held
-- PAD_* buttons, redrawn every frame. For checking a Bluetooth pad by hand
-- (issue #68); no test launches it. Esc exits.
local pc = picocalc
local d, gp, input = pc.display, pc.gamepad, pc.input

local AXES = {
    {"LX", gp.AXIS_LX}, {"LY", gp.AXIS_LY}, {"RX", gp.AXIS_RX},
    {"RY", gp.AXIS_RY}, {"LT", gp.AXIS_LT}, {"RT", gp.AXIS_RT},
}
local BUTTONS = {
    {"up", gp.PAD_UP}, {"down", gp.PAD_DOWN}, {"left", gp.PAD_LEFT},
    {"right", gp.PAD_RIGHT}, {"a", gp.PAD_A}, {"b", gp.PAD_B}, {"x", gp.PAD_X},
    {"y", gp.PAD_Y}, {"l", gp.PAD_L}, {"r", gp.PAD_R}, {"start", gp.PAD_START},
    {"select", gp.PAD_SELECT},
}

local BAR_X, BAR_W, MID = 100, 200, 200

while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    pc.sys.resetIdleTimer()
    d.clear(d.BLACK)
    d.drawText(10, 10, "Axis viewer (Esc exits)", d.WHITE)
    for i, a in ipairs(AXES) do
        local v, y = gp.getAxis(a[2]), 20 + i * 24
        d.drawText(10, y, string.format("%s %6.3f", a[1], v), d.WHITE)
        d.drawRect(BAR_X, y - 2, BAR_W, 12, d.GRAY)
        if i <= 4 then
            local w = math.floor(math.abs(v) * BAR_W / 2 + 0.5)
            d.fillRect(v < 0 and MID - w or MID, y, w, 8, d.GREEN)
        else
            d.fillRect(BAR_X, y, math.floor(v * BAR_W + 0.5), 8, d.GREEN)
        end
    end
    local held, names = gp.getButtons(), {}
    for _, b in ipairs(BUTTONS) do
        if held & b[2] ~= 0 then names[#names + 1] = b[1] end
    end
    d.drawText(10, 200, "held: " .. table.concat(names, " "), d.WHITE)
    d.flush()
end
