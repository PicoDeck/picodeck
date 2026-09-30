-- Guinea Pig Run - A PicoDeck Platformer
-- Collect veggies, avoid enemies, find your way home!

local pc = picocalc
local disp = pc.display
local input = pc.input
local game = pc.game

-- Input: the logical gamepad (rebindable in Settings > Controls) where the
-- firmware has it, raw keys on older firmware. P.* are masks that work with
-- either read; key() names the key to show in on-screen hints.
local gp = pc.gamepad
local P, read_buttons, key
if gp then
    P = {UP = gp.PAD_UP, DOWN = gp.PAD_DOWN, LEFT = gp.PAD_LEFT, RIGHT = gp.PAD_RIGHT,
         A = gp.PAD_A, B = gp.PAD_B, X = gp.PAD_X}
    read_buttons = function() return gp.getButtons(), gp.getButtonsPressed() end
    key = function(btn) return gp.getLabel(btn) or "?" end
else
    P = {UP = input.BTN_UP, DOWN = input.BTN_DOWN, LEFT = input.BTN_LEFT,
         RIGHT = input.BTN_RIGHT, A = input.BTN_ENTER, B = input.BTN_F2, X = input.BTN_F1}
    read_buttons = function() return input.getButtons(), input.getButtonsPressed() end
    local names = {[P.A] = "Enter", [P.B] = "F2", [P.X] = "F1"}
    key = function(btn) return names[btn] end
end

-- ============================================================
-- [1] CONSTANTS & COLORS
-- ============================================================

-- Physics
local GRAVITY = 800
local JUMP_POWER = -380
local RUN_SPEED = 150
local FRICTION = 0.8
local DASH_SPEED = 350
local DASH_DURATION = 0.15
local DASH_COOLDOWN = 1.0
local ZOOMIES_SPEED = 400
local ZOOMIES_DURATION = 3.0
local POPCORN_JUMP_POWER = -300
local POPCORN_KNOCKBACK_RADIUS = 40
local NAIL_GRIP_DURATION = 5.0
local NAIL_GRIP_CLIMB_SPEED = 100
local SQUEAK_CHARGE_TIME = 1.0
local SQUEAK_COOLDOWN = 5.0
local SQUEAK_SPEED = 300
local SQUEAK_PUSH = 60

-- World
local WORLD_WIDTH = 3200
local WORLD_HEIGHT = 320
local GROUND_Y = 280
local SCREEN_W = 320
local SCREEN_H = 320

-- Colors
local BLACK = disp.rgb(0, 0, 0)
local WHITE = disp.rgb(255, 255, 255)
local RED = disp.rgb(255, 50, 50)
local GREEN = disp.rgb(50, 200, 50)
local DARK_GREEN = disp.rgb(20, 100, 20)
local BLUE = disp.rgb(50, 100, 255)
local YELLOW = disp.rgb(255, 255, 50)
local ORANGE = disp.rgb(255, 160, 30)
local BROWN = disp.rgb(139, 90, 43)
local DARK_BROWN = disp.rgb(90, 55, 25)
local LIGHT_BROWN = disp.rgb(180, 130, 70)
local GRAY = disp.rgb(128, 128, 128)
local DARK_GRAY = disp.rgb(64, 64, 64)
local SKY_LIGHT = disp.rgb(255, 224, 160)   -- golden horizon glow
local SKY_MID = disp.rgb(150, 200, 235)
local SKY_DARK = disp.rgb(90, 160, 215)
local GRASS_GREEN = disp.rgb(80, 180, 50)
local GRASS_DARK = disp.rgb(50, 130, 30)
local CLOUD_WHITE = disp.rgb(240, 245, 255)
local CLOUD_SHADOW = disp.rgb(200, 210, 230)
local PINK = disp.rgb(255, 180, 180)
local GOLD = disp.rgb(255, 215, 0)
local MAGENTA = disp.rgb(255, 0, 255)
local DARK_RED = disp.rgb(180, 30, 30)
local FIRE_ORANGE = disp.rgb(255, 100, 20)
local WATER_BLUE = disp.rgb(80, 160, 255)
local WATER_LIGHT = disp.rgb(150, 210, 255)
local HAY_GOLD = disp.rgb(218, 185, 90)
local HAY_DARK = disp.rgb(180, 150, 60)
local SHELL_BROWN = disp.rgb(160, 100, 50)
local HOUSE_RED = disp.rgb(200, 60, 40)
local HOUSE_DARK = disp.rgb(140, 40, 30)
local SNAIL_BODY = disp.rgb(180, 170, 130)
local HOUSE_HI = disp.rgb(240, 100, 80)

-- ============================================================
-- [2] UTILITY FUNCTIONS
-- ============================================================

local function aabb_overlap(ax, ay, aw, ah, bx, by, bw, bh)
    return ax < bx + bw and ax + aw > bx and ay < by + bh and ay + ah > by
end

local function dist(x1, y1, x2, y2)
    local dx = x1 - x2
    local dy = y1 - y2
    return math.sqrt(dx * dx + dy * dy)
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function sign(v)
    if v > 0 then return 1 elseif v < 0 then return -1 else return 0 end
end

-- Particles
local particles = {}
local MAX_PARTICLES = 30

local function spawn_particle(x, y, color)
    if #particles >= MAX_PARTICLES then return end
    table.insert(particles, {
        x = x, y = y,
        vx = (math.random() - 0.5) * 200,
        vy = (math.random() - 0.5) * 200 - 80,
        life = 0.4 + math.random() * 0.3,
        color = color
    })
end

local function spawn_burst(x, y, color, count)
    for i = 1, count do
        spawn_particle(x, y, color)
    end
end

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

local function draw_particles_at(ox, oy)
    for _, p in ipairs(particles) do
        local sx = math.floor(p.x + ox)
        local sy = math.floor(p.y + oy)
        if sx >= 0 and sx < SCREEN_W and sy >= 0 and sy < SCREEN_H then
            disp.fillRect(sx, sy, 2, 2, p.color)
        end
    end
end

-- Sound engine: SFX bank (play ranges) + playTone fallback
local sfx = { ok = false, banks = {}, players = {}, pbank = {}, ranges = {}, busy = {} }
local SFX_PRIORITY = {  -- higher wins when stealing a busy player
    damage = 100, game_over = 95, win_jingle = 90, collect_powerup = 60,
    enemy_defeat = 55, collect_carrot = 50, collect_capsicum = 50,
    collect_zucchini = 50, popcorn = 40, jump = 30, dash = 30,
    squeak = 30, hawk_screech = 20, water_burst = 10,
    menu_move = 80, menu_select = 85,
}

local function sfx_init()
    -- dofile is blocked on PicoDeck (no host fopen for the SD FATFS);
    -- load the ranges module via pc.fs + load() like picoforge does.
    local src = pc.fs.readFile(APP_DIR .. "/sfx/sfx_ranges.lua")
    if not src then return end
    local fn = load(src, "@sfx_ranges.lua")
    if not fn then return end
    local ok, ranges = pcall(fn)
    if not ok or type(ranges) ~= "table" then return end
    sfx.ranges = ranges
    for b = 1, 2 do
        local path = APP_DIR .. "/sfx/bank" .. b .. ".wav"
        if pc.fs.exists(path) then
            local s = pc.sound.sample(path)   -- sample userdata: GC-safe load
            if s then sfx.banks[b] = s end
        end
    end
    if not sfx.banks[1] then return end
    for i = 1, 4 do
        local p = pc.sound.sampleplayer(sfx.banks[1])
        if not p then break end
        sfx.players[i] = p
        sfx.pbank[i] = 1
        sfx.busy[i] = 0
    end
    sfx.ok = #sfx.players > 0
end

local SFX_FALLBACK = {  -- name -> {freq, ms} for playTone when bank missing
    jump = {440, 80}, popcorn = {660, 60}, collect_carrot = {880, 100},
    collect_capsicum = {660, 100}, collect_zucchini = {550, 100},
    collect_powerup = {440, 80}, dash = {330, 60}, squeak = {1200, 200},
    damage = {220, 150}, hawk_screech = {1000, 120}, win_jingle = {440, 150},
    water_burst = {300, 120}, enemy_defeat = {480, 100},
    menu_move = {800, 30}, menu_select = {1000, 60}, game_over = {220, 250},
}

function sfx.play(name)
    local fb = SFX_FALLBACK[name]
    if not sfx.ok then
        if fb then pc.audio.playTone(fb[1], fb[2]) end
        return
    end
    local r = sfx.ranges[name]
    local bank = r and (r.bank or 1) or 1
    if not r or not sfx.banks[bank] then
        if fb then pc.audio.playTone(fb[1], fb[2]) end
        return
    end
    local my_pri = SFX_PRIORITY[name] or 10
    local slot, lowest, lowest_i = nil, math.huge, 1
    for i, p in ipairs(sfx.players) do
        if not p:isPlaying() then slot = i break end
        if sfx.busy[i] < lowest then lowest = sfx.busy[i] lowest_i = i end
    end
    if not slot then
        if lowest >= my_pri then return end  -- keep higher-priority sound
        slot = lowest_i
    end
    local p = sfx.players[slot]
    p:stop()
    if sfx.pbank[slot] ~= bank then
        -- owns_sample=false for userdata samples: reseat is safe
        p:setSample(sfx.banks[bank])
        sfx.pbank[slot] = bank
    end
    p:setPlayRange(r.first, r.last)
    p:setVolume(100)
    p:play(1)
    sfx.busy[slot] = my_pri
