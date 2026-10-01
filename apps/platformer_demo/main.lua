-- Platformer Demo for PicoDeck Game Framework
-- Demonstrates: camera, scenes, save/load, particles, object pooling

local pc = picocalc
local disp = pc.display
local input = pc.input
local game = pc.game

-- Controls. Games read the logical gamepad (picocalc.gamepad) instead of raw
-- keys: players can rebind it in Settings > Controls with no code here, and
-- hints use getLabel() so they stay right after a rebind. Esc is not a
-- gamepad button; it stays on picocalc.input. Older firmware has no gamepad,
-- so this small adapter falls back to the keys the game read before it.
local gp = pc.gamepad
local PAD, read_pad, pad_label
if gp then
    PAD = {LEFT = gp.PAD_LEFT, RIGHT = gp.PAD_RIGHT, UP = gp.PAD_UP, A = gp.PAD_A}
    read_pad = function() return gp.getButtons(), gp.getButtonsPressed() end
    pad_label = function(btn) return gp.getLabel(btn) or "?" end
else
    PAD = {LEFT = input.BTN_LEFT, RIGHT = input.BTN_RIGHT, UP = input.BTN_UP, A = input.BTN_ENTER}
    read_pad = function() return input.getButtons(), input.getButtonsPressed() end
    local names = {[PAD.UP] = "Up", [PAD.A] = "Enter"}
    pad_label = function(btn) return names[btn] or "?" end
end

-- Menus take A, or Enter as before the gamepad. Enter counts only when it
-- pressed no gamepad button: a player who bound it to one gets that button's
-- meaning alone. Gameplay reads A only.
local function confirm_pressed(pad_pressed)
    if pad_pressed & PAD.A ~= 0 then return true end
    return gp ~= nil and pad_pressed == 0
        and input.getButtonsPressed() & input.BTN_ENTER ~= 0
end

-- Colors
local BLACK = disp.rgb(0, 0, 0)
local WHITE = disp.rgb(255, 255, 255)
local RED = disp.rgb(255, 50, 50)
local GREEN = disp.rgb(50, 255, 50)
local BLUE = disp.rgb(50, 100, 255)
local YELLOW = disp.rgb(255, 255, 50)
local GRAY = disp.rgb(128, 128, 128)
local DARK_GRAY = disp.rgb(64, 64, 64)

-- Game state
local player = {
    x = 50, y = 200,
    vx = 0, vy = 0,
    w = 16, h = 24,
    on_ground = false,
    jump_power = -400,
    speed = 200,
    gravity = 800
}

local platforms = {}
local coins = {}
local particles = {}
local score = 0
local high_score = 0
local camera = nil
local world_width = 1000
local world_height = 320

-- Load high score
local function load_high_score()
    local data = game.save.get("platformer")
    if data and data.high_score then
        high_score = tonumber(data.high_score) or 0
    end
end

-- Save high score
local function save_high_score()
    if score > high_score then
        high_score = score
        game.save.set("platformer", {high_score = high_score})
    end
end

-- Initialize level
local function init_level()
    platforms = {}
    coins = {}
    particles = {}
    score = 0
    
    -- Ground
    table.insert(platforms, {x = 0, y = 280, w = world_width, h = 40})
    
    -- Platforms
    for i = 1, 20 do
        table.insert(platforms, {
            x = 100 + i * 80,
            y = 200 - (i % 3) * 40,
            w = 80,
            h = 16
        })
    end
    
    -- Coins
    for i = 1, 30 do
        table.insert(coins, {
            x = 150 + i * 60,
            y = 150 + (i % 5) * 30,
            collected = false
        })
    end
    
    -- Reset player
    player.x = 50
    player.y = 200
    player.vx = 0
    player.vy = 0
    player.on_ground = false
end

-- Spawn particle
local function spawn_particle(x, y, color)
    table.insert(particles, {
        x = x, y = y,
        vx = (math.random() - 0.5) * 200,
        vy = (math.random() - 0.5) * 200 - 100,
        life = 0.5,
        color = color
    })
