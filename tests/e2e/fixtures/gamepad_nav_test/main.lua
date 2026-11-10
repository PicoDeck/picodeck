-- Gamepad navigation fixture (tests/e2e/test_gamepad_nav.py). Logs every
-- non-zero gamepad.getNavPressed() and getButtonsRepeated() mask with the
-- time, reading each twice per frame (both reads must agree). X turns
-- repeat off (input.setRepeat(0)); Y clears input state (input.clearState()).
-- Esc exits after its frame is logged.
local pc = picocalc
local input, gp, log = pc.input, pc.gamepad, pc.sys.log

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Gamepad nav test", pc.display.WHITE)
pc.display.flush()

log("NAV:READY " .. tostring(gp.getNavPressed ~= nil) .. " "
    .. tostring(gp.getButtonsRepeated ~= nil))

local deadline = pc.sys.getTimeMs() + 20000
while pc.sys.getTimeMs() < deadline do
    input.update()
    local nav, rep = gp.getNavPressed(), gp.getButtonsRepeated()
    local nav2, rep2 = gp.getNavPressed(), gp.getButtonsRepeated()
    local t = pc.sys.getTimeMs()
    if nav ~= nav2 or rep ~= rep2 then
        log(string.format("NAV:UNSTABLE %d/%d %d/%d", nav, nav2, rep, rep2))
    end
    if nav ~= 0 then log(string.format("NAV:NAV %d t=%d", nav, t)) end
    if rep ~= 0 then log(string.format("NAV:REP %d t=%d", rep, t)) end
    if gp.getButtonsPressed() & gp.PAD_X ~= 0 then
        input.setRepeat(0)
        log("NAV:NOREPEAT")
    end
    if gp.getButtonsPressed() & gp.PAD_Y ~= 0 then
        input.clearState()
        log("NAV:CLEARED")
    end
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then break end
    pc.sys.sleep(16)
end
log("NAV:DONE")