end

sfx_init()

-- Thin wrappers (same names as before: zero call-site churn)
local function snd_jump() sfx.play("jump") end
local function snd_popcorn() sfx.play("popcorn") end
local function snd_collect_carrot() sfx.play("collect_carrot") end
local function snd_collect_capsicum() sfx.play("collect_capsicum") end
local function snd_collect_zucchini() sfx.play("collect_zucchini") end
local function snd_collect_powerup() sfx.play("collect_powerup") end
local function snd_dash() sfx.play("dash") end
local function snd_squeak() sfx.play("squeak") end
local function snd_damage() sfx.play("damage") end
local function snd_hawk() sfx.play("hawk_screech") end
local function snd_win() sfx.play("win_jingle") end

-- BGM: streamed loop via fileplayer; replay-on-finish (panels.lua pattern)
local bgm = { fp = nil, want = nil, restart = false }
local BGM_VOLUME = { menu = 70, play = 40, win = 70 }

function bgm.play(scene)
    bgm.want = scene
    if not bgm.fp then
        local ok, fp = pcall(pc.sound.fileplayer)
        if not ok or not fp then return end
        local loaded = pcall(function() fp:load(APP_DIR .. "/sfx/bgm.wav") end)
        if not loaded then return end
        bgm.fp = fp
        pcall(function()
            bgm.fp:setFinishCallback(function() bgm.restart = true end)
        end)
    end
    pcall(function() bgm.fp:setVolume(BGM_VOLUME[scene] or 50) end)
    if bgm.fp.isPlaying and not bgm.fp:isPlaying() then
        pcall(function() bgm.fp:play(1) end)
    end
end

function bgm.update()
    if bgm.fp and bgm.restart and bgm.want then
        bgm.restart = false
        pcall(function() bgm.fp:play(1) end)
    end
end

function bgm.stop()
    bgm.want = nil
    if bgm.fp then pcall(function() bgm.fp:stop() end) end
end

-- ============================================================
-- [3] SPRITE LOADING & DRAWING
-- ============================================================

local gfx = pc.graphics
local sprites = {}

local function load_sprite(name, filename)
    local path = APP_DIR .. "/sprites/" .. filename
    local ok, img = pcall(gfx.image.load, path)
    if ok and img then
        img:setTransparentColor(MAGENTA)
        sprites[name] = img
        return img
    end
    return nil
end

-- Load all sprites
load_sprite("gp_east", "gp_east.png")
load_sprite("gp_west", "gp_west.png")
load_sprite("gp_idle_east", "gp_idle_east.png")   -- 8 frames, 256x32
load_sprite("gp_idle_west", "gp_idle_west.png")   -- 8 frames, 256x32
load_sprite("gp_jump_west", "gp_jump_west.png")   -- 8 frames, 256x32
load_sprite("gp_run_east", "gp_run_east.png")     -- 4 frames, 128x32
load_sprite("gp_run_west", "gp_run_west.png")     -- 4 frames, 128x32
load_sprite("carrot", "carrot.png")
load_sprite("capsicum", "capsicum.png")
load_sprite("zucchini", "zucchini.png")
load_sprite("strawberry", "strawberry.png")
load_sprite("vitamin_c", "vitamin_c.png")
load_sprite("nail_grip", "nail_grip.png")
load_sprite("heart", "heart.png")                -- 32x32 HUD heart
load_sprite("hawk_glide", "hawk_glide.png")        -- 48x32
load_sprite("hawk_dive", "hawk_dive.png")          -- 48x32
load_sprite("snail", "snail.png")
load_sprite("broccoli", "broccoli.png")            -- 32x40
load_sprite("spicy_pepper", "spicy_pepper.png")    -- 32x40
load_sprite("sprinkler_body", "sprinkler_body.png") -- 32x32 brass head on spike
load_sprite("water_arc", "water_arc.png")           -- 5 frames, 320x32
load_sprite("house", "house.png")                  -- 64x64
load_sprite("hay_pile", "hay_pile.png")            -- 48x32
load_sprite("cloud_large", "cloud_large.png")      -- 64x32
load_sprite("cloud_small", "cloud_small.png")      -- 48x32
load_sprite("tile_grass", "tile_grass_top.png")     -- 16x16 grass top
load_sprite("tile_earth", "tile_earth.png")         -- 16x16 solid earth
load_sprite("tile_edge_l", "tile_edge_l.png")     -- 16x16 left end cap
load_sprite("tile_edge_r", "tile_edge_r.png")     -- 16x16 right end cap
load_sprite("bg_trees_far_1", "bg_trees_far_1.png")     -- 160x72 dense tree line
load_sprite("bg_trees_far_2", "bg_trees_far_2.png")     -- 160x72 orchard + shed
load_sprite("bg_fence_mid_1", "bg_fence_mid_1.png")     -- 160x56 picket fence + hedge
load_sprite("bg_fence_mid_2", "bg_fence_mid_2.png")     -- 160x56 weathered fence + flowers
load_sprite("bg_garden_near_1", "bg_garden_near_1.png") -- 128x32 carrot/radish bed
load_sprite("bg_garden_near_2", "bg_garden_near_2.png") -- 128x32 tulip bed
load_sprite("title_logo", "title_logo.png")             -- 256x64 title logo

-- Guinea pig animation: sprite sheet frame drawing
-- Sheets are horizontal strips: frame N is at x=N*32, y=0, w=32, h=32
local GP_FRAME_W = 32
local GP_FRAME_H = 32
local GP_IDLE_FRAMES = 8
local GP_JUMP_FRAMES = 8
local GP_RUN_FRAMES = 4

local function draw_guinea_pig(sx, sy, facing, frame, has_shield, shield_flicker, game_time)
    local img = nil
    local sub = nil
    local west = facing < 0

    if frame == 3 then
        -- Jump: west sheet, flip for east
        img = sprites.gp_jump_west
        if img then
            local jf = math.floor(game_time * 10) % GP_JUMP_FRAMES
            sub = {x = jf * GP_FRAME_W, y = 0, w = GP_FRAME_W, h = GP_FRAME_H}
            img:draw(sx, sy, {flipX = not west}, sub)
        end
    elseif frame == 0 then
        -- Idle: direction-specific sheets
        img = west and sprites.gp_idle_west or sprites.gp_idle_east
        if img then
            local idf = math.floor(game_time * 6) % GP_IDLE_FRAMES
            sub = {x = idf * GP_FRAME_W, y = 0, w = GP_FRAME_W, h = GP_FRAME_H}
            img:draw(sx, sy, nil, sub)
        end
    else
        -- Run: direction-specific sheets
        img = west and sprites.gp_run_west or sprites.gp_run_east
        if img then
            local rf = math.floor(game_time * 10) % GP_RUN_FRAMES
            sub = {x = rf * GP_FRAME_W, y = 0, w = GP_FRAME_W, h = GP_FRAME_H}
            img:draw(sx, sy, nil, sub)
        end
    end

    -- Fallback: static sprite if no sheet loaded
    if not img then
        local static = west and sprites.gp_west or sprites.gp_east
        if static then
            static:draw(sx, sy)
        else
            disp.fillRect(sx + 4, sy + 4, 20, 12, BROWN)
            disp.fillRect(sx + 8, sy + 8, 12, 6, LIGHT_BROWN)
        end
    end

    -- Shield effect (always procedural)
    if has_shield and shield_flicker then
        disp.drawCircle(sx + 16, sy + 16, 18, ORANGE)
    end
end

-- Collectible draw offset: center 32x32 canvas on original hitbox positions
local function draw_carrot(sx, sy)
    if sprites.carrot then
        sprites.carrot:drawScaled(sx - 4, sy - 1, 0.5)
    else
        disp.fillRect(sx + 2, sy + 4, 4, 10, ORANGE)
        disp.fillRect(sx + 2, sy, 4, 5, GREEN)
    end
end

local function draw_capsicum(sx, sy)
    if sprites.capsicum then
        sprites.capsicum:drawScaled(sx - 3, sy - 2, 0.5)
    else
        disp.fillRect(sx + 1, sy + 3, 8, 8, RED)
        disp.fillRect(sx + 3, sy, 4, 3, GREEN)
    end
end

local function draw_zucchini(sx, sy)
    if sprites.zucchini then
        sprites.zucchini:drawScaled(sx - 2, sy - 4, 0.5)
    else
        disp.fillRect(sx, sy + 2, 12, 4, DARK_GREEN)
    end
end

local function draw_strawberry(sx, sy)
    if sprites.strawberry then
        sprites.strawberry:drawScaled(sx - 3, sy - 3, 0.5)
    else
        disp.fillRect(sx + 1, sy + 3, 8, 5, RED)
        disp.fillRect(sx + 3, sy, 4, 3, GREEN)
    end
end

