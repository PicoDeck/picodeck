-- Font demo: the D-pad's Left/Right cycle fonts, B or Esc exits.
local pc, disp, input, gfx = picocalc, picocalc.display, picocalc.input, picocalc.graphics
local BLACK, WHITE, GRAY = 0x0000, 0xFFFF, disp.rgb(128, 128, 128)
local gp = picocalc.gamepad
local CYAN, YELLOW = disp.rgb(0, 255, 255), disp.rgb(255, 255, 0)
local PARA = "PicoDeck fonts: four built-ins live in flash and any .pfn on the SD card " ..
             "loads into PSRAM. Proportional fonts carry an advance per glyph, so " ..
             "measurement, alignment and wrapping all use real widths."

local fonts = {
  { obj = gfx.font.new("6x8") },
  { obj = gfx.font.new("8x12") },
  { obj = gfx.font.new("scientifica") },
  { obj = gfx.font.new("scientifica-bold") },
}
local ok, loaded = pcall(gfx.font.new, APP_DIR .. "/fonts/demo_prop.pfn")
if ok then fonts[#fonts + 1] = { obj = loaded, loaded = true }
else pc.sys.log("fonttest: load failed: " .. tostring(loaded)) end

local function draw(i)
  local f = fonts[i].obj
  local fw, fh = f:getWidth(), f:getHeight()
  disp.clear(BLACK)
  local y = 4
  f:drawText(4, y, ("%d/%d %s  %dx%d%s"):format(i, #fonts, f:getName():match("[^/]+$"),
                                                fw, fh, fonts[i].loaded and "  (loaded)" or ""), CYAN, BLACK)
  y = y + fh + 6
  local sample = "The quick brown fox jumps over the lazy dog 0123456789"
  local w = f:drawText(4, y, sample, WHITE, BLACK)
  y = y + fh + 2
  f:drawText(4, y, ("textWidth=%d  iiiii=%d  WWWWW=%d"):format(w, f:getTextWidth("iiiii"),
                                                            f:getTextWidth("WWWWW")), GRAY, BLACK)
  y = y + fh + 8
  disp.drawRect(4, y, 312, 7 * fh + 4, GRAY)
  f:drawTextInRect(6, y + 2, 308, 7 * fh, PARA, 0, WHITE, BLACK)
  y = y + 7 * fh + 12
  f:drawTextAligned(160, y, "centered", 1, YELLOW, BLACK)
  y = y + fh + 2
  f:drawTextAligned(316, y, "right aligned", 2, YELLOW, BLACK)
  y = y + fh + 8
  if f:getName():match("scientifica") then
    local s = {}
    for c = 0x80, 0x9F do s[#s + 1] = string.char(c) end
    f:drawText(4, y, table.concat(s), WHITE, BLACK)
    y = y + fh + 4
  end
  disp.setFont(disp.FONT_6X8)
  disp.drawText(4, 308, (gp.getLabel(gp.PAD_LEFT) or "?") .. "/" ..
                (gp.getLabel(gp.PAD_RIGHT) or "?") .. ": font   Esc: exit", GRAY, BLACK)
  disp.flush()
end

local i = 1
draw(i)
while true do
  input.update()
  local nav = gp.getNavPressed()
  if (nav & gp.PAD_B) ~= 0 then break end
  if (nav & gp.PAD_RIGHT) ~= 0 then i = i % #fonts + 1; draw(i) end
  if (nav & gp.PAD_LEFT) ~= 0 then i = (i - 2) % #fonts + 1; draw(i) end
  pc.sys.sleep(10)
end
