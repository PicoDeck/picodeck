-- Snake for PicoDeck
-- A fully playable snake game using the picocalc API

local pc    = picocalc
local disp  = pc.display
local input = pc.input

-- Input: the logical gamepad (rebindable in Settings > Controls) where the
-- firmware has it, raw keys on older firmware.
local gp = pc.gamepad
local P, read_buttons, key_label
if gp then
    P = {UP = gp.PAD_UP, DOWN = gp.PAD_DOWN, LEFT = gp.PAD_LEFT,
         RIGHT = gp.PAD_RIGHT, A = gp.PAD_A}
    read_buttons = function() return gp.getButtons(), gp.getButtonsPressed() end
    key_label = function() return gp.getLabel(gp.PAD_A) or "?" end
else
    P = {UP = input.BTN_UP, DOWN = input.BTN_DOWN, LEFT = input.BTN_LEFT,
         RIGHT = input.BTN_RIGHT, A = input.BTN_ENTER}
    read_buttons = function() return input.getButtons(), input.getButtonsPressed() end
    key_label = function() return "Enter" end
end

-- A, or Enter as before the gamepad, answers "play again". Enter counts only
-- when it pressed no gamepad button: a player who bound it to one gets that
-- button's meaning alone.
local function confirm_pressed(pad_pressed)
    if pad_pressed & P.A ~= 0 then return true end
    return gp ~= nil and pad_pressed == 0
        and input.getButtonsPressed() & input.BTN_ENTER ~= 0
end

-- ── Config ────────────────────────────────────────────────────────────────────

local CELL  = 10          -- grid cell size in pixels
local COLS  = math.floor(disp.getWidth()  / CELL)   -- 32
local ROWS  = math.floor(disp.getHeight() / CELL)   -- 32
local SPEED = 8           -- frames between moves (lower = faster)

-- Colours
local BG       = disp.rgb(10, 20, 10)
local GRID     = disp.rgb(15, 30, 15)
local SNAKE_H  = disp.rgb(80, 220, 80)
local SNAKE_B  = disp.rgb(50, 160, 50)
local FOOD_C   = disp.rgb(220, 60, 60)
local TEXT_C   = disp.WHITE
local DIM_C    = disp.GRAY

-- ── State ─────────────────────────────────────────────────────────────────────

local snake   = {}    -- array of {x, y} from head (index 1) to tail
local dir     = {x=1, y=0}
local next_dir = {x=1, y=0}
local food    = {x=0, y=0}
local score   = 0
local frame   = 0
local alive   = true
local highscore = 0

-- ── Helpers ───────────────────────────────────────────────────────────────────

local function reset()
    snake = { {x=COLS//2, y=ROWS//2}, {x=COLS//2-1, y=ROWS//2} }
    dir      = {x=1, y=0}
    next_dir = {x=1, y=0}
    score = 0
    frame = 0
    alive = true
    -- Place food
    food.x = math.random(1, COLS)
    food.y = math.random(1, ROWS)
end

local function place_food()
    while true do
        local fx = math.random(1, COLS)
        local fy = math.random(1, ROWS)
        local ok = true
        for _, seg in ipairs(snake) do
            if seg.x == fx and seg.y == fy then ok = false; break end
        end
        if ok then food.x = fx; food.y = fy; return end
    end
end

local function draw_grid()
    disp.clear(BG)
    -- light grid lines
    for x = 0, COLS do
        disp.drawLine(x*CELL, 0, x*CELL, ROWS*CELL, GRID)
    end
    for y = 0, ROWS do
        disp.drawLine(0, y*CELL, COLS*CELL, y*CELL, GRID)
    end
end

local function draw_snake()
    for i, seg in ipairs(snake) do
        local c = (i == 1) and SNAKE_H or SNAKE_B
        disp.fillRect((seg.x-1)*CELL+1, (seg.y-1)*CELL+1, CELL-2, CELL-2, c)
    end
end

local function draw_food()
    disp.fillRect((food.x-1)*CELL+1, (food.y-1)*CELL+1, CELL-2, CELL-2, FOOD_C)
end

local function draw_hud()
    -- Score overlay (bottom strip)
    disp.fillRect(0, ROWS*CELL, disp.getWidth(), disp.getHeight()-ROWS*CELL, disp.BLACK)
    disp.drawText(4,  ROWS*CELL+4, "Score: " .. score,     TEXT_C, disp.BLACK)
    disp.drawText(disp.getWidth()-90, ROWS*CELL+4, "Hi: " .. highscore, DIM_C, disp.BLACK)
end

local function game_over_screen()
    if score > highscore then highscore = score end
    disp.clear(disp.BLACK)
    disp.drawText(90, 120, "GAME OVER", TEXT_C,  disp.BLACK)
    disp.drawText(90, 140, "Score: " .. score,   FOOD_C,  disp.BLACK)
    disp.drawText(76, 160, "Hi: "    .. highscore, DIM_C, disp.BLACK)
    disp.drawText(40, 190, key_label() .. ": Play again  Esc: Quit", DIM_C, disp.BLACK)
    disp.flush()

    while true do
        input.update()
        local _, p = read_buttons()
        if confirm_pressed(p) then return "restart" end
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return "quit" end
        pc.sys.sleep(16)
    end
end

-- ── Main loop ─────────────────────────────────────────────────────────────────

math.randomseed(pc.sys.getTimeMs())
reset()

while true do
    -- Input
    input.update()
    local pressed = input.getButtonsPressed()
    if pressed & input.BTN_ESC ~= 0 then return end

    local held = read_buttons()
    if held & P.UP    ~= 0 and dir.y == 0 then next_dir = {x=0, y=-1} end
    if held & P.DOWN  ~= 0 and dir.y == 0 then next_dir = {x=0, y=1}  end
    if held & P.LEFT  ~= 0 and dir.x == 0 then next_dir = {x=-1,y=0}  end
    if held & P.RIGHT ~= 0 and dir.x == 0 then next_dir = {x=1, y=0}  end

    -- Move every SPEED frames
    if frame % SPEED == 0 then
        dir = next_dir

        -- New head position
        local head = snake[1]
        local nx = head.x + dir.x
        local ny = head.y + dir.y

        -- Wall collision (wrap around instead of die — feels better on small screen)
        nx = ((nx - 1) % COLS) + 1
        ny = ((ny - 1) % ROWS) + 1

        -- Self collision
        for _, seg in ipairs(snake) do
            if seg.x == nx and seg.y == ny then
                alive = false
                break
            end
        end

        if alive then
            table.insert(snake, 1, {x=nx, y=ny})

            -- Ate food?
            if nx == food.x and ny == food.y then
                score = score + 10
                place_food()
                -- Speed up slightly every 50 points
                SPEED = math.max(3, SPEED - (score // 50 > 0 and 1 or 0))
            else
                table.remove(snake)   -- remove tail
            end
        end
    end

    -- Draw
    draw_grid()
    draw_snake()
    draw_food()
    draw_hud()
    disp.flush()

    if not alive then
        local action = game_over_screen()
        if action == "restart" then
            reset()
        else
            return
        end
    end

    frame = frame + 1
    pc.sys.sleep(16)
end