local function draw_vitamin_c(sx, sy, t)
    if sprites.vitamin_c then
        sprites.vitamin_c:drawScaled(sx - 2, sy - 2, 0.5)
    else
        local pulse = math.floor(math.sin(t * 4) * 2)
        disp.fillCircle(sx + 6, sy + 6, 5 + pulse, ORANGE)
        disp.fillCircle(sx + 6, sy + 6, 3, YELLOW)
    end
end

local function draw_nail_grip_item(sx, sy, t)
    if sprites.nail_grip then
        sprites.nail_grip:drawScaled(sx - 3, sy - 3, 0.5)
    else
        local pulse = math.floor(math.sin(t * 5) * 1)
        disp.fillCircle(sx + 6, sy + 6, 5 + pulse, GRAY)
    end
end

local function draw_house(sx, sy)
    if sprites.house then
        sprites.house:draw(sx - 8, sy - 14)
    else
        disp.fillCircle(sx + 24, sy + 18, 22, HOUSE_RED)
        disp.fillRect(sx + 2, sy + 18, 44, 18, HOUSE_RED)
        disp.fillCircle(sx + 24, sy + 26, 8, BLACK)
    end
end

local function draw_hawk(sx, sy, facing, diving)
    local img = diving and sprites.hawk_dive or sprites.hawk_glide
    if img then
        img:draw(sx - 10, sy - 8, {flipX = (facing < 0)})
    else
        disp.fillRect(sx + 8, sy + 4, 12, 8, DARK_GRAY)
        disp.fillRect(sx, sy + 4, 10, 3, DARK_GRAY)
        disp.fillRect(sx + 18, sy + 4, 10, 3, DARK_GRAY)
    end
end

local function draw_snail(sx, sy, flipped)
    if sprites.snail then
        sprites.snail:draw(sx - 8, sy - 10, {flipX = flipped})
    else
        disp.fillRect(sx + 2, sy + 8, 12, 4, SNAIL_BODY)
        disp.fillCircle(sx + 8, sy + 5, 5, SHELL_BROWN)
    end
end

local function draw_broccoli(sx, sy)
    if sprites.broccoli then
        sprites.broccoli:draw(sx - 9, sy - 11)
    else
        disp.fillRect(sx + 5, sy + 10, 4, 8, GREEN)
        disp.fillCircle(sx + 7, sy + 3, 4, DARK_GREEN)
    end
end

local function draw_spicy_pepper(sx, sy)
    if sprites.spicy_pepper then
        sprites.spicy_pepper:draw(sx - 11, sy - 13)
    else
        disp.fillRect(sx + 2, sy + 4, 6, 8, RED)
        disp.fillRect(sx + 4, sy, 2, 4, GREEN)
    end
end

local function draw_sprinkler(sx, sy, sweep, active, t)
    if sprites.sprinkler_body then
        sprites.sprinkler_body:draw(sx - 8, sy - 16)
    else
        disp.fillRect(sx + 4, sy + 4, 8, 8, GREEN)
    end
    if not active then return end
    if sprites.water_arc then
        -- 320x32 strip, 5 frames of 64x32, arc always sprays right; flip for left
        local frame = math.floor(t * 12) % 5
        local ax = math.floor(sx - 56 + sweep * 48)   -- arc pivots over the body with the sweep
        sprites.water_arc:draw(ax, sy - 16, {flipX = sweep < 0.5},
            {x = frame * 64, y = 0, w = 64, h = 32})
        -- droplets at the arc's landing point, following the sweep
        local dx = sweep >= 0.5 and (ax + 60) or (ax + 3)
        for i = 0, 2 do
            local dy = math.floor(math.sin(t * 10 + i * 2) * 3)
            disp.fillRect(dx + i * 3 - 3, sy + 5 + dy + i * 2, 2, 2, WATER_LIGHT)
        end
    else
        local jet_w = 24
        -- mirror the sprite path: sweep<0.5 sprays left of the nozzle, >=0.5 right;
        -- travel the same 48px envelope as the hitbox swing
        local jx = math.floor(sx - jet_w + sweep * 48 + 8)
        for i = 0, jet_w - 4, 6 do
            disp.fillRect(jx + i, sy + 1 + (i % 3), 4, 3, WATER_BLUE)
        end
    end
end

local function draw_hay_pile(sx, sy, wiggle)
    local wx = wiggle or 0
    if sprites.hay_pile then
        sprites.hay_pile:draw(sx + wx - 8, sy - 6)
    else
        disp.fillRect(sx + wx, sy + 10, 32, 10, HAY_GOLD)
        disp.fillRect(sx + wx + 2, sy + 6, 28, 14, HAY_GOLD)
        disp.fillRect(sx + wx + 4, sy + 2, 24, 6, HAY_GOLD)
        disp.fillRect(sx + wx + 8, sy, 16, 4, HAY_GOLD)
    end
end

-- Fireball stays procedural (6x6, too small for pixel art)
local function draw_fireball(sx, sy)
    disp.fillRect(sx, sy, 4, 4, FIRE_ORANGE)
    disp.fillRect(sx + 1, sy + 1, 2, 2, YELLOW)
end

-- ============================================================
-- [4] LEVEL DATA
-- ============================================================

local platforms = {}
local collectibles = {}
local enemies = {}
local fireballs = {}
local hay_piles = {}
local squeaks = {}
local glass_barriers = {}
local total_veggies = 0

local function build_level()
    platforms = {}
    collectibles = {}
    enemies = {}
    fireballs = {}
    hay_piles = {}
    squeaks = {}
    glass_barriers = {}
    total_veggies = 0

    -- Ground segments
    table.insert(platforms, {x = 0, y = GROUND_Y, w = 550, h = 40, ground = true})
    table.insert(platforms, {x = 600, y = GROUND_Y, w = 350, h = 40, ground = true})
    table.insert(platforms, {x = 1350, y = GROUND_Y, w = 100, h = 40, ground = true})
    table.insert(platforms, {x = 1450, y = GROUND_Y, w = 200, h = 40, ground = true})
    table.insert(platforms, {x = 1700, y = GROUND_Y, w = 150, h = 40, ground = true})
    table.insert(platforms, {x = 1900, y = GROUND_Y, w = 150, h = 40, ground = true})
    table.insert(platforms, {x = 2050, y = GROUND_Y, w = 250, h = 40, ground = true})
    table.insert(platforms, {x = 2350, y = GROUND_Y, w = 200, h = 40, ground = true})
    table.insert(platforms, {x = 2600, y = GROUND_Y, w = 150, h = 40, ground = true})
    table.insert(platforms, {x = 2750, y = GROUND_Y, w = 450, h = 40, ground = true})

    -- Floating platforms
    table.insert(platforms, {x = 540, y = 230, w = 70, h = 12})
    table.insert(platforms, {x = 700, y = 220, w = 60, h = 12})
    table.insert(platforms, {x = 960, y = 250, w = 80, h = 12})
    table.insert(platforms, {x = 1040, y = 210, w = 90, h = 12})
    table.insert(platforms, {x = 1140, y = 170, w = 80, h = 12})
    table.insert(platforms, {x = 1250, y = 200, w = 100, h = 12})
    table.insert(platforms, {x = 1100, y = 130, w = 80, h = 12})
    table.insert(platforms, {x = 1480, y = 230, w = 70, h = 12})
    table.insert(platforms, {x = 1580, y = 200, w = 60, h = 12})
    table.insert(platforms, {x = 1650, y = 240, w = 70, h = 12})
    table.insert(platforms, {x = 1760, y = 180, w = 80, h = 12})
    table.insert(platforms, {x = 1850, y = 220, w = 60, h = 12})
    table.insert(platforms, {x = 1800, y = 100, w = 12, h = 180, wall = true})
    table.insert(platforms, {x = 2100, y = 230, w = 60, h = 12})
    table.insert(platforms, {x = 2400, y = 220, w = 70, h = 12})

    -- Collectibles
    table.insert(collectibles, {x = 150, y = GROUND_Y - 20, type = "carrot", collected = false})
    table.insert(collectibles, {x = 300, y = GROUND_Y - 20, type = "carrot", collected = false})
    table.insert(collectibles, {x = 560, y = 210, type = "capsicum", collected = false})
    table.insert(collectibles, {x = 720, y = 200, type = "zucchini", collected = false})
    table.insert(collectibles, {x = 980, y = 230, type = "carrot", collected = false})
    table.insert(collectibles, {x = 1060, y = 190, type = "carrot", collected = false})
    table.insert(collectibles, {x = 1160, y = 150, type = "capsicum", collected = false})
    table.insert(collectibles, {x = 1280, y = 180, type = "strawberry", collected = false})
    table.insert(collectibles, {x = 1500, y = 210, type = "zucchini", collected = false})
    table.insert(collectibles, {x = 1600, y = 180, type = "zucchini", collected = false})
    table.insert(collectibles, {x = 1670, y = 220, type = "carrot", collected = false})
    table.insert(collectibles, {x = 1780, y = 160, type = "vitamin_c", collected = false})
    table.insert(collectibles, {x = 1820, y = 120, type = "nail_grip", collected = false})
    table.insert(collectibles, {x = 2120, y = 210, type = "capsicum", collected = false})
    table.insert(collectibles, {x = 2250, y = GROUND_Y - 20, type = "zucchini", collected = false})
    table.insert(collectibles, {x = 2420, y = 200, type = "strawberry", collected = false})
    table.insert(collectibles, {x = 2650, y = GROUND_Y - 20, type = "vitamin_c", collected = false})
    table.insert(collectibles, {x = 3000, y = GROUND_Y - 20, type = "carrot", collected = false})

    for _, c in ipairs(collectibles) do
        if c.type == "carrot" or c.type == "capsicum" or c.type == "zucchini" then
            total_veggies = total_veggies + 1
        end
    end

    -- Enemies
    table.insert(enemies, {type = "hose", x = 750, y = GROUND_Y - 16, sweep = 0.5, timer = 0, active = false, spray_w = 48, alive = true})
    table.insert(enemies, {type = "hawk", x = 1100, y = 30, patrol_x1 = 900, patrol_x2 = 1400, state = "patrol", vx = 80, target_x = 0, target_y = 0, timer = 0, alive = true, w = 28, h = 16})
    table.insert(enemies, {type = "snail", x = 1060, y = 210 - 12, vx = 30, flipped = false, flip_timer = 0, w = 16, h = 12, alive = true})
    table.insert(enemies, {type = "broccoli", x = 1200, y = GROUND_Y - 18, w = 14, h = 18, alive = true, hit_timer = 0})
    table.insert(enemies, {type = "hose", x = 1500, y = GROUND_Y - 16, sweep = 0.5, timer = 2.0, active = false, spray_w = 48, alive = true})
    table.insert(enemies, {type = "snail", x = 1770, y = 180 - 12, vx = 30, flipped = false, flip_timer = 0, w = 16, h = 12, alive = true})
    table.insert(enemies, {type = "pepper", x = 1870, y = GROUND_Y - 14, w = 10, h = 14, alive = true, fire_timer = 1.5})
    table.insert(enemies, {type = "hawk", x = 2300, y = 30, patrol_x1 = 2050, patrol_x2 = 2600, state = "patrol", vx = 80, target_x = 0, target_y = 0, timer = 0, alive = true, w = 28, h = 16})
    table.insert(enemies, {type = "broccoli", x = 2200, y = GROUND_Y - 18, w = 14, h = 18, alive = true, hit_timer = 0})
    table.insert(enemies, {type = "pepper", x = 2500, y = GROUND_Y - 14, w = 10, h = 14, alive = true, fire_timer = 0.5})

    -- Snail platform edges: find their platforms
    for _, e in ipairs(enemies) do
        if e.type == "snail" then
            for _, p in ipairs(platforms) do
                if not p.wall and not p.ground and e.x >= p.x and e.x <= p.x + p.w and math.abs(p.y - 12 - e.y) < 5 then
                    e.plat_x = p.x
                    e.plat_w = p.w
                    break
                end
            end
        end
    end

    -- Hay piles
    table.insert(hay_piles, {x = 400, y = GROUND_Y - 20, w = 32, h = 20})
    table.insert(hay_piles, {x = 1110, y = 130 - 20, w = 32, h = 20})
    table.insert(hay_piles, {x = 1380, y = GROUND_Y - 20, w = 32, h = 20})
    table.insert(hay_piles, {x = 1950, y = GROUND_Y - 20, w = 32, h = 20})
    table.insert(hay_piles, {x = 2480, y = GROUND_Y - 20, w = 32, h = 20})

    -- Glass barrier
    table.insert(glass_barriers, {x = 2550, y = GROUND_Y - 40, w = 6, h = 40, broken = false})
