picocalc.ui.drawHeader("Graphics API Test")

-- Load testing images
local img_path = picocalc.fs.appPath("test.bmp")
local test_img = nil

-- Create file to make sure appPath worked and directory exists
if not picocalc.fs.exists(img_path) then
    -- We can just draw directly on the screen for now, since we haven't created the bmp generator.
    -- Or the user can manually copy an image.
end

-- Try loading it, we will put one there manually using python
local success, err = pcall(function()
    test_img = picocalc.graphics.image.load(img_path)
end)

local x = 0

function update()
    picocalc.perf.beginFrame()
    picocalc.input.update()
    
    -- Clear with custom color
    picocalc.graphics.setBackgroundColor(picocalc.display.BLACK)
    picocalc.graphics.clear()
    
    picocalc.ui.drawHeader("Graphics API Test")
    
    if success and test_img then
        local w, h = test_img:getSize()
        
        -- Draw centered
        test_img:drawAnchored(160, 160, 0.5, 0.5)
        
        -- Draw flipped
        test_img:draw(160, 200, {flipX=true})
        
        -- Draw Sub-rect
        test_img:draw(10, 200, false, {x=0, y=0, w=w/2, h=h/2})
        
        -- Draw tiled
        test_img:drawTiled(0, 30, 100, 50)
        
        -- Scroll across screen
        test_img:draw(x, 100)
    else
        picocalc.display.drawText(10, 50, "Failed to load test.bmp", picocalc.display.RED)
    end
    
    x = x + 1
    if x > 320 then x = -64 end
    
    -- B or Esc exits (the gamepad: keys, rebound keys or a controller)
    local gp = picocalc.gamepad
    if gp.getNavPressed() & gp.PAD_B ~= 0 then
        picocalc.sys.exit()
    end
    
    picocalc.perf.drawFPS(250, 4)
    picocalc.display.flush()
    picocalc.perf.endFrame()
end

while true do
    update()
end
