-- Terminal Example App for PicoDeck
-- Demonstrates the terminal emulator SDK with VT100 escape sequences

local pc = picocalc
local disp = pc.display
local input = pc.input
local gp = pc.gamepad

-- The key bound to a gamepad button, for the text the demos print.
local function pad_key(btn) return gp.getLabel(btn) or "?" end

-- "Press any key": a key, a character or any gamepad button. The terminal's
-- own wait-for-key calls read the keyboard's buttons only, so a controller (or
-- a typed letter) could not get past it. After the press it waits for release
-- and drops the queued characters: a held key repeats its character, which
-- would otherwise skip the next "Press any key" screen.
local function wait_any()
    while true do
        input.update()
        if input.getButtonsPressed() ~= 0 or gp.getButtonsPressed() ~= 0
           or input.getChar() then
            break
        end
        pc.sys.sleep(16)
    end
    while true do
        input.update()
        while input.getChar() do end
        if input.getButtons() == 0 and gp.getButtons() == 0 then
            return
        end
        pc.sys.sleep(16)
    end
end

local function demo_basic_text(term)
    term:write("\x1b[2J")  -- Clear screen
    term:write("\x1b[H")    -- Home cursor
    
    term:write("=== PicoDeck Terminal Demo ===\n\n")
    term:write("This is plain text.\n")
    term:write("This is \x1b[1mbold\x1b[0m text.\n")
    term:write("This is \x1b[3mitalic\x1b[0m text.\n")
    term:write("This is \x1b[4munderlined\x1b[0m text.\n")
    term:write("This is \x1b[7minverted\x1b[0m text.\n")
end

local function demo_colors(term)
    term:write("\n=== 16 Color Palette ===\n")
    
    local colors = {
        {30, "Black"}, {31, "Red"}, {32, "Green"}, {33, "Yellow"},
        {34, "Blue"}, {35, "Magenta"}, {36, "Cyan"}, {37, "White"},
        {90, "Bright Black"}, {91, "Bright Red"}, {92, "Bright Green"}, {93, "Bright Yellow"},
        {94, "Bright Blue"}, {95, "Bright Magenta"}, {96, "Bright Cyan"}, {97, "Bright White"}
    }
    
    for i, c in ipairs(colors) do
        term:write(string.format("\x1b[%dm%-15s\x1b[0m", c[1], c[2]))
        if i % 4 == 0 then
            term:write("\n")
        end
    end
end

local function demo_rgb(term)
    term:write("\n=== 24-bit RGB Colors ===\n")
    
    term:write(string.format("\x1b[38;2;255;0;0mRed\x1b[0m "))
    term:write(string.format("\x1b[38;2;0;255;0mGreen\x1b[0m "))
    term:write(string.format("\x1b[38;2;0;0;255mBlue\x1b[0m\n"))
    
    term:write(string.format("\x1b[38;2;255;165;0mOrange\x1b[0m "))
    term:write(string.format("\x1b[38;2;255;105;180mPink\x1b[0m "))
    term:write(string.format("\x1b[38;2;0;255;255mCyan\x1b[0m\n"))
end

local function demo_cursor(term)
    term:write("\n=== Cursor Movement ===\n")
    
    term:write("Home position, ")
    term:write("\x1b[5G")  -- Cursor to column 5
    term:write("moved here\n")
    
    term:write("\x1b[10;10H")  -- Cursor to row 10, col 10
    term:write("Cursor at (10,10)\n")
    
    term:write("\x1b[H")  -- Home
end

local function demo_box(term)
    term:write("\n=== Box Drawing ===\n")
    
    term:write("┌────────────┐\n")
    term:write("│            │\n")
    term:write("│   Text     │\n")
    term:write("│            │\n")
    term:write("└────────────┘\n")
end

