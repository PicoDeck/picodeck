-- Boot splash still-frame fixture (tests/e2e/test_splash.py): draws the
-- splash's final frame through picocalc.ui.splash, logs SPLASH:READY, then
-- redraws it until Esc.
local ui, input, sys = picocalc.ui, picocalc.input, picocalc.sys
ui.splash("Loading...", "Please wait")
sys.log("SPLASH:READY")
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then break end
    ui.splash("Loading...", "Please wait")
    sys.sleep(20)
end
