-- System-menu restore fixture (tests/e2e/test_menu_restore.py).
--
-- The two framebuffers hold different colours and the loop never draws, so
-- after the system menu closes the screen shows only what the OS gave back:
--   * buffer A is cleared red and flushed, then buffer B blue and flushed:
--     blue is on the panel and A (red) is the back buffer;
--   * Enter flushes without drawing, so the panel shows the back buffer
--     (red, if the menu gave it back intact);
--   * Esc returns to the launcher.
-- A menu item "Quit fixture" calls sys.exit() from inside the menu callback.
-- mem.txt records the PSRAM heap's free bytes at start, so a test can tell
-- whether an earlier run leaked.
-- On the device the idle dimmer must stay off: the injected menu key does
-- not count as activity, so on a dimmed screen the next key (the Esc meant
-- for the menu) would only wake the screen. The timer is reset at start and
-- on every loop pass, so however slow a run is, the dimmer never fires.
local pc = picocalc
local d = pc.display
local input = pc.input
local sys = pc.sys

sys.resetIdleTimer()

collectgarbage("collect")
local f = pc.fs.open(pc.fs.appPath("mem.txt"), "w")
pc.fs.write(f, tostring(sys.getMemInfo().psram_free))
pc.fs.close(f)

sys.addMenuItem("Quit fixture", function() sys.exit() end)

d.clear(d.RED)
d.flush()
d.clear(d.BLUE)
d.flush()
sys.log("MR:READY")

while true do
    sys.resetIdleTimer()
    input.update()
    local pressed = input.getButtonsPressed()
    if (pressed & input.BTN_ENTER) ~= 0 then
        d.flush()
        sys.log("MR:FLUSHED")
    end
    if (pressed & input.BTN_ESC) ~= 0 then
        sys.log("MR:EXIT")
        return
    end
    sys.sleep(20)
end
