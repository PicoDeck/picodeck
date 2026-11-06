-- Panels Demo — interactive-comics framework showcase.
--
-- Everything interesting lives in comic.lua (the data) and the shared
-- /system/lib/panels.lua library. This file is deliberately the whole of what
-- a comic app needs to be.

local pc  = picocalc
local sys = pc.sys

local ok, Panels = pcall(sys.loadlib, "panels")
if not ok or type(Panels) ~= "table" then
    -- /system/lib is a folder on the card that nothing installs (a published
    -- app would ship panels.lua itself and require it). Say so instead of a
    -- blank screen.
    local disp = pc.display
    while true do
        pc.input.update()
        if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
        disp.clear(disp.rgb(20, 0, 0))
        disp.drawText(20, 140, "panels library not found", disp.rgb(255, 255, 255))
        disp.drawText(20, 156, "copy system/lib to the SD card",
                      disp.rgb(200, 200, 200))
        disp.drawText(20, 172, "(needs /system/lib/panels.lua)",
                      disp.rgb(200, 200, 200))
        disp.drawText(20, 300, "ESC exit", disp.rgb(160, 160, 160))
        disp.flush()
    end
end

local result, err = Panels.start(require("comic"), { resume = true })
sys.log("PANELS_DEMO: exited with " .. tostring(result)
        .. (err and (" (" .. tostring(err) .. ")") or ""))