end

-- ============================================================
-- [5] PLAYER STATE
-- ============================================================

local player = {}
local camera_obj = nil
local game_time = 0
local veggies_collected = 0
local high_score = 0

local function reset_player()
    player.x = 50
    player.y = GROUND_Y - 16
    player.vx = 0
    player.vy = 0
    player.w = 24
    player.h = 16
    player.on_ground = false
    player.facing = 1
    player.anim_timer = 0
    player.anim_frame = 0
    player.hp = 3
    player.score = 0
    player.hiding = false
    player.has_shield = false
    player.zoomies_timer = 0
    player.zoomies_active = false
    player.nail_grip_timer = 0
    player.nail_grip_active = false
    player.gripping_wall = false
    player.can_popcorn = true
    player.dash_cooldown = 0
    player.dashing = false
    player.dash_timer = 0
    player.dash_dir = 1
    player.inverted_controls = false
    player.invert_timer = 0
    player.invuln_timer = 0
    player.squeak_charge = 0
    player.squeak_charging = false
    player.squeak_cooldown = 0
    player.afterimages = {}
    player.dead = false
    player.checkpoint_x = 50
    veggies_collected = 0
    particles = {}
    fireballs = {}
    squeaks = {}
    game_time = 0
end

-- ============================================================
-- [6] PARALLAX BACKGROUND
-- ============================================================

local clouds_far = {
    {x = 100, y = 40, w = 48},
    {x = 500, y = 55, w = 48},
    {x = 1000, y = 35, w = 48},
    {x = 1600, y = 50, w = 48},
    {x = 2200, y = 45, w = 48},
    {x = 2800, y = 55, w = 48},
}

local clouds_near = {
    {x = 200, y = 70, w = 32},
    {x = 600, y = 85, w = 32},
    {x = 900, y = 65, w = 32},
    {x = 1300, y = 80, w = 32},
    {x = 1800, y = 60, w = 32},
    {x = 2400, y = 75, w = 32},
    {x = 3000, y = 70, w = 32},
}

local bg_trees_far = {
    {x = 80, img = 1}, {x = 620, img = 2}, {x = 1180, img = 1},
    {x = 1760, img = 2}, {x = 2340, img = 1}, {x = 2920, img = 2},
}
local bg_fence_mid = {
    {x = 260, img = 1}, {x = 900, img = 2}, {x = 1540, img = 1},
    {x = 2180, img = 2}, {x = 2820, img = 1},
}
local bg_garden_near = {
    {x = 40, img = 1}, {x = 480, img = 2}, {x = 980, img = 1},
    {x = 1520, img = 2}, {x = 2060, img = 1}, {x = 2560, img = 2},
    {x = 2980, img = 1},
}

local function parallax_x(world_x, ox, factor)
    return math.floor(world_x + ox * factor + 160 * (1 - factor))
end

local function draw_sky()
    disp.fillRect(0, 0, SCREEN_W, 120, SKY_DARK)
    disp.fillRect(0, 120, SCREEN_W, 120, SKY_MID)
    disp.fillRect(0, 240, SCREEN_W, 80, SKY_LIGHT)
end

local function draw_cluster_set(set, imgs, base_y, ox, factor)
    for _, c in ipairs(set) do
        local img = imgs[c.img]
        if img then
            local _, ih = img:getSize()
            local sx = parallax_x(c.x, ox, factor)
            if sx > -170 and sx < SCREEN_W + 10 then
                img:draw(sx, base_y - ih)
            end
        end
    end
end

local function draw_garden_layers(ox)
    draw_cluster_set(bg_trees_far,  {sprites.bg_trees_far_1, sprites.bg_trees_far_2},  GROUND_Y + 8, ox, 0.10)
    draw_cluster_set(bg_fence_mid,  {sprites.bg_fence_mid_1, sprites.bg_fence_mid_2},  GROUND_Y + 4, ox, 0.25)
    draw_cluster_set(bg_garden_near,{sprites.bg_garden_near_1, sprites.bg_garden_near_2}, GROUND_Y + 2, ox, 0.50)
end

local function draw_cloud(sx, sy, w)
    if sx > SCREEN_W + 10 or sx + w < -10 then return end
    -- Use sprite if available, matched by size
    local img = w >= 48 and sprites.cloud_large or sprites.cloud_small
    if img then
        img:draw(sx, sy)
    else
        disp.fillRect(sx + 4, sy + 2, w - 8, 8, CLOUD_WHITE)
        disp.fillRect(sx + 2, sy + 4, w - 4, 6, CLOUD_WHITE)
        disp.fillRect(sx + 4, sy + 8, w - 8, 2, CLOUD_SHADOW)
    end
end

local function draw_parallax(ox)
    for _, c in ipairs(clouds_far) do
        draw_cloud(parallax_x(c.x, ox, 0.25), c.y, c.w)
    end
    for _, c in ipairs(clouds_near) do
        draw_cloud(parallax_x(c.x, ox, 0.5), c.y, c.w)
    end
end

-- ============================================================
-- [7] DAMAGE SYSTEM (must be before player/enemy logic)
-- ============================================================

local function damage_player(amount)
    if player.invuln_timer > 0 or player.hiding or player.dead then return end
    if player.has_shield then
        player.has_shield = false
        player.invuln_timer = 0.5
        spawn_burst(player.x + player.w / 2, player.y + player.h / 2, ORANGE, 8)
        return
    end
    player.hp = player.hp - amount
    player.invuln_timer = 1.5
    snd_damage()
    if camera_obj then camera_obj:shake(6, 0.3) end
    if player.hp <= 0 then
        player.dead = true
        sfx.play("game_over")
        spawn_burst(player.x + player.w / 2, player.y + player.h / 2, RED, 10)
    end
