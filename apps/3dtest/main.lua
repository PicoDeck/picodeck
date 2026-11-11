-- 3D PicoCalc Model Demo
-- Tests display performance with real-time 3D graphics

local pc = picocalc
local disp = picocalc.display
local gfx = picocalc.graphics
local input = picocalc.input
local perf  = picocalc.perf
local gp    = picocalc.gamepad

-- The key bound to a gamepad button, for the on-screen help.
local function pad_key(btn) return gp.getLabel(btn) or "?" end

-- Screen centre
local cx, cy = 160, 160

-- PicoCalc model vertices (scaled up from [-1,1] to ~100 units)
-- Body: -100 to 100 in X/Y, +/- 15 in Z
-- Screen area: center X, upper Y
-- D-pad: left side
-- Buttons: right side
local vertices = {
    -- Main body - front (1-4)
    -100, -100, 15,
    100, -100, 15,
    100, 100, 15,
    -100, 100, 15,
    -- Main body - back (5-8)
    -100, -100, -15,
    100, -100, -15,
    100, 100, -15,
    -100, 100, -15,
    -- Screen bezel front (9-12)
    -60, -10, 16,
    60, -10, 16,
    60, 50, 16,
    -60, 50, 16,
    -- Screen display (13-16)
    -50, -2, 13,
    50, -2, 13,
    50, 42, 13,
    -50, 42, 13,
    -- D-pad vertical (17-20)
    -75, -55, 16,
    -65, -55, 16,
    -65, -45, 16,
    -75, -45, 16,
    -- D-pad horizontal (21-24)
    -85, -65, 16,
    -75, -65, 16,
    -75, -55, 16,
    -85, -55, 16,
    -- A button (25-28)
    65, -55, 16,
    80, -55, 16,
    80, -40, 16,
    65, -40, 16,
    -- B button (29-32)
    55, -70, 16,
    70, -70, 16,
    70, -55, 16,
    55, -55, 16,
    -- Left shoulder (33-36)
    -90, 90, 10,
    -60, 90, 10,
    -60, 100, 10,
    -90, 100, 10,
    -- Right shoulder (37-40)
    60, 90, 10,
    90, 90, 10,
    90, 100, 10,
    60, 100, 10,
    -- Speaker grille (41-44)
    60, -75, 16,
    90, -75, 16,
    90, -85, 16,
    60, -85, 16,
}

-- Scale down to 80% for better framing
local scale = 0.8
for i = 1, #vertices do
    vertices[i] = vertices[i] * scale
end

-- Edges derived from quads (each quad = 4 edges)
local edges = {
    -- Front face
    1,2, 2,3, 3,4, 4,1,
    -- Back face
    5,6, 6,7, 7,8, 8,5,
    -- Top face
    4,3, 3,7, 7,8, 8,4,
    -- Bottom face
    1,5, 5,6, 6,2, 2,1,
    -- Right face
    2,6, 6,7, 7,3, 3,2,
    -- Left face
    5,1, 1,4, 4,8, 8,5,
    -- Screen bezel front
    9,10, 10,11, 11,12, 12,9,
    -- Screen display
    13,14, 14,15, 15,16, 16,13,
    -- D-pad vertical
    17,18, 18,19, 19,20, 20,17,
    -- D-pad horizontal
    21,22, 22,23, 23,24, 24,21,
    -- A button
    25,26, 26,27, 27,28, 28,25,
    -- B button
    29,30, 30,31, 31,32, 32,29,
    -- Left shoulder
    33,34, 34,35, 35,36, 36,33,
    -- Right shoulder
    37,38, 38,39, 39,40, 40,37,
    -- Speaker grille
    41,42, 42,43, 43,44, 44,41,
}

-- Triangular faces for proper fill (derived from quads)
local faces = {
    -- Body front
    1,2,3, 1,3,4,
    -- Body back
    5,7,6, 5,8,7,
    -- Body top
    4,3,8, 3,7,8,
    -- Body bottom
    1,5,6, 1,6,2,
    -- Body right
    2,7,3, 2,6,7,
    -- Body left
    1,4,8, 1,8,5,
    -- Screen bezel
    9,10,11, 9,11,12,
    -- Screen display
    13,14,15, 13,15,16,
    -- D-pad
    17,18,19, 17,19,20,
    21,22,23, 21,23,24,
    -- A button
    25,26,27, 25,27,28,
    -- B button
    29,30,31, 29,31,32,
    -- Left shoulder
    33,34,35, 33,35,36,
    -- Right shoulder
    37,38,39, 37,39,40,
    -- Speaker (Y decreases, so reverse winding)
    41,43,42, 41,44,43,
}

