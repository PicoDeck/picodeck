-- Hello World app for PicoDeck
-- Demonstrates: display drawing, input handling, basic game loop, FPS tracking

local pc = picocalc          -- short alias for convenience
local disp = pc.display
local input = pc.input
local perf = pc.perf         -- performance tracking

-- App state
local frame = 0
local blink = true

-- Colours
local BG   = disp.BLACK
local FG   = disp.WHITE
local DIM  = disp.GRAY
local ACC  = disp.CYAN

-- ── Main loop ─────────────────────────────────────────────────────────────────
-- Your app owns the screen until it returns (exits to launcher).
-- Call input.update() once per frame to read key state.
-- No sleep needed for a demo that animates every frame: flush() limits it to ~40-60 fps.
-- It does keep the CPU busy, so an app waiting for a key should sleep: see
-- docs/Writing-Fast-Apps.md.

while true do
    perf.beginFrame()  -- Start FPS timing
    
    -- Poll input (reads STM32 FIFO — must be called once per frame)
    input.update()
    local pressed = input.getButtonsPressed()

    -- Esc = exit back to launcher
    if pressed & input.BTN_ESC ~= 0 then
        return
    end

    -- ── Draw frame ──────────────────────────────────────────────────────────
    disp.clear(BG)

    -- Title
    pc.ui.drawHeader("Hello, PicoCalc!")

    -- FPS display (built-in, easy!)
    perf.drawFPS()  -- defaults to the top-right corner, below the header
    -- Or customize: perf.drawFPS(10, 30)
    -- Or manual: local fps = perf.getFPS(); disp.drawText(x, y, "FPS: " .. fps, ...)

    -- Animated blink
    blink = (frame % 60) < 30
    if blink then
        disp.drawText(100, 140, "* Hello World *", FG, BG)
    end

    -- Battery & Time (for the footer)
    local bat = pc.sys.getBattery()
    local bat_str = (bat >= 0) and ("Bat: " .. bat .. "%  ") or ""
    local ms = pc.sys.getTimeMs()
    local secs = math.floor(ms / 1000)
    
    pc.ui.drawFooter("Press Esc to exit", bat_str .. "Uptime: " .. secs .. "s")

    -- Draw a bouncing box based on frame
    local bx = 8 + math.floor(math.abs(math.sin(frame / 60 * math.pi)) * 260)
    local by = 100
    disp.fillRect(bx, by, 20, 20, ACC)

    disp.flush()   -- waits for the previous frame's transfer (~16 ms)

    perf.endFrame()
    frame = frame + 1
end