end

local function knockback_player(from_x)
    local dir = player.x > from_x and 1 or -1
    player.vx = dir * 200
    player.vy = -150
    player.on_ground = false
end

-- ============================================================
-- [8] PLAYER LOGIC
-- ============================================================

local function update_player(dt)
    if player.dead then return end

    local held, pressed = read_buttons()

    -- Timers
    if player.invuln_timer > 0 then player.invuln_timer = player.invuln_timer - dt end
    if player.dash_cooldown > 0 then player.dash_cooldown = player.dash_cooldown - dt end
    if player.squeak_cooldown > 0 then player.squeak_cooldown = player.squeak_cooldown - dt end
    if player.invert_timer > 0 then
        player.invert_timer = player.invert_timer - dt
        if player.invert_timer <= 0 then player.inverted_controls = false end
    end
    if player.zoomies_timer > 0 then
        player.zoomies_timer = player.zoomies_timer - dt
        if player.zoomies_timer <= 0 then player.zoomies_active = false end
    end
    if player.nail_grip_timer > 0 then
        player.nail_grip_timer = player.nail_grip_timer - dt
        if player.nail_grip_timer <= 0 then player.nail_grip_active = false end
    end

    -- Hiding
    if player.hiding then
        if held & P.DOWN == 0 or
           held & P.LEFT ~= 0 or held & P.RIGHT ~= 0 then
            player.hiding = false
        end
        return
    end

    -- Dashing
    if player.dashing then
        player.dash_timer = player.dash_timer - dt
        player.vx = DASH_SPEED * player.dash_dir
        player.vy = 0
        if player.dash_timer <= 0 then player.dashing = false end
        player.x = player.x + player.vx * dt
        player.x = clamp(player.x, 0, WORLD_WIDTH - player.w)
        return
    end

    -- Squeak charging
    if held & P.X ~= 0 and player.squeak_cooldown <= 0 then
        player.squeak_charging = true
        player.squeak_charge = player.squeak_charge + dt
    else
        if player.squeak_charging and player.squeak_charge >= SQUEAK_CHARGE_TIME then
            table.insert(squeaks, {
                x = player.x + (player.facing > 0 and player.w or -10),
                y = player.y,
                vx = SQUEAK_SPEED * player.facing,
                w = 20, h = 16, life = 1.5
            })
            player.squeak_cooldown = SQUEAK_COOLDOWN
            snd_squeak()
        end
        player.squeak_charging = false
        player.squeak_charge = 0
    end

    -- Movement direction (with inversion)
    local move_left = held & P.LEFT ~= 0
    local move_right = held & P.RIGHT ~= 0
    if player.inverted_controls then
        move_left, move_right = move_right, move_left
    end

    local speed = player.zoomies_active and ZOOMIES_SPEED or RUN_SPEED
    if move_left then
        player.vx = -speed
        player.facing = -1
    elseif move_right then
        player.vx = speed
        player.facing = 1
    else
        player.vx = player.vx * FRICTION
        if math.abs(player.vx) < 5 then player.vx = 0 end
    end

    -- Nail grip
    player.gripping_wall = false
    if player.nail_grip_active and not player.on_ground then
        for _, p in ipairs(platforms) do
            if p.wall then
                local touching = false
                if player.facing > 0 and player.x + player.w >= p.x and player.x + player.w <= p.x + p.w + 4 and
                   player.y + player.h > p.y and player.y < p.y + p.h then
                    touching = true
                elseif player.facing < 0 and player.x <= p.x + p.w and player.x >= p.x - 4 and
                   player.y + player.h > p.y and player.y < p.y + p.h then
                    touching = true
                end
                if touching and (move_left or move_right) then
                    player.gripping_wall = true
                    player.vy = 0
                    if held & P.UP ~= 0 then
                        player.y = player.y - NAIL_GRIP_CLIMB_SPEED * dt
                    end
                    break
                end
            end
        end
    end

    -- Jump
    local jump_pressed = pressed & (P.UP | P.A) ~= 0
    if jump_pressed then
        if player.on_ground or player.gripping_wall then
            player.vy = JUMP_POWER
            player.on_ground = false
            player.gripping_wall = false
            snd_jump()
            spawn_burst(player.x + player.w / 2, player.y + player.h, WHITE, 3)
        elseif player.can_popcorn then
            player.vy = POPCORN_JUMP_POWER
            player.can_popcorn = false
            snd_popcorn()
            spawn_burst(player.x + player.w / 2, player.y + player.h / 2, YELLOW, 6)
            -- Knockback nearby enemies
            for _, e in ipairs(enemies) do
                if e.alive and e.type == "snail" and not e.flipped then
                    local ex = e.x + (e.w or 16) / 2
                    local ey = e.y + (e.h or 16) / 2
                    local px = player.x + player.w / 2
                    local py = player.y + player.h / 2
                    if dist(px, py, ex, ey) < POPCORN_KNOCKBACK_RADIUS then
                        e.flipped = true
                        e.flip_timer = 0
                        e.vx = sign(ex - px) * 60
                    end
                end
            end
        end
    end

    -- Dash (B)
    if pressed & P.B ~= 0 and player.dash_cooldown <= 0 and not player.dashing then
        player.dashing = true
        player.dash_timer = DASH_DURATION
        player.dash_dir = player.facing
        player.dash_cooldown = DASH_COOLDOWN
        player.invuln_timer = math.max(player.invuln_timer, DASH_DURATION)
        snd_dash()
        spawn_burst(player.x + player.w / 2, player.y + player.h / 2, WHITE, 4)
    end

    -- Hay hiding
    if pressed & P.DOWN ~= 0 and player.on_ground then
        for _, hay in ipairs(hay_piles) do
            if aabb_overlap(player.x, player.y, player.w, player.h, hay.x, hay.y, hay.w, hay.h) then
                player.hiding = true
                break
            end
        end
    end

    -- Gravity
    if not player.gripping_wall then
        player.vy = player.vy + GRAVITY * dt
    end

    -- Move
    player.x = player.x + player.vx * dt
    player.y = player.y + player.vy * dt
    player.x = clamp(player.x, 0, WORLD_WIDTH - player.w)

    -- Platform collision
    player.on_ground = false
    for _, plat in ipairs(platforms) do
        if not plat.wall then
            if aabb_overlap(player.x, player.y, player.w, player.h, plat.x, plat.y, plat.w, plat.h) then
                if player.vy > 0 and player.y + player.h - player.vy * dt <= plat.y + 4 then
                    player.y = plat.y - player.h
                    player.vy = 0
                    player.on_ground = true
                    player.can_popcorn = true
                    player.gripping_wall = false
                elseif player.vy < 0 and player.y - player.vy * dt >= plat.y + plat.h - 4 then
                    player.y = plat.y + plat.h
                    player.vy = 0
                end
            end
        end
    end

    -- Glass barrier
    for _, gb in ipairs(glass_barriers) do
        if not gb.broken and aabb_overlap(player.x, player.y, player.w, player.h, gb.x, gb.y, gb.w, gb.h) then
            if player.vx > 0 then player.x = gb.x - player.w
            elseif player.vx < 0 then player.x = gb.x + gb.w end
            player.vx = 0
        end
    end

    -- Fall off world
    if player.y > GROUND_Y + 60 then
        damage_player(1)
        if not player.dead then
            player.x = player.checkpoint_x
            player.y = GROUND_Y - 20
            player.vx = 0
            player.vy = 0
        end
    end

    -- Checkpoint (respawn x must sit ON solid ground: AABB overlap needs
    -- player.x + w > plat.x, so the old gap-lip values 550/950 respawned the
    -- pig over the void -> fall loop -> guaranteed death. 620/900 are safely
    -- inside ground segments 600-950.)
    if player.x > 2050 then player.checkpoint_x = 2050
    elseif player.x > 1450 then player.checkpoint_x = 1450
    elseif player.x > 950 then player.checkpoint_x = 900
    elseif player.x > 550 then player.checkpoint_x = 620 end

    -- Animation
    if not player.on_ground then
        player.anim_frame = 3
    elseif math.abs(player.vx) > 10 then
        player.anim_timer = player.anim_timer + dt
        if player.anim_timer > 0.125 then
            player.anim_timer = 0
            player.anim_frame = player.anim_frame == 1 and 2 or 1
        end
    else
        player.anim_frame = 0
        player.anim_timer = 0
    end

    -- Zoomies afterimage
    if player.zoomies_active then
        table.insert(player.afterimages, 1, {x = player.x, y = player.y})
        if #player.afterimages > 4 then table.remove(player.afterimages) end
    else
        player.afterimages = {}
    end
end

-- ============================================================
-- [9] ENEMY LOGIC
-- ============================================================

