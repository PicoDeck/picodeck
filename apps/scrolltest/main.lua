-- scrolltest — ST7365P vertical-scroll register probe.
--
-- The scroll registers (VSCRDEF 0x33 / VSCRSADD 0x37) had no consumers before
-- panels.lua, so their wrap behaviour on this panel is unverified silicon.
-- The ST7365P has 480 lines of frame memory driving a 320-line panel; this
-- app cycles three VSCRDEF configurations and sweeps the offset through two
-- full laps each.  A HUMAN must watch the panel:
--
--   phase 1 "DEFAULT"  — no VSCRDEF write (chip reset area, expected 0/480/0).
--                        Expect the image to slide up, then show garbage in
--                        the lower part once unwritten lines 320..479 scan in.
--   phase 2 "RING 480" — setScrollArea(0, 320, 160): sums to the 480-line
--                        frame memory, rings the visible panel mod 320.
--                        EXPECTED WINNER: clean wraparound, no garbage.
--   phase 3 "SUM 320"  — setScrollArea(0, 320, 0): matches the (likely wrong)
--                        old driver comment.  Behaviour unknown.
--
-- Whichever phase scrolls cleanly through both laps is the config panels.lua
-- should use.  Self-driving (no key input needed); B or Esc exits.

local disp  = picocalc.display
local input = picocalc.input
local sys   = picocalc.sys

local SCREEN = 320

local COLORS = {
    disp.rgb(200, 60, 60),  disp.rgb(60, 160, 60),  disp.rgb(60, 90, 200),
    disp.rgb(200, 160, 40), disp.rgb(150, 60, 180), disp.rgb(40, 180, 180),
    disp.rgb(120, 120, 120), disp.rgb(230, 230, 230),
    disp.rgb(140, 90, 40),  disp.rgb(200, 100, 160),
}

-- Ten labelled 32px bands + seam markers: a white line at GRAM row 0 and a
-- black line at GRAM row 319.  During a clean mod-320 wrap the two lines
-- stay adjacent as they travel; any gap, jump or noise between them means
-- the controller wrapped somewhere other than line 320.
local function drawPattern(label)
    for band = 0, 9 do
        local y = band * 32
        disp.fillRect(0, y, SCREEN, 32, COLORS[band + 1])
        disp.drawText(8, y + 6, "ROW " .. y, 0x0000, false)
        disp.drawText(8, y + 18, label, 0xFFFF, false)
    end
    disp.fillRect(0, 0, SCREEN, 2, 0xFFFF)
    disp.fillRect(0, 318, SCREEN, 2, 0x0000)
end

local function wantExit()
    input.update()
    -- B, or Esc (which the gamepad reads as B)
    return picocalc.gamepad.getNavPressed() & picocalc.gamepad.PAD_B ~= 0
end

local phases = {
    { label = "P1 DEFAULT AREA", area = nil },
    { label = "P2 RING 0/320/160", area = { 0, 320, 160 } },
    { label = "P3 SUM 0/320/0", area = { 0, 320, 0 } },
}

local pi = 1
while true do
    local ph = phases[pi]
    sys.log("SCROLLTEST:PHASE " .. ph.label)

    disp.setScrollOffset(0)
    if ph.area then
        disp.setScrollArea(ph.area[1], ph.area[2], ph.area[3])
    end
    drawPattern(ph.label)
    disp.flush()
    drawPattern(ph.label) -- both buffers, so nothing stale can flush later

    -- Hold static so the phase label is readable.
    local t0 = sys.getTimeMs()
    while sys.getTimeMs() - t0 < 1800 do
        if wantExit() then disp.setScrollOffset(0) return end
        sys.sleep(30)
    end

    -- Sweep two full laps at 2px per ~16ms tick.
    local off = 0
    for _ = 1, 320 do
        if wantExit() then disp.setScrollOffset(0) return end
        off = (off + 2) % 320
        disp.setScrollOffset(off)
        sys.sleep(16)
    end
    disp.setScrollOffset(0)

    pi = pi % #phases + 1
end
