-- Minesweeper for PicoDeck
-- A fully playable minesweeper game using the picocalc API

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local gfx   = pc.graphics

-- ── Config ────────────────────────────────────────────────────────────────────

local COLS         = 9
local ROWS         = 9
local MINES        = 10
local CELL_SIZE    = 32
local GRID_OFFSET_X = (disp.getWidth() - COLS * CELL_SIZE) // 2
local GRID_OFFSET_Y = 36
local HEADER_HEIGHT = 28

-- Cell states
local CELL_COVERED   = 0
local CELL_REVEALED  = 1
local CELL_FLAGGED   = 2
local CELL_QUESTION  = 3

-- Game states
local STATE_PLAYING = 0
local STATE_WON     = 1
local STATE_LOST    = 2

-- Colors
local BG_COLOR      = disp.rgb(20, 20, 40)
local HEADER_BG     = disp.rgb(15, 15, 30)
local CURSOR_COLOR  = disp.rgb(255, 255, 0)
local TEXT_COLOR    = disp.WHITE
local DIM_COLOR     = disp.GRAY

-- Sprites directory
local SPRITE_DIR = APP_DIR .. "/sprites/"

-- ── State ─────────────────────────────────────────────────────────────────────

local grid         = {}
local cursor       = {x = 4, y = 4}
local game_state   = STATE_PLAYING
local mines_remaining = MINES
local timer        = 0
local start_time   = 0
local first_click  = true

-- Loaded sprites
local sprites = {}

-- ── Sprite Loading ────────────────────────────────────────────────────────────

local function load_sprites()
    local sprite_files = {
        covered       = "covered.png",
        revealed_empty = "revealed_empty.png",
        mine          = "mine.png",
        flagged       = "flagged.png",
        question      = "question.png",
        mine_exploded = "mine_exploded.png",
        n1            = "n1.png",
        n2            = "n2.png",
        n3            = "n3.png",
        n4            = "n4.png",
        n5            = "n5.png",
        n6            = "n6.png",
        n7            = "n7.png",
        n8            = "n8.png",
    }
    
    for key, filename in pairs(sprite_files) do
        local path = SPRITE_DIR .. filename
        local ok, img = pcall(gfx.image.load, path)
        if ok and img then
            sprites[key] = img
        else
            sprites[key] = nil
        end
    end
end

-- ── Board Generation ─────────────────────────────────────────────────────────

local function init_board()
    grid = {}
    for y = 1, ROWS do
        grid[y] = {}
        for x = 1, COLS do
            grid[y][x] = {
                mine       = false,
                revealed   = false,
                flagged    = false,
                question   = false,
                adjacent   = 0,
                state      = CELL_COVERED
            }
        end
    end
    cursor = {x = 5, y = 5}
    game_state  = STATE_PLAYING
    mines_remaining = MINES
    timer       = 0
    start_time  = pc.sys.getTimeMs()
    first_click = true
end

local function place_mines(safe_x, safe_y)
    local placed = 0
    while placed < MINES do
        local x = math.random(1, COLS)
        local y = math.random(1, ROWS)
        
        -- Don't place mine on first click or adjacent cells
        local dx = math.abs(x - safe_x)
        local dy = math.abs(y - safe_y)
        if not grid[y][x].mine and not (dx <= 1 and dy <= 1) then
            grid[y][x].mine = true
            placed = placed + 1
        end
    end
    
    -- Calculate adjacent mine counts
    for y = 1, ROWS do
        for x = 1, COLS do
            if not grid[y][x].mine then
                local count = 0
                for dy = -1, 1 do
                    for dx = -1, 1 do
                        local nx, ny = x + dx, y + dy
                        if nx >= 1 and nx <= COLS and ny >= 1 and ny <= ROWS then
                            if grid[ny][nx].mine then
                                count = count + 1
                            end
                        end
                    end
                end
                grid[y][x].adjacent = count
            end
        end
    end
end

-- ── Game Logic ────────────────────────────────────────────────────────────────

local function reveal_cell(x, y)
    if x < 1 or x > COLS or y < 1 or y > ROWS then
        return
    end
    
    local cell = grid[y][x]
    
    if cell.revealed or cell.flagged then
        return
    end
    
    cell.revealed = true
    cell.state = CELL_REVEALED
    
    if cell.mine then
        return -- Hit a mine, game over
    end
    
    -- Flood fill for empty cells
    if cell.adjacent == 0 then
        for dy = -1, 1 do
            for dx = -1, 1 do
                reveal_cell(x + dx, y + dy)
            end
        end
    end
end