local function demo_scrollback(term)
    term:write("\n=== Scrollback Test ===\n")
    term:write("Generating lines...\n")
    
    for i = 1, 40 do
        term:write(string.format("Line %d of scrollback buffer\n", i))
    end
    
    term:write("\nUse scrollback to view previous lines!\n")
end

local function demo_animation(term)
    term:write("\n=== Animation Demo ===\n")
    term:write("Watch the spinner below...\n\n")
end

local function demo_font(term)
    term:write("\n=== Font Demo ===\n\n")
    
    term:write("Current font: " .. term:getFont() .. "\n\n")
    
    term:write("Press " .. pad_key(gp.PAD_LEFT) .. "/" .. pad_key(gp.PAD_RIGHT) .. " to switch fonts:\n")
    term:write("- scientifica (default)\n")
    term:write("- scientifica_bold\n")
    term:write("\n")
    
    term:write("Sample text in scientifica:\n")
    term:setFont("scientifica")
    term:write("The quick brown fox\n")
    term:write("jumps over the lazy dog.\n\n")
    
    term:write("Sample text in scientifica_bold:\n")
    term:setFont("scientifica_bold")
    term:write("The quick brown fox\n")
    term:write("jumps over the lazy dog.\n")
end

-- Demo state management
local DEMO_BASIC = 1
local DEMO_COLORS = 2
local DEMO_RGB = 3
local DEMO_CURSOR = 4
local DEMO_BOX = 5
local DEMO_SCROLLBACK = 6
local DEMO_ANIMATION = 7
local DEMO_FONT = 8
local DEMO_COMPLETE = 9

local current_demo = DEMO_BASIC

local function run_demo(term, demo_num)
    term:clear()
    if demo_num == DEMO_BASIC then
        demo_basic_text(term)
    elseif demo_num == DEMO_COLORS then
        demo_colors(term)
    elseif demo_num == DEMO_RGB then
        demo_rgb(term)
    elseif demo_num == DEMO_CURSOR then
        demo_cursor(term)
    elseif demo_num == DEMO_BOX then
        demo_box(term)
    elseif demo_num == DEMO_SCROLLBACK then
        demo_scrollback(term)
    elseif demo_num == DEMO_ANIMATION then
        demo_animation(term)
    elseif demo_num == DEMO_FONT then
        demo_font(term)
    end
end