-- Base fill colors
local fillColors = {
    disp.rgb(60, 80, 100),   -- Dark blue gray (body)
    disp.rgb(40, 100, 60),   -- Green (screen)
    disp.rgb(200, 80, 80),   -- Red (A button)
    disp.rgb(80, 80, 200),   -- Blue (B button)
    disp.rgb(150, 150, 160), -- Light gray (shoulder)
}

-- Rotation angles (radians)
local angleX = 0
local angleY = 0
local angleZ = 0

-- Auto-rotate speeds (radians per frame)
local rotSpeed = 0.02

-- Main loop
local mode = 1  -- 1=auto rotate, 2=manual control
local fillMode = 0  -- 0=wireframe only, 1=fill only, 2=both
local showHelp = true
local helpTimer = 0
local fillColorIndex = 1

while true do
    perf.beginFrame()
    input.update()

    -- Input handling: the gamepad (keys, rebound keys or a controller)
    local nav = gp.getNavPressed()
    if nav & gp.PAD_B ~= 0 then
        return  -- B or Esc: exit to launcher
    end

    if nav & gp.PAD_A ~= 0 then
        mode = (mode == 1) and 2 or 1
        showHelp = true
        helpTimer = 0
    end

    -- Start (F1 by default): cycle fill mode
    if nav & gp.PAD_START ~= 0 then
        fillMode = (fillMode + 1) % 3
        showHelp = true
        helpTimer = 0
    end

    -- L (F2 by default): cycle fill color
    if nav & gp.PAD_L ~= 0 then
        fillColorIndex = (fillColorIndex % #fillColors) + 1
    end

    -- Update rotation
    if mode == 1 then
        angleX = angleX + rotSpeed
        angleY = angleY + rotSpeed * 0.7
        angleZ = angleZ + rotSpeed * 0.5
    else
        -- Held: the D-pad turns X/Y, Start and L roll Z
        local held = gp.getButtons()
        if held & gp.PAD_UP    ~= 0 then angleX = angleX + 0.05 end
        if held & gp.PAD_DOWN  ~= 0 then angleX = angleX - 0.05 end
        if held & gp.PAD_LEFT  ~= 0 then angleY = angleY - 0.05 end
        if held & gp.PAD_RIGHT ~= 0 then angleY = angleY + 0.05 end
        if held & gp.PAD_START ~= 0 then angleZ = angleZ - 0.05 end
        if held & gp.PAD_L     ~= 0 then angleZ = angleZ + 0.05 end
    end

    -- Clear screen
    disp.clear(disp.BLACK)

    -- Use the new draw3DWireframeEx function with fill mode
    -- fillMode: 0 = wireframe only, 1 = fill only, 2 = both
    draw3DWireframeEx(
        vertices, edges,
        angleX, angleY, angleZ,
        cx, cy, 200,
        disp.CYAN, fillColors[fillColorIndex], fillMode, 2, faces)

    -- Draw title and mode
    local modeText = mode == 1 and "AUTO" or "MANUAL"
    local fillText = fillMode == 0 and "WIRE" or (fillMode == 1 and "FILL" or "BOTH")
    pc.ui.drawHeader("3D PicoCalc - " .. modeText .. " - " .. fillText)

    -- Show help for 3 seconds
    if showHelp then
        helpTimer = helpTimer + 1
        if helpTimer < 180 then
            local y = 230
            if mode == 1 then
                disp.drawText(4, y, string.format("%s: Manual  %s: Fill Mode  %s: Color", pad_key(gp.PAD_A), pad_key(gp.PAD_START), pad_key(gp.PAD_L)), disp.GRAY, disp.BLACK)
            else
                disp.drawText(4, y,      "D-Pad: Rotate X/Y", disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 10, string.format("%s: Fill  %s: Color", pad_key(gp.PAD_START), pad_key(gp.PAD_L)), disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 20, pad_key(gp.PAD_A) .. ": Auto mode",  disp.GRAY, disp.BLACK)
            end
        else
            showHelp = false
        end
    end

    pc.ui.drawFooter(pad_key(gp.PAD_A) .. ": Toggle Mode", "ESC: Exit")

    -- FPS counter (top right)
    perf.drawFPS(250, 4)

    -- Flush to display
    disp.flush()

    perf.endFrame()
end