local function toggle_flag(x, y)
    if x < 1 or x > COLS or y < 1 or y > ROWS then
        return
    end
    
    local cell = grid[y][x]
    
    if cell.revealed then
        return
    end
    
    if cell.flagged then
        cell.flagged = false
        cell.question = false
        cell.state = CELL_COVERED
        mines_remaining = mines_remaining + 1
    elseif cell.question then
        cell.question = false
        cell.state = CELL_COVERED
    else
        cell.flagged = true
        cell.state = CELL_FLAGGED
        mines_remaining = mines_remaining - 1
    end
end

local function toggle_question(x, y)
    if x < 1 or x > COLS or y < 1 or y > ROWS then
        return
    end
    
    local cell = grid[y][x]
    
    if cell.revealed or cell.flagged then
        return
    end
    
    if cell.question then
        cell.question = false
        cell.state = CELL_COVERED
    else
        cell.question = true
        cell.state = CELL_QUESTION
    end
end

local function chord_reveal(x, y)
    if x < 1 or x > COLS or y < 1 or y > ROWS then
        return
    end
    
    local cell = grid[y][x]
    
    if not cell.revealed or cell.adjacent == 0 then
        return
    end
    
    -- Count adjacent flags
    local flag_count = 0
    for dy = -1, 1 do
        for dx = -1, 1 do
            local nx, ny = x + dx, y + dy
            if nx >= 1 and nx <= COLS and ny >= 1 and ny <= ROWS then
                if grid[ny][nx].flagged then
                    flag_count = flag_count + 1
                end
            end
        end
    end
    
    -- If flag count matches adjacent number, reveal unflagged neighbors
    if flag_count == cell.adjacent then
        for dy = -1, 1 do
            for dx = -1, 1 do
                local nx, ny = x + dx, y + dy
                if nx >= 1 and nx <= COLS and ny >= 1 and ny <= ROWS then
                    local neighbor = grid[ny][nx]
                    if not neighbor.revealed and not neighbor.flagged then
                        reveal_cell(nx, ny)
                    end
                end
            end
        end
    end
end

local function check_game_state()
    local unrevealed = 0
    local exploded_mine_x, exploded_mine_y
    
    for y = 1, ROWS do
        for x = 1, COLS do
            local cell = grid[y][x]
            if not cell.revealed then
                unrevealed = unrevealed + 1
            elseif cell.mine then
                exploded_mine_x, exploded_mine_y = x, y
            end
        end
    end
    
    if exploded_mine_x then
        game_state = STATE_LOST
        -- Mark the exploded mine
        grid[exploded_mine_y][exploded_mine_x].exploded = true
        return
    end
    
    if unrevealed == MINES then
        game_state = STATE_WON
        return
    end
end

local function reveal_all_mines()
    for y = 1, ROWS do
        for x = 1, COLS do
            local cell = grid[y][x]
            if cell.mine and not cell.flagged then
                cell.revealed = true
                cell.state = CELL_REVEALED
            end
        end
    end
end

-- ── Drawing ──────────────────────────────────────────────────────────────────

local function draw_sprite(sprite_key, x, y)
    local sprite = sprites[sprite_key]
    if sprite then
        sprite:draw(x, y)
    end
end

local function draw_cell(x, y)
    local cell = grid[y][x]
    local screen_x = GRID_OFFSET_X + (x - 1) * CELL_SIZE
    local screen_y = GRID_OFFSET_Y + (y - 1) * CELL_SIZE
    
    -- Draw sprite based on state
    if cell.revealed then
        if cell.mine then
            if cell.exploded then
                draw_sprite("mine_exploded", screen_x, screen_y)
            else
                draw_sprite("mine", screen_x, screen_y)
            end
        elseif cell.adjacent > 0 then
            draw_sprite("n" .. cell.adjacent, screen_x, screen_y)
        else
            draw_sprite("revealed_empty", screen_x, screen_y)
        end
    elseif cell.flagged then
        draw_sprite("covered", screen_x, screen_y)
        draw_sprite("flagged", screen_x, screen_y)
    elseif cell.question then
        draw_sprite("covered", screen_x, screen_y)
        draw_sprite("question", screen_x, screen_y)
    else
        draw_sprite("covered", screen_x, screen_y)
    end
    
    -- Draw cursor
    if cursor.x == x and cursor.y == y and game_state == STATE_PLAYING then
        disp.drawRect(screen_x + 1, screen_y + 1, CELL_SIZE - 2, CELL_SIZE - 2, CURSOR_COLOR)
    end
end

local function draw_board()
    for y = 1, ROWS do
        for x = 1, COLS do
            draw_cell(x, y)
        end
    end
end

