-- Gamepad fixture (tests/e2e/test_gamepad.py). Logs the labels of a few
-- bindings, then every picocalc.gamepad edge with the input.* press mask of
-- the same frame (a bound key still reports as itself). Esc exits.

local pc = picocalc
local input, gp, log = pc.input, pc.gamepad, pc.sys.log

local NAMES = {
    {gp.PAD_UP, "up"}, {gp.PAD_DOWN, "down"}, {gp.PAD_LEFT, "left"},
    {gp.PAD_RIGHT, "right"}, {gp.PAD_A, "a"}, {gp.PAD_B, "b"},
    {gp.PAD_X, "x"}, {gp.PAD_Y, "y"}, {gp.PAD_L, "l"}, {gp.PAD_R, "r"},
    {gp.PAD_START, "start"}, {gp.PAD_SELECT, "select"},
}

local function names(mask)
    local out = {}
    for _, n in ipairs(NAMES) do
        if (mask & n[1]) ~= 0 then out[#out + 1] = n[2] end
    end
    return #out > 0 and table.concat(out, ",") or "-"
end

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Gamepad test", pc.display.WHITE)
pc.display.flush()

log(string.format("GP:LABEL a=%s a_alt=%s x=%s up_alt=%s select=%s",
    tostring(gp.getLabel(gp.PAD_A)), tostring(gp.getLabel(gp.PAD_A, 1)),
    tostring(gp.getLabel(gp.PAD_X, 0)), tostring(gp.getLabel(gp.PAD_UP, 1)),
    tostring(gp.getLabel(gp.PAD_SELECT))))
log("GP:BADARGS " .. tostring(pcall(gp.getLabel, gp.PAD_A | gp.PAD_B)) ..
    " " .. tostring(pcall(gp.getLabel, gp.PAD_A, 2)) ..
    " " .. tostring(pcall(gp.getLabel, 1.5)))
log("GP:READY")

local deadline = pc.sys.getTimeMs() + 20000
while pc.sys.getTimeMs() < deadline do
    input.update()
    local p, r = gp.getButtonsPressed(), gp.getButtonsReleased()
    if p ~= 0 then
        log(string.format("GP:PRESS %s held=%s btn=%d", names(p),
            names(gp.getButtons()), input.getButtonsPressed()))
    end
    if r ~= 0 then log("GP:RELEASE " .. names(r)) end
    if (input.getButtonsPressed() & input.BTN_ESC) ~= 0 then break end
    pc.sys.sleep(16)
end
log("GP:DONE")