-- Main app
local function main()
    local term = pc.terminal.new(53, 26, 1000)
    
    local font = pc.sysconfig.get("terminal_font")
    if font then
        term:setFont(font)
    end
    
    term:setCursorVisible(true)
    term:setCursorBlink(true)
    
    run_demo(term, current_demo)
    term:render()
    disp.flush()
    
    term:write("\nPress any key to continue...")
    term:render()
    disp.flush()
    wait_any()
    
    current_demo = DEMO_COLORS
    run_demo(term, current_demo)
    term:render()
    disp.flush()
    term:write("\nPress any key to continue...")
    term:render()
    disp.flush()
    wait_any()
    
    current_demo = DEMO_RGB
    run_demo(term, current_demo)
    term:render()
    disp.flush()
    term:write("\nPress any key to continue...")
    term:render()
    disp.flush()
    wait_any()
    
    current_demo = DEMO_CURSOR
    run_demo(term, current_demo)
    term:render()
    disp.flush()
    term:write("\nPress any key to continue...")
    term:render()
    disp.flush()
    wait_any()
    
    current_demo = DEMO_BOX
    run_demo(term, current_demo)
    term:render()
    disp.flush()
    term:write("\nPress any key to continue...")
    term:render()
    disp.flush()
    wait_any()
    
    -- Interactive font demo
    local fonts = {"scientifica", "scientifica_bold"}
    local font_idx = 1

    local redraw = true
    while true do
        if redraw then
            term:clear()
            term:write("=== Font Demo ===\n\n")
            term:write("Font: " .. fonts[font_idx] .. "\n\n")
            term:write("Sample:\n")
            term:setFont(fonts[font_idx])
            term:write("The quick brown fox\n")
            term:write("jumps over the lazy dog.\n\n")
            term:write(pad_key(gp.PAD_LEFT) .. "/" .. pad_key(gp.PAD_RIGHT) .. ": switch font\n")
            term:write(pad_key(gp.PAD_A) .. ": continue\n")
            term:render()
            disp.flush()
            redraw = false
        end

        input.update()
        local nav = gp.getNavPressed()
        if nav & gp.PAD_A ~= 0 then
            break
        elseif nav & gp.PAD_LEFT ~= 0 then
            font_idx = font_idx - 1
            if font_idx < 1 then font_idx = #fonts end
            redraw = true
        elseif nav & gp.PAD_RIGHT ~= 0 then
            font_idx = font_idx + 1
            if font_idx > #fonts then font_idx = 1 end
            redraw = true
        end
        pc.sys.sleep(30)
    end

    current_demo = DEMO_ANIMATION
    run_demo(term, current_demo)
    term:render()
    disp.flush()

    local scroll_offset = 0
    local frames = {"|", "/", "-", "\\"}
    local anim_frame = 1
    local anim_start_time = pc.sys.getTimeMs()

    while true do
        input.update()
        -- getNavPressed() repeats a held D-pad direction (input.setRepeat's
        -- timing) and gives A and B once per press: no timers needed here.
        local nav = gp.getNavPressed()
        local now = pc.sys.getTimeMs()

        if nav & gp.PAD_B ~= 0 then
            return
        end

        -- Advance from animation to scrollback demo on A
        if current_demo == DEMO_ANIMATION then
            if nav & gp.PAD_A ~= 0 then
                current_demo = DEMO_SCROLLBACK
                run_demo(term, current_demo)
                term:render()
                disp.flush()
            else
                local elapsed = now - anim_start_time
                local new_frame = (math.floor(elapsed / 150) % 4) + 1
                if new_frame ~= anim_frame then
                    anim_frame = new_frame
                    term:write("\rSpinning: " .. frames[anim_frame] .. "   ")
                    term:render()
                    disp.flush()
                end
            end
        elseif current_demo == DEMO_SCROLLBACK then
            if nav & gp.PAD_UP ~= 0 then
                local total = term:getScrollbackCount()
                if total > 0 then
                    scroll_offset = scroll_offset + 1
                    if scroll_offset > total then scroll_offset = total end
                    term:setScrollbackOffset(scroll_offset)
                end
            elseif nav & gp.PAD_DOWN ~= 0 then
                if scroll_offset > 0 then
                    scroll_offset = scroll_offset - 1
                    term:setScrollbackOffset(scroll_offset)
                end
            elseif scroll_offset > 0 and nav & (gp.PAD_LEFT | gp.PAD_RIGHT | gp.PAD_A) ~= 0 then
                scroll_offset = 0
                term:setScrollbackOffset(0)
            end

            if scroll_offset > 0 then
                term:render()
                disp.flush()
            elseif term:isFullDirty() then
                term:render()
                disp.flush()
            else
                local first = term:getDirtyRange()
                if first >= 0 then
                    term:renderDirty()
                    disp.flush()
                end
            end
        elseif current_demo == DEMO_FONT then
            if nav & gp.PAD_LEFT ~= 0 then
                term:setFont("scientifica")
                term:write("\rFont: scientifica    ")
                term:render()
                disp.flush()
            elseif nav & gp.PAD_RIGHT ~= 0 then
                term:setFont("scientifica_bold")
                term:write("\rFont: scientifica_bold")
                term:render()
                disp.flush()
            elseif nav & gp.PAD_A ~= 0 then
                current_demo = DEMO_ANIMATION
                run_demo(term, current_demo)
                term:write("\nPress " .. pad_key(gp.PAD_A) .. " for next demo...")
                term:render()
                disp.flush()
            end
        end

        pc.sys.sleep(16)  -- nothing above draws unless it is due: don't spin
    end
end

main()