local function draw_header()
    disp.fillRect(0, 0, disp.getWidth(), HEADER_HEIGHT + 8, HEADER_BG)
    
    -- Mine count
    disp.drawText(8, 6, "Mines:", TEXT_COLOR, HEADER_BG)
    disp.drawText(56, 6, string.format("%2d", mines_remaining), 
                  mines_remaining < 0 and disp.RED or TEXT_COLOR, HEADER_BG)
    
    -- Timer
    local elapsed = 0
    if game_state == STATE_PLAYING and not first_click then
        elapsed = math.floor((pc.sys.getTimeMs() - start_time) / 1000)
    end
    disp.drawText(disp.getWidth() // 2 - 20, 6, "Time:", DIM_COLOR, HEADER_BG)
    disp.drawText(disp.getWidth() // 2 + 12, 6, string.format("%3d", elapsed), TEXT_COLOR, HEADER_BG)
    
    -- Game state indicator
    if game_state == STATE_WON then
        disp.drawText(disp.getWidth() - 70, 6, "WIN!", disp.rgb(50, 255, 50), HEADER_BG)
    elseif game_state == STATE_LOST then
        disp.drawText(disp.getWidth() - 80, 6, "BOOM!", disp.RED, HEADER_BG)
    end
end

local function draw_footer()
    local footer_y = GRID_OFFSET_Y + ROWS * CELL_SIZE + 8
    disp.fillRect(0, footer_y, disp.getWidth(), disp.getHeight() - footer_y, HEADER_BG)

    if game_state == STATE_LOST then
        disp.drawText(8, footer_y + 4, "Enter: Play again  Esc: Exit", DIM_COLOR, HEADER_BG)
    elseif game_state == STATE_WON then
        disp.drawText(8, footer_y + 4, "Enter: Play again  Esc: Exit", DIM_COLOR, HEADER_BG)
    else
        disp.drawText(8, footer_y + 4, "F4:Reveal F5:Flag Del:Chord Bksp:? Esc:Exit", DIM_COLOR, HEADER_BG)
    end
end

-- ── Input Handling ───────────────────────────────────────────────────────────

local REPEAT_DELAY  = 200  -- ms before key repeat starts
local REPEAT_RATE   = 80   -- ms between repeats

local last_dpad     = 0    -- last held d-pad bitmask
local dpad_time     = 0    -- timestamp of last d-pad move

local DPAD_MASK = input.BTN_UP | input.BTN_DOWN | input.BTN_LEFT | input.BTN_RIGHT

local function handle_input()
    input.update()
    local pressed = input.getButtonsPressed()

    -- Exit
    if pressed & input.BTN_ESC ~= 0 then
        return "exit"
    end

    if game_state ~= STATE_PLAYING then
        if pressed & input.BTN_ENTER ~= 0 then
            init_board()
        end
        return
    end

    -- D-pad movement with key repeat
    local held = input.getButtons() & DPAD_MASK
    local now  = pc.sys.getTimeMs()
    local move = false

    if held ~= 0 then
        if held ~= last_dpad then
            -- New direction pressed — move immediately
            move = true
            dpad_time = now
        else
            -- Same direction held — apply repeat delay then repeat rate
            local elapsed = now - dpad_time
            if elapsed >= REPEAT_DELAY then
                -- Snap to repeat grid so rate stays consistent
                move = true
                dpad_time = dpad_time + REPEAT_RATE
                if now - dpad_time >= REPEAT_RATE then
                    dpad_time = now
                end
            end
        end
    end
    last_dpad = held

    if move then
        if held & input.BTN_UP ~= 0 and cursor.y > 1 then
            cursor.y = cursor.y - 1
        end
        if held & input.BTN_DOWN ~= 0 and cursor.y < ROWS then
            cursor.y = cursor.y + 1
        end
        if held & input.BTN_LEFT ~= 0 and cursor.x > 1 then
            cursor.x = cursor.x - 1
        end
        if held & input.BTN_RIGHT ~= 0 and cursor.x < COLS then
            cursor.x = cursor.x + 1
        end
    end
    
    -- Reveal cell
    if pressed & input.BTN_F4 ~= 0 then
        local x, y = cursor.x, cursor.y
        if first_click then
            first_click = false
            place_mines(x, y)
            start_time = pc.sys.getTimeMs()
        end
        reveal_cell(x, y)
        check_game_state()
        if game_state == STATE_LOST then
            reveal_all_mines()
        end
    end
    
    -- Toggle flag
    if pressed & input.BTN_F5 ~= 0 then
        toggle_flag(cursor.x, cursor.y)
    end
    
    -- Chord reveal
    if pressed & input.BTN_DEL ~= 0 then
        chord_reveal(cursor.x, cursor.y)
    end
    
    -- Toggle question mark
    if pressed & input.BTN_BACKSPACE ~= 0 then
        toggle_question(cursor.x, cursor.y)
    end
    
    return nil
end

-- ── Main Loop ────────────────────────────────────────────────────────────────

math.randomseed(pc.sys.getTimeMs())
load_sprites()
init_board()

while true do
    local result = handle_input()
    if result == "exit" then
        return
    end
    
    disp.clear(BG_COLOR)
    draw_header()
    draw_board()
    draw_footer()
    disp.flush()
    
    pc.sys.sleep(16)
end