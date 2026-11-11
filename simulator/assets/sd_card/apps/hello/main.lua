-- Hello World app for PicoDeck
-- Demonstrates: display drawing, gamepad input, basic game loop, FPS tracking

local pc = picocalc          -- short alias for convenience
local disp = pc.display
local input = pc.input
local gp = pc.gamepad        -- the logical gamepad: keys, rebound keys or a controller
local perf = pc.perf         -- performance tracking

-- App state
local frame = 0
local blink = true
local paused = false

-- Colours
local BG   = disp.BLACK
local FG   = disp.WHITE
local DIM  = disp.GRAY
local ACC  = disp.CYAN

-- ── Main loop ─────────────────────────────────────────────────────────────────
-- Your app owns the screen until it returns (exits to launcher).
-- Call input.update() once per frame: it reads the keyboard and the gamepad.
-- No sleep needed for a demo that animates every frame: flush() limits it to ~40-60 fps.
-- It does keep the CPU busy, so an app waiting for a key should sleep: see
-- docs/Writing-Fast-Apps.md.

while true do
    perf.beginFrame()  -- Start FPS timing
    
    -- Poll input (reads STM32 FIFO — must be called once per frame)
    input.update()

    -- getNavPressed(): the D-pad (repeating while held), A, B and the other
    -- buttons' presses, with Enter read as A and Esc as B. One call drives
    -- the app from the keyboard, rebound keys or a Bluetooth controller.
    local nav = gp.getNavPressed()

    -- B or Esc = exit back to launcher
    if nav & gp.PAD_B ~= 0 then
        return
    end

    -- A or Enter = pause / resume the animation
    if nav & gp.PAD_A ~= 0 then
        paused = not paused
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
    
    -- Name a gamepad button by its key with getLabel(), so the hint stays
    -- right after the player rebinds it (nil when A is left unbound).
    local a_key = gp.getLabel(gp.PAD_A) or "?"
    pc.ui.drawFooter(a_key .. ": pause  Esc: exit", bat_str .. "Uptime: " .. secs .. "s")

    -- Draw a bouncing box based on frame
    local bx = 8 + math.floor(math.abs(math.sin(frame / 60 * math.pi)) * 260)
    local by = 100
    disp.fillRect(bx, by, 20, 20, ACC)

    disp.flush()   -- waits for the previous frame's transfer (~16 ms)

    perf.endFrame()
    if not paused then
        frame = frame + 1
    end
end