end

-- Update particles
local function update_particles(dt)
    local i = 1
    while i <= #particles do
        local p = particles[i]
        p.x = p.x + p.vx * dt
        p.y = p.y + p.vy * dt
        p.vy = p.vy + 400 * dt
        p.life = p.life - dt
        
        if p.life <= 0 then
            table.remove(particles, i)
        else
            i = i + 1
        end
    end
end

-- Draw particles
local function draw_particles()
    for _, p in ipairs(particles) do
        local alpha = math.min(1, p.life * 2)
        disp.setPixel(math.floor(p.x), math.floor(p.y), p.color)
    end
end

-- AABB collision
local function aabb_overlap(ax, ay, aw, ah, bx, by, bw, bh)
    return ax < bx + bw and ax + aw > bx and
           ay < by + bh and ay + ah > by
end

-- Menu scene
local menu_scene = {
    enter = function()
        load_high_score()
    end,
    
    update = function(dt)
        local _, pressed = read_pad()
        if confirm_pressed(pressed) then
            game.scene.switch("play")
        end
    end,
    
    draw = function()
        disp.clear(BLACK)
        
        -- Title
        disp.drawText(80, 80, "PLATFORMER DEMO", YELLOW, BLACK)
        disp.drawText(60, 100, "Game Framework Demo", WHITE, BLACK)
        
        -- Instructions
        disp.drawText(40, 160, "D-pad: Move", GRAY, BLACK)
        disp.drawText(40, 180, "Up/" .. pad_label(PAD.A) .. ": Jump", GRAY, BLACK)
        disp.drawText(40, 200, "Collect coins!", YELLOW, BLACK)
        
        -- High score
        disp.drawText(60, 240, "High Score: " .. high_score, GREEN, BLACK)
        
        -- Start prompt
        disp.drawText(50, 280, "Press " .. pad_label(PAD.A) .. " to Start", WHITE, BLACK)
    end
}

