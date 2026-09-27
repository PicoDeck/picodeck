-- Draws a 2x2 image stretched and a sub-rect of it; the harness probes pixels.
local pc = picocalc
local disp, gfx = pc.display, pc.graphics
local RED, GREEN, BLUE, WHITE = disp.rgb(255, 0, 0), disp.rgb(0, 255, 0), disp.rgb(0, 0, 255), disp.rgb(255, 255, 255)

-- 2x2 BMP: bottom-up rows. Top row red, green; bottom row blue, white.
local function bmp2x2()
  local function le(n, b) local s = "" for i = 0, b - 1 do s = s .. string.char((n >> (8 * i)) & 0xFF) end return s end
  local function px(r, g, b) return string.char(b, g, r) end
  local rows = px(0, 0, 255) .. px(255, 255, 255) .. "\0\0" .. px(255, 0, 0) .. px(0, 255, 0) .. "\0\0"
  return "BM" .. le(54 + #rows, 4) .. le(0, 4) .. le(54, 4) .. le(40, 4) .. le(2, 4) .. le(2, 4)
      .. le(1, 2) .. le(24, 2) .. le(0, 4) .. le(#rows, 4) .. le(2835, 4) .. le(2835, 4) .. le(0, 4) .. le(0, 4) .. rows
end
local img = gfx.image.loadFromBuffer(bmp2x2())

local ok1 = not pcall(img.drawStretched, img, 0, 0, 10)                      -- h missing
local ok2, e2 = pcall(img.drawStretched, img, 0, 0, 10, 10, { x = "a" })
ok2 = (not ok2) and tostring(e2):find("field 'x'") ~= nil
pc.sys.log("ST:ERRORS " .. ((ok1 and ok2) and "ok" or ("bad " .. tostring(e2))))

-- Redrawn every frame (not just re-flushed): display_flush() swaps between
-- two hardware framebuffers, so a flush with no draw in between alternates
-- onto whichever buffer was last drawn two frames ago (stale/undrawn here).
local function draw()
  disp.clear(disp.BLACK)
  img:drawStretched(0, 0, 100, 60)                                  -- whole image
  img:drawStretched(120, 0, 30, 30, { x = 1, y = 1, w = 1, h = 1 }) -- white only
  img:drawStretched(160, 0, 0, 30)                                  -- empty: nothing
  img:drawStretched(200, 0, 30, 30, { x = 5, y = 5 })               -- off-image: nothing
  disp.flush()
end

draw()
pc.sys.log("ST:READY")
while true do
  pc.input.update()
  if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
  draw()
  pc.sys.sleep(20)
end