local function update_enemies(dt)
    for _, e in ipairs(enemies) do
        if not e.alive then
            -- skip dead enemies
        elseif e.type == "hose" then
            e.timer = e.timer + dt
            if e.active then
                if e.timer > 4.8 then e.active = false; e.timer = 0 end
                -- sweep: phase 0..1 across the active window, dwell at extremes
                local phase = clamp(e.timer / 4.8, 0, 1)
                local swing = math.sin(phase * math.pi * 2 - math.pi / 2) * 0.5 + 0.5
                e.sweep = swing                       -- 0=left .. 1=right
                e.dir = swing >= 0.5 and 1 or -1
                local spray_x = e.x - e.spray_w + swing * (e.spray_w - 8) + 8
                if not player.dead and not player.hiding and
                   aabb_overlap(player.x, player.y, player.w, player.h, spray_x, e.y - 4, 24, 16) then
                    player.vx = player.vx + e.dir * 2000 * dt
                end
                if not e.burst_done then sfx.play("water_burst"); e.burst_done = true end
            else
                e.burst_done = false
                if e.timer > 3.0 then e.active = true; e.timer = 0 end
            end

        elseif e.type == "hawk" then
            if e.state == "patrol" then
                e.x = e.x + e.vx * dt
                if e.x > e.patrol_x2 then e.vx = -math.abs(e.vx)
                elseif e.x < e.patrol_x1 then e.vx = math.abs(e.vx) end
                e.y = 30 + math.sin(game_time * 2) * 5
                local dx = math.abs(e.x - player.x)
                if dx < 200 and not player.hiding and not player.dead then
                    e.state = "diving"
                    e.target_x = player.x
                    e.target_y = player.y
                    snd_hawk()
                end
            elseif e.state == "diving" then
                local dx = e.target_x - e.x
                local dy = e.target_y - e.y
                local d = math.sqrt(dx * dx + dy * dy)
                if d > 5 then
                    e.x = e.x + (dx / d) * 250 * dt
                    e.y = e.y + (dy / d) * 250 * dt
                    e.vx = dx > 0 and 80 or -80
                end
                if player.hiding then e.state = "rising" end
                if e.y >= GROUND_Y - 20 or d <= 10 then e.state = "rising" end
                if not player.dead and aabb_overlap(e.x, e.y, 28, 16, player.x, player.y, player.w, player.h) then
                    damage_player(1)
                    knockback_player(e.x)
                    e.state = "rising"
                end
            elseif e.state == "rising" then
                e.y = e.y - 120 * dt
                if e.y <= 30 then e.y = 30; e.state = "patrol" end
            end

        elseif e.type == "snail" then
            if e.flipped then
                e.flip_timer = e.flip_timer + dt
                e.x = e.x + e.vx * dt
                if e.flip_timer > 3 then e.alive = false end
            else
                e.x = e.x + e.vx * dt
                if e.plat_x then
                    if e.x <= e.plat_x then e.x = e.plat_x; e.vx = math.abs(e.vx) end
                    if e.x + e.w >= e.plat_x + e.plat_w then e.x = e.plat_x + e.plat_w - e.w; e.vx = -math.abs(e.vx) end
                end
                if not player.dead and not player.hiding and player.invuln_timer <= 0 and
                   aabb_overlap(player.x, player.y, player.w, player.h, e.x, e.y, e.w, e.h) then
                    damage_player(1)
                    knockback_player(e.x)
                end
            end

        elseif e.type == "broccoli" then
            e.hit_timer = math.max(0, e.hit_timer - dt)
            if not player.dead and not player.hiding and player.invuln_timer <= 0 then
                if aabb_overlap(player.x, player.y, player.w, player.h, e.x, e.y, e.w, e.h) then
                    if player.vy > 0 and player.y + player.h - player.vy * dt <= e.y + 4 then
                        player.vy = -250
                    else
                        damage_player(1)
                        knockback_player(e.x)
                    end
                end
            end
            if player.dashing and aabb_overlap(player.x, player.y, player.w, player.h, e.x, e.y + 9, e.w, 9) then
                e.alive = false
                sfx.play("enemy_defeat")
                spawn_burst(e.x + 7, e.y + 9, GREEN, 6)
                player.score = player.score + 50
            end

        elseif e.type == "pepper" then
            e.fire_timer = e.fire_timer - dt
            if e.fire_timer <= 0 and not player.dead then
                local dx = player.x - e.x
                local dy = player.y - e.y
                local d = math.sqrt(dx * dx + dy * dy)
                if d > 10 and d < 400 then
                    table.insert(fireballs, {
                        x = e.x + 5, y = e.y + 5,
                        vx = (dx / d) * 150,
                        vy = (dy / d) * 150 - 50,
                        life = 3.0
                    })
                end
                e.fire_timer = 2.0
            end
            if not player.dead and not player.hiding and player.invuln_timer <= 0 then
                if aabb_overlap(player.x, player.y, player.w, player.h, e.x, e.y, e.w, e.h) then
                    player.inverted_controls = true
                    player.invert_timer = 4.0
                    player.zoomies_active = true
                    player.zoomies_timer = 4.0
                    player.invuln_timer = 0.5
                    spawn_burst(e.x + 5, e.y + 7, RED, 5)
                end
            end
            if player.dashing and aabb_overlap(player.x, player.y, player.w, player.h, e.x, e.y, e.w, e.h) then
                e.alive = false
                sfx.play("enemy_defeat")
                spawn_burst(e.x + 5, e.y + 7, RED, 6)
                player.score = player.score + 50
            end
        end
    end

    -- Fireballs
    local i = 1
    while i <= #fireballs do
        local fb = fireballs[i]
        fb.x = fb.x + fb.vx * dt
        fb.y = fb.y + fb.vy * dt
        fb.vy = fb.vy + 200 * dt
        fb.life = fb.life - dt
        if not player.dead and not player.hiding and player.invuln_timer <= 0 and
           aabb_overlap(player.x, player.y, player.w, player.h, fb.x, fb.y, 4, 4) then
            damage_player(1)
            knockback_player(fb.x)
            table.remove(fireballs, i)
        elseif fb.life <= 0 or fb.y > GROUND_Y + 20 then
            table.remove(fireballs, i)
        else
            i = i + 1
        end
    end

    -- Squeaks
    i = 1
    while i <= #squeaks do
        local sq = squeaks[i]
        sq.x = sq.x + sq.vx * dt
        sq.life = sq.life - dt
        for _, e in ipairs(enemies) do
            if e.alive and e.type ~= "hawk" then
                local ew = e.w or 16
                local eh = e.h or 16
                if aabb_overlap(sq.x, sq.y, sq.w, sq.h, e.x, e.y, ew, eh) then
                    if e.type == "snail" and not e.flipped then
                        e.flipped = true
                        e.flip_timer = 0
                        e.vx = sign(sq.vx) * 60
                    elseif e.type == "broccoli" or e.type == "pepper" then
                        e.x = e.x + sign(sq.vx) * SQUEAK_PUSH
                    end
                end
            end
        end
        for _, gb in ipairs(glass_barriers) do
            if not gb.broken and aabb_overlap(sq.x, sq.y, sq.w, sq.h, gb.x, gb.y, gb.w, gb.h) then
                gb.broken = true
                spawn_burst(gb.x + 3, gb.y + gb.h / 2, WATER_LIGHT, 8)
            end
        end
        if sq.life <= 0 or sq.x < -50 or sq.x > WORLD_WIDTH + 50 then
            table.remove(squeaks, i)
        else
            i = i + 1
        end
    end
end

-- ============================================================
-- [10] COLLECTIBLE LOGIC
-- ============================================================

local function update_collectibles()
    for _, c in ipairs(collectibles) do
        if not c.collected and
           aabb_overlap(player.x, player.y, player.w, player.h, c.x - 6, c.y - 6, 12, 12) then
            c.collected = true
            if c.type == "carrot" then
                player.score = player.score + 10
                veggies_collected = veggies_collected + 1
                snd_collect_carrot()
                spawn_burst(c.x, c.y, ORANGE, 6)
            elseif c.type == "capsicum" then
                player.score = player.score + 25
                veggies_collected = veggies_collected + 1
                snd_collect_capsicum()
                spawn_burst(c.x, c.y, RED, 6)
            elseif c.type == "zucchini" then
                player.score = player.score + 15
                veggies_collected = veggies_collected + 1
                snd_collect_zucchini()
                spawn_burst(c.x, c.y, GREEN, 6)
            elseif c.type == "strawberry" then
                player.zoomies_active = true
                player.zoomies_timer = ZOOMIES_DURATION
                snd_collect_powerup()
                spawn_burst(c.x, c.y, RED, 8)
            elseif c.type == "vitamin_c" then
                player.has_shield = true
                snd_collect_powerup()
                spawn_burst(c.x, c.y, ORANGE, 8)
            elseif c.type == "nail_grip" then
                player.nail_grip_active = true
                player.nail_grip_timer = NAIL_GRIP_DURATION
                snd_collect_powerup()
                spawn_burst(c.x, c.y, GRAY, 8)
            end
        end
    end
end

-- ============================================================
-- [11] DRAWING
-- ============================================================