-- Play scene
local play_scene = {
    enter = function()
        init_level()
        camera = game.camera.new()
        camera:setPosition(160, player.y - 40)
        camera:setBounds(0, 0, world_width, world_height)
    end,
    
    update = function(dt)
        -- Check for ESC to open exit dialog
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
            if pc.ui.confirm("Exit to menu?") then
                save_high_score()
                game.scene.switch("menu")
            end
            return
        end

        -- Input
        local btn = read_pad()
        
        -- Horizontal movement
        if btn & PAD.LEFT ~= 0 then
            player.vx = -player.speed
        elseif btn & PAD.RIGHT ~= 0 then
            player.vx = player.speed
        else
            player.vx = player.vx * 0.8
        end
        
        -- Jump
        if btn & (PAD.UP | PAD.A) ~= 0 and player.on_ground then
            player.vy = player.jump_power
            player.on_ground = false
            spawn_particle(player.x + player.w/2, player.y + player.h, WHITE)
        end
        
        -- Gravity
        player.vy = player.vy + player.gravity * dt
        
        -- Move X
        player.x = player.x + player.vx * dt
        
        -- Move Y
        player.y = player.y + player.vy * dt
        
        -- Platform collision
        player.on_ground = false
        for _, plat in ipairs(platforms) do
            if aabb_overlap(player.x, player.y, player.w, player.h,
                           plat.x, plat.y, plat.w, plat.h) then
                -- Landing on top
                if player.vy > 0 and player.y + player.h - player.vy * dt <= plat.y + 4 then
                    player.y = plat.y - player.h
                    player.vy = 0
                    player.on_ground = true
                -- Hitting from below
                elseif player.vy < 0 and player.y - player.vy * dt >= plat.y + plat.h - 4 then
                    player.y = plat.y + plat.h
                    player.vy = 0
                end
            end
        end
        
        -- Collect coins
        for _, coin in ipairs(coins) do
            if not coin.collected then
                if aabb_overlap(player.x, player.y, player.w, player.h,
                               coin.x - 8, coin.y - 8, 16, 16) then
                    coin.collected = true
                    score = score + 10
                    -- Play coin collection sound
                    pc.audio.playTone(880, 100)
                    for i = 1, 8 do
                        spawn_particle(coin.x, coin.y, YELLOW)
                    end
                end
            end
        end
        
        -- Fall off screen
        if player.y > world_height + 50 then
            save_high_score()
            game.scene.switch("gameover")
        end
        
        -- Update camera (Mario-style: follow only after player passes screen center)
        local target_x = math.max(player.x, 160)
        camera:setTarget(target_x, player.y - 40, 0.1)
        camera:update(dt)
        
        -- Update particles
        update_particles(dt)
        
        -- Check all coins collected
        local all_collected = true
        for _, coin in ipairs(coins) do
            if not coin.collected then
                all_collected = false
                break
            end
        end
        
        if all_collected then
            save_high_score()
            game.scene.switch("gameover")
        end
    end,
    
    draw = function()
        disp.clear(BLACK)
        
        local ox, oy = camera:getOffset()
        
        -- Draw platforms
        for _, plat in ipairs(platforms) do
            local sx = plat.x + ox
            local sy = plat.y + oy
            if sx > -plat.w and sx < 320 and sy > -plat.h and sy < 320 then
                disp.fillRect(sx, sy, plat.w, plat.h, DARK_GRAY)
                disp.drawRect(sx, sy, plat.w, plat.h, GRAY)
            end
        end
        
        -- Draw coins
        for _, coin in ipairs(coins) do
            if not coin.collected then
                local sx = coin.x + ox
                local sy = coin.y + oy
                if sx > -16 and sx < 320 and sy > -16 and sy < 320 then
                    disp.fillRect(sx - 6, sy - 6, 12, 12, YELLOW)
                end
            end
        end
        
        -- Draw player
        local px = player.x + ox
        local py = player.y + oy
        disp.fillRect(px, py, player.w, player.h, BLUE)
        disp.drawRect(px, py, player.w, player.h, WHITE)
        
        -- Draw particles
        draw_particles()
        
        -- HUD
        disp.fillRect(0, 0, 320, 20, BLACK)
        disp.drawText(4, 4, "Score: " .. score, WHITE, BLACK)
        disp.drawText(200, 4, "Hi: " .. high_score, YELLOW, BLACK)
    end
}

-- Game over scene
local gameover_scene = {
    enter = function()
        camera:shake(10, 0.5)
    end,
    
    update = function(dt)
        camera:update(dt)
        
        local _, pressed = read_pad()
        if confirm_pressed(pressed) then
            game.scene.switch("play")
        elseif input.getButtonsPressed() & input.BTN_ESC ~= 0 then
            game.scene.switch("menu")
        end
    end,
    
    draw = function()
        disp.clear(BLACK)
        
        disp.drawText(90, 100, "GAME OVER", RED, BLACK)
        disp.drawText(90, 130, "Score: " .. score, WHITE, BLACK)
        
        if score >= high_score and score > 0 then
            disp.drawText(70, 150, "NEW HIGH SCORE!", YELLOW, BLACK)
        else
            disp.drawText(80, 150, "High Score: " .. high_score, GREEN, BLACK)
        end
        
        disp.drawText(40, 220, pad_label(PAD.A) .. ": Play Again", GRAY, BLACK)
        disp.drawText(40, 240, "ESC: Menu", GRAY, BLACK)
    end
}

-- Register scenes
game.scene.add("menu", menu_scene)
game.scene.add("play", play_scene)
game.scene.add("gameover", gameover_scene)

-- Start with menu
game.scene.switch("menu")

-- Main loop
while true do
    pc.perf.beginFrame()
    input.update()
    
    local dt = pc.perf.getFrameTime() / 1000.0
    if dt > 0.05 then dt = 0.05 end
    
    game.scene.update(dt)
    game.scene.draw()
    
    pc.display.flush()
    pc.perf.endFrame()
    pc.sys.sleep(16)
end