local function draw_tiled_platform(sx, sy, w, h)
    if not (sprites.tile_grass and sprites.tile_earth) then
        disp.fillRect(sx, sy + 3, w, h - 3, BROWN)
        disp.fillRect(sx, sy, w, 4, GRASS_GREEN)
        return
    end
    local body_x, body_w = sx, w
    if sprites.tile_edge_l and w >= 16 then
        sprites.tile_edge_l:draw(sx, sy)
        body_x = body_x + 16
        body_w = body_w - 16
    end
    if sprites.tile_edge_r and w >= 32 then
        sprites.tile_edge_r:draw(sx + w - 16, sy)
        body_w = body_w - 16
    end
    if body_w > 0 then
        sprites.tile_grass:drawTiled(body_x, sy, body_w, 16)
        if h > 16 then
            sprites.tile_earth:drawTiled(body_x, sy + 16, body_w, h - 16)
            -- fill strip under the caps too
            sprites.tile_earth:drawTiled(sx, sy + 16, 16, h - 16)
            sprites.tile_earth:drawTiled(sx + w - 16, sy + 16, 16, h - 16)
        end
    end
end

local function draw_world(ox, oy)
    -- Platforms
    for _, plat in ipairs(platforms) do
        local sx = math.floor(plat.x + ox)
        local sy = math.floor(plat.y + oy)
        if sx > SCREEN_W or sx + plat.w < 0 or sy > SCREEN_H or sy + plat.h < 0 then
            -- skip off-screen
        elseif plat.ground then
            draw_tiled_platform(sx, sy, plat.w, plat.h)
        elseif plat.wall then
            disp.fillRect(sx, sy, plat.w, plat.h, GRAY)
            disp.drawRect(sx, sy, plat.w, plat.h, DARK_GRAY)
        else
            draw_tiled_platform(sx, sy, plat.w, plat.h)
        end
    end

    -- Glass barriers
    for _, gb in ipairs(glass_barriers) do
        if not gb.broken then
            local sx = math.floor(gb.x + ox)
            local sy = math.floor(gb.y + oy)
            if sx > -10 and sx < SCREEN_W + 10 then
                disp.fillRect(sx, sy, gb.w, gb.h, disp.rgb(180, 220, 255))
                disp.drawRect(sx, sy, gb.w, gb.h, disp.rgb(200, 230, 255))
            end
        end
    end

    -- Hay piles
    for _, hay in ipairs(hay_piles) do
        local sx = math.floor(hay.x + ox)
        local sy = math.floor(hay.y + oy)
        if sx > -40 and sx < SCREEN_W + 10 then
            local wiggle = 0
            if player.hiding and aabb_overlap(player.x, player.y, player.w, player.h, hay.x, hay.y, hay.w, hay.h) then
                wiggle = math.floor(math.sin(game_time * 6) * 1)
            end
            draw_hay_pile(sx, sy, wiggle)
        end
    end

    -- Collectibles
    for _, c in ipairs(collectibles) do
        if not c.collected then
            local sx = math.floor(c.x + ox)
            local sy = math.floor(c.y + oy + math.sin(game_time * 3) * 2)
            if sx > -16 and sx < SCREEN_W + 16 then
                if c.type == "carrot" then draw_carrot(sx, sy)
                elseif c.type == "capsicum" then draw_capsicum(sx, sy)
                elseif c.type == "zucchini" then draw_zucchini(sx, sy)
                elseif c.type == "strawberry" then draw_strawberry(sx, sy)
                elseif c.type == "vitamin_c" then draw_vitamin_c(sx, sy, game_time)
                elseif c.type == "nail_grip" then draw_nail_grip_item(sx, sy, game_time)
                end
            end
        end
    end

    -- Enemies
    for _, e in ipairs(enemies) do
        if e.alive then
            local sx = math.floor(e.x + ox)
            local sy = math.floor(e.y + oy)
            if sx > -50 and sx < SCREEN_W + 50 then
                if e.type == "hose" then
                    draw_sprinkler(sx, sy, e.sweep or 0.5, e.active, game_time)
                elseif e.type == "hawk" then
                    draw_hawk(sx, sy, e.vx < 0 and -1 or 1, e.state == "diving")
                elseif e.type == "snail" then
                    draw_snail(sx, sy, e.flipped)
                elseif e.type == "broccoli" then
                    draw_broccoli(sx, sy)
                elseif e.type == "pepper" then
                    draw_spicy_pepper(sx, sy)
                end
            end
        end
    end

    -- Fireballs
    for _, fb in ipairs(fireballs) do
        local sx = math.floor(fb.x + ox)
        local sy = math.floor(fb.y + oy)
        if sx > -10 and sx < SCREEN_W + 10 then
            draw_fireball(sx, sy)
        end
    end

    -- Squeaks
    for _, sq in ipairs(squeaks) do
        local sx = math.floor(sq.x + ox)
        local sy = math.floor(sq.y + oy)
        if sx > -30 and sx < SCREEN_W + 30 then
            local r = math.floor((1.5 - sq.life) * 15) + 5
            disp.drawCircle(sx + sq.w / 2, sy + sq.h / 2, r, YELLOW)
            disp.drawCircle(sx + sq.w / 2, sy + sq.h / 2, math.max(1, r - 2), WHITE)
        end
    end

    -- House
    local hsx = math.floor(3050 + ox)
    local hsy = math.floor(GROUND_Y - 36 + oy)
    if hsx > -50 and hsx < SCREEN_W + 50 then
        draw_house(hsx, hsy)
    end

    -- Player
    if not player.dead then
        -- Afterimages
        if player.zoomies_active then
            for ii, ai in ipairs(player.afterimages) do
                local asx = math.floor(ai.x + ox)
                local asy = math.floor(ai.y + oy)
                local shade = math.max(0, 139 - ii * 25)
                disp.fillRect(asx + 4, asy + 4, 16, 8, disp.rgb(shade, shade * 65 // 139, shade * 43 // 139))
            end
        end

        local visible = true
        if player.invuln_timer > 0 then
            visible = math.floor(game_time * 10) % 2 == 0
        end
        if visible and not player.hiding then
            local px = math.floor(player.x + ox)
            local py = math.floor(player.y + oy)
            local sf = math.floor(game_time * 8) % 2 == 0
            draw_guinea_pig(px, py, player.facing, player.anim_frame, player.has_shield, sf, game_time)
            -- Nail grip sparkles
            if player.nail_grip_active and math.floor(game_time * 6) % 3 == 0 then
                disp.fillRect(px + 2, py - 2, 2, 2, WHITE)
                disp.fillRect(px + 18, py - 2, 2, 2, WHITE)
            end
            -- Squeak charge bar
            if player.squeak_charging then
                local bar_w = math.floor(clamp(player.squeak_charge / SQUEAK_CHARGE_TIME, 0, 1) * 20)
                disp.fillRect(px + 2, py - 6, bar_w, 3, YELLOW)
                disp.drawRect(px + 2, py - 6, 20, 3, WHITE)
            end
            -- WHEEK text
            if player.squeak_cooldown > SQUEAK_COOLDOWN - 0.5 then
                disp.drawText(px - 4, py - 14, "WHEEK!", YELLOW, BLACK)
            end
        end
    end

    draw_particles_at(ox, oy)
end

-- ============================================================
-- [12] HUD
-- ============================================================

local function draw_hud()
    disp.fillRect(0, 0, SCREEN_W, 18, BLACK)
    for i = 0, 2 do
        local hx = 4 + i * 18
        if i < player.hp then
            if sprites.heart then
                sprites.heart:drawScaled(hx, 1, 0.5)
            else
                disp.fillRect(hx, 3, 3, 3, RED) disp.fillRect(hx + 4, 3, 3, 3, RED)
                disp.fillRect(hx + 1, 5, 5, 4, RED) disp.fillRect(hx + 2, 9, 3, 2, RED)
            end
        else
            -- empty heart: dark outline, same footprint as the 16px sprite
            disp.drawRect(hx + 2, 3, 5, 5, DARK_GRAY) disp.drawRect(hx + 9, 3, 5, 5, DARK_GRAY)
            disp.fillRect(hx + 3, 8, 10, 3, DARK_GRAY) disp.fillRect(hx + 5, 11, 6, 3, DARK_GRAY)
        end
    end
    disp.drawText(62, 4, "" .. player.score, WHITE, BLACK)
    if sprites.carrot then
        sprites.carrot:drawScaled(104, 1, 0.5)
    end
    disp.drawText(122, 4, veggies_collected .. "/" .. total_veggies, GREEN, BLACK)
    -- Power indicators
    local ix = 168
    if player.zoomies_active then
        local bar = math.floor((player.zoomies_timer / ZOOMIES_DURATION) * 16)
        disp.fillRect(ix, 3, bar, 4, RED)
        disp.drawText(ix, 8, "SPD", RED, BLACK)
        ix = ix + 26
    end
    if player.nail_grip_active then
        local bar = math.floor((player.nail_grip_timer / NAIL_GRIP_DURATION) * 16)
        disp.fillRect(ix, 3, bar, 4, GRAY)
        disp.drawText(ix, 8, "GRP", GRAY, BLACK)
        ix = ix + 26
    end
    if player.has_shield then
        disp.fillCircle(ix + 4, 8, 4, ORANGE)
        ix = ix + 14
    end
    if player.inverted_controls then
        disp.drawText(ix, 4, "!?", MAGENTA, BLACK)
    end
end

-- ============================================================
-- [13] SCENES
-- ============================================================

local function load_high_score()
    local data = game.save.get("guineapig")
    if data and data.high_score then
        -- JSON round-trips numbers as floats; keep display/comparison integral
        high_score = math.floor(tonumber(data.high_score) or 0)
    end
end

local function save_high_score()
    if player.score > high_score then high_score = player.score end
    game.save.set("guineapig", {high_score = high_score})
end

-- Title backdrop helper: tile a garden layer's sprites edge-to-edge across
-- the screen. The gameplay cluster sets are sprinkled over a 3200px world —
-- far too sparse to fill a static 320px title frame.
local function draw_title_strip(imgs, base_y, tile_w)
    if #imgs == 0 then return end
    local n = 0
    for x = 0, SCREEN_W - 1, tile_w do
        local img = imgs[(n % #imgs) + 1]
        if img then
            local _, ih = img:getSize()
            img:draw(x, base_y - ih)
        end
        n = n + 1
    end
end

-- MENU
local menu_scene = {
    enter = function()
        load_high_score()
        game_time = 0
        bgm.play("menu")
    end,
    update = function(dt)
        game_time = game_time + dt
        local _, pressed = read_buttons()
        if pressed & P.A ~= 0 then
            sfx.play("menu_select")
            game.scene.switch("play")
        end
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
            game.quit = true
        end
    end,
    draw = function()
        local FOOTER_TOP = 236
        draw_sky()
        for _, c in ipairs(clouds_near) do
            draw_cloud(c.x % SCREEN_W, c.y * 0.6, c.w)
        end
        if sprites.title_logo then
            local lw = sprites.title_logo:getSize()
            sprites.title_logo:drawScaled((SCREEN_W - lw * 0.75) / 2, 40, 0.75)
        else
            disp.drawText(66, 48, "GUINEA PIG RUN", DARK_BROWN, SKY_DARK)
            disp.drawText(64, 46, "GUINEA PIG RUN", YELLOW, SKY_DARK)
        end
        -- Garden backdrop composed from the game's own layer sprites,
        -- grounded on the footer panel
        draw_title_strip({sprites.bg_trees_far_1, sprites.bg_trees_far_2}, FOOTER_TOP, 160)
        draw_title_strip({sprites.bg_fence_mid_1, sprites.bg_fence_mid_2}, FOOTER_TOP, 160)
        draw_title_strip({sprites.bg_garden_near_1, sprites.bg_garden_near_2}, FOOTER_TOP, 128)
        -- Pig runs home along the footer top; the house occludes it at the
        -- end of each pass
        local run_x = math.floor((game_time * 60) % (SCREEN_W + 80)) - 60
        local rf = math.floor(game_time * 10) % GP_RUN_FRAMES
        if sprites.gp_run_east then
            sprites.gp_run_east:draw(run_x, FOOTER_TOP - GP_FRAME_H, nil,
                {x = rf * GP_FRAME_W, y = 0, w = GP_FRAME_W, h = GP_FRAME_H})
        end
        draw_house(250, FOOTER_TOP - 50)
        -- Footer panel
        local panel = disp.rgb(30, 60, 30)
        disp.fillRect(0, FOOTER_TOP, SCREEN_W, SCREEN_H - FOOTER_TOP, panel)
        disp.drawText(12, 244, "Arrows: Move   Up/" .. key(P.A) .. ": Jump", WHITE, panel)
        disp.drawText(12, 258, key(P.X) .. ": Sonic Squeak (hold)", WHITE, panel)
        disp.drawText(12, 272, key(P.B) .. ": Dash   Down: Hide in hay", WHITE, panel)
        if high_score > 0 then
            disp.drawText(12, 288, "Best: " .. high_score, GOLD, panel)
        end
        if math.floor(game_time * 2) % 2 == 0 then
            disp.drawText(196, 288, "Press " .. key(P.A), YELLOW, panel)
        end
        disp.drawText(196, 302, "ESC to Exit", GRAY, panel)
    end
}

-- PLAY
local play_scene = {
    enter = function()
        build_level()
        reset_player()
        camera_obj = game.camera.new()
        camera_obj:setPosition(160, player.y - 40)
        camera_obj:setBounds(0, 0, WORLD_WIDTH, WORLD_HEIGHT)
        bgm.play("play")
    end,
    update = function(dt)
        game_time = game_time + dt
        local _, pressed = read_buttons()
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
            if pc.ui.confirm("Exit to menu?") then
                save_high_score()
                game.scene.switch("menu")
            end
            return
        end
        if player.dead then
            if pressed & P.A ~= 0 then
                save_high_score()
                game.scene.switch("play")
            end
            update_particles(dt)
            camera_obj:update(dt)
            return
        end
        update_player(dt)
        update_enemies(dt)
        update_collectibles()
        update_particles(dt)
        local target_x = math.max(player.x, 160)
        camera_obj:setTarget(target_x, player.y - 40, 0.1)
        camera_obj:update(dt)
        -- Win
        if aabb_overlap(player.x, player.y, player.w, player.h, 3050, GROUND_Y - 36, 48, 36) then
            if veggies_collected >= total_veggies then
                player.score = player.score + 500
            end
            save_high_score()
            game.scene.switch("win")
        end
    end,
    draw = function()
        local ox, oy = camera_obj:getOffset()
        draw_sky()
        draw_garden_layers(ox)
        draw_parallax(ox)
        draw_world(ox, oy)
        draw_hud()
        if player.dead then
            pc.display.applyEffect("darken", 140)
            disp.fillRect(56, 116, 208, 88, BLACK)
            disp.drawRect(56, 116, 208, 88, RED)
            disp.drawText(116, 130, "GAME OVER", RED, BLACK)
            disp.drawText(100, 150, "Score: " .. player.score, WHITE, BLACK)
            disp.drawText(84, 168, "Veggies: " .. veggies_collected .. "/" .. total_veggies, GREEN, BLACK)
            disp.drawText(84, 186, key(P.A) .. ": Try Again", GRAY, BLACK)
        end
    end
}

-- WIN
local win_time = 0
local win_scene = {
    enter = function()
        win_time = 0
        snd_win()
        bgm.play("win")
    end,
    update = function(dt)
        win_time = win_time + dt
        game_time = game_time + dt
        update_particles(dt)
        local _, pressed = read_buttons()
        if pressed & P.A ~= 0 then game.scene.switch("play") end
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then game.scene.switch("menu") end
    end,
    draw = function()
        local PANEL_TOP = 116
        draw_sky()
        for _, c in ipairs(clouds_near) do
            draw_cloud(c.x % SCREEN_W, c.y * 0.6, c.w)
        end
        -- Garden crest tiled edge-to-edge (menu-style strip; the gameplay
        -- cluster sets are far too sparse to fill a static 320px frame),
        -- grounded on the panel top so the house stands on it
        draw_title_strip({sprites.bg_trees_far_1, sprites.bg_trees_far_2}, PANEL_TOP, 160)
        draw_title_strip({sprites.bg_fence_mid_1, sprites.bg_fence_mid_2}, PANEL_TOP, 160)
        draw_title_strip({sprites.bg_garden_near_1, sprites.bg_garden_near_2}, PANEL_TOP, 128)
        draw_house(136, PANEL_TOP - 36)
        -- Stats panel
        local panel = disp.rgb(20, 40, 20)
        disp.fillRect(40, 116, 240, 120, panel)
        disp.drawRect(40, 116, 240, 120, GOLD)
        disp.drawText(72, 126, "HOME SWEET HOME!", GOLD, panel)
        disp.drawText(100, 146, "Score: " .. player.score, WHITE, panel)
        disp.drawText(84, 162, "Veggies: " .. veggies_collected .. "/" .. total_veggies, GREEN, panel)
        if veggies_collected >= total_veggies then
            disp.drawText(64, 178, "ALL VEGGIES! +500!", GOLD, panel)
        end
        if player.score >= high_score and player.score > 0 then
            disp.drawText(76, 196, "NEW HIGH SCORE!", YELLOW, panel)
        else
            disp.drawText(80, 196, "Best: " .. high_score, GRAY, panel)
        end
        disp.drawText(74, 212, key(P.A) .. ": Play Again", WHITE, panel)
        disp.drawText(92, 226, "ESC: Menu", GRAY, panel)
        -- Foreground ground echo, matching the gameplay platform look
        disp.fillRect(0, 280, SCREEN_W, SCREEN_H - 280, BROWN)
        disp.fillRect(0, 280, SCREEN_W, 4, GRASS_GREEN)
        draw_particles_at(0, 0)
    end
}

-- ============================================================
-- [14] MAIN LOOP
-- ============================================================

game.scene.add("menu", menu_scene)
game.scene.add("play", play_scene)
game.scene.add("win", win_scene)
game.scene.switch("menu")

while not game.quit do
    pc.perf.beginFrame()
    input.update()
    local dt = pc.perf.getFrameTime() / 1000.0
    if dt > 0.05 then dt = 0.05 end
    game.scene.update(dt)
    bgm.update()
    game.scene.draw()
    pc.display.flush()
    pc.perf.endFrame()
end
