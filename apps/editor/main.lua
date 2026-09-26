-- Text Editor for PicoDeck (Terminal SDK version)
-- Nano-like text editor with full editing capabilities

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local fs    = pc.fs
local sys   = pc.sys
local ui    = pc.ui

-- Terminal setup (53 cols x 26 rows, minus 2 rows for status/footer = 24 text rows)
local TERM_COLS = 53
local TERM_ROWS = 24
local TEXT_ROWS = 24

-- Colors for terminal
local TEXT_FG          = disp.WHITE
local TEXT_BG          = disp.BLACK
local LINE_NUM_FG      = disp.rgb(150, 150, 150)
local LINE_NUM_BG      = disp.rgb(40, 40, 40)
local SCROLLBAR_BG     = disp.rgb(80, 80, 80)
local SCROLLBAR_THUMB  = disp.rgb(150, 150, 150)

-- Create terminal with scrollback
local term = pc.terminal.new(TERM_COLS, TERM_ROWS, 5000)

-- Font state (only scientifica fonts available in terminal)
local current_font = 0
local FONT_NAMES = { [0]="scientifica", [1]="scientifica_bold" }

-- ── Large-file pager constants ────────────────────────────────────────────────
local LARGE_FILE_THRESHOLD = 32768  -- 32 KB
local INDEX_STRIDE         = 64     -- record a byte offset every N lines

-- ── State ─────────────────────────────────────────────────────────────────────
local lines        = {""}   -- text buffer (array of strings, 1-indexed)
local cursor_x     = 1      -- column (1-indexed)
local cursor_y     = 1      -- line   (1-indexed)
local scroll_y     = 0      -- top visible line (0-indexed)
local scroll_seg   = 1      -- first visible wrap segment of the top line (1-based; >1 only with word wrap)
local scroll_x     = 0      -- leftmost visible column (0-indexed)
local filename     = nil    -- current file path (nil = untitled)
local modified     = false  -- dirty flag
local trailing_newline = true  -- file ends with '\n' (preserved on save; POSIX default for new files)
local message      = ""
local message_time = 0

-- Editor options
local word_wrap       = false
local show_line_numbers = false
local show_help       = false

-- Key repeat
local repeat_timer = 0
local repeat_delay = 15
local repeat_rate  = 3
local last_key     = 0

-- ── Large-file pager state ────────────────────────────────────────────────────
local large_file        = false
local large_total_lines = 0
local line_index        = {}
local view_cache        = {}
local view_cache_scroll = -1

-- ── Unicode Sanitization ──────────────────────────────────────────────────────
local function sanitize_text(s)
    if not s then return nil end
    s = s:gsub("\226\128\152", "'")  -- smart quote
    s = s:gsub("\226\128\153", "'")  -- smart quote
    s = s:gsub("\226\128\156", '"')  -- smart quote
    s = s:gsub("\226\128\157", '"')  -- smart quote
    s = s:gsub("\226\128\147", "-")  -- en dash
    s = s:gsub("\226\128\148", "-")  -- em dash
    s = s:gsub("\194\160", " ")      -- non-breaking space
    s = s:gsub("\226\128\166", "...") -- ellipsis
    s = s:gsub("\226\128\168", " ")  -- line separator
    s = s:gsub("\226\128\169", " ")  -- paragraph separator
    return s
end

-- ── Large-file helpers ────────────────────────────────────────────────────────
local function total_lines()
    return large_file and large_total_lines or #lines
end

local function get_line(idx)
    if large_file then
        return view_cache[idx - scroll_y] or ""
    end
    return lines[idx] or ""
end

local function update_view_cache()
    if not large_file then return end
    if view_cache_scroll == scroll_y then return end

    local top_line = scroll_y + 1
    local base_k = math.floor((top_line - 1) / INDEX_STRIDE) + 1
    if base_k > #line_index then base_k = #line_index end

    local base_line   = (base_k - 1) * INDEX_STRIDE + 1
    local base_offset = line_index[base_k] or 0
    local skip        = top_line - base_line

    local handle = fs.open(filename, "r")
    if not handle then return end
    fs.seek(handle, base_offset)

    local new_cache   = {}
    local parts       = {}
    local lines_skipped = 0
    local collecting  = (skip == 0)

    while #new_cache < TEXT_ROWS do
        local chunk = fs.read(handle, 512)
        if not chunk then
            if collecting then
                new_cache[#new_cache + 1] = sanitize_text(table.concat(parts))
            end
            break
        end

        local pos = 1
        while pos <= #chunk do
            local nl = chunk:find('\n', pos, true)
            if collecting then
                if nl then
                    parts[#parts + 1] = chunk:sub(pos, nl - 1)
                    new_cache[#new_cache + 1] = sanitize_text(table.concat(parts))
                    parts = {}
                    if #new_cache >= TEXT_ROWS then break end
                    pos = nl + 1
                else
                    parts[#parts + 1] = chunk:sub(pos)
                    pos = #chunk + 1
                end
            else
                if nl then
                    lines_skipped = lines_skipped + 1
                    if lines_skipped >= skip then collecting = true end
                    pos = nl + 1
                else
                    pos = #chunk + 1
                end
            end
        end

        if #new_cache >= TEXT_ROWS then break end
    end

    fs.close(handle)
    view_cache       = new_cache
    view_cache_scroll = scroll_y
end

-- ── Word Jumping ───────────────────────────────────────────────────────────────
local function next_word_boundary(line, col)
    local line_len = #line
    if col > line_len then return col end
    
    -- Skip current word (non-whitespace)
    while col <= line_len and line:sub(col, col):match("%S") do
        col = col + 1
    end
    -- Skip whitespace
    while col <= line_len and line:sub(col, col):match("%s") do
        col = col + 1
    end
    return col
end

local function prev_word_boundary(line, col)
    if col <= 1 then return 1 end
    col = col - 1
    
    -- Skip whitespace
    while col > 1 and line:sub(col, col):match("%s") do
        col = col - 1
    end
    -- Skip current word (non-whitespace)
    while col > 1 and line:sub(col, col):match("%S") do
        col = col - 1
    end
    return col
end

-- ── Word wrap layout (mirrors src/os/text_wrap.c) ─────────────────────────────
-- Word wrap is done entirely editor-side: the terminal's own word wrap must
-- stay OFF. Writing 24 full-length logical lines with terminal wrap on
-- overflows the 24-row cell buffer (putChar auto-wraps long lines onto extra
-- rows and scrolls the top of the buffer into scrollback).

local WRAP_BREAK = { [" "]=true, ["\t"]=true, ["-"]=true, [","]=true, ["."]=true,
                     [";"]=true, [":"]=true, ["!"]=true, ["?"]=true, [")"]=true,
                     ["]"]=true, ["}"]=true }

-- Returns array of 0-based char offsets where each visual row of `line` starts.
local function wrap_segments(line, max_cols)
    local len = #line
    if len <= max_cols then return { 0 } end
    local segs = {}
    local pos = 0
    while pos < len do
        segs[#segs + 1] = pos
        local remaining = len - pos
        if remaining <= max_cols then break end
        -- Prefer breaking after space/punctuation within the last 20 columns
        local brk = max_cols
        local search_end = max_cols - 20
        if search_end < 1 then search_end = 1 end
        for i = max_cols, search_end, -1 do
            if WRAP_BREAK[line:sub(pos + i, pos + i)] then brk = i; break end
        end
        pos = pos + brk
    end
    return segs
end

local function line_seg_starts(idx, content_cols)
    if not word_wrap then return { 0 } end
    return wrap_segments(get_line(idx), content_cols)
end

-- Returns the 1-based segment index and 0-based column of the cursor within
-- its line's segments.
local function cursor_segment(segs, line_len, max_cols)
    local c = cursor_x - 1  -- 0-based; may equal line_len (end of line)
    for s = 1, #segs do
        local seg_end = (s < #segs) and segs[s + 1] or line_len
        if c < seg_end or s == #segs then
            local col = c - segs[s]
            -- Cursor exactly past the end of a full segment: keep it on the
            -- last cell rather than spilling onto a phantom row.
            if col >= max_cols then col = max_cols - 1 end
            return s, col
        end
    end
    return 1, 0
end

-- Keep the cursor inside the window, scrolling by visual rows.
-- Maintains the (scroll_y, scroll_seg) anchor. Word-wrap mode only.
local function scroll_to_cursor_wrapped(content_cols)
    local cline = get_line(cursor_y)
    local csegs = wrap_segments(cline, content_cols)
    local cseg = cursor_segment(csegs, #cline, content_cols)

    -- Clamp anchor to a valid position
    if scroll_y > total_lines() - 1 then scroll_y = total_lines() - 1 end
    if scroll_y < 0 then scroll_y = 0 end
    local asegs = line_seg_starts(scroll_y + 1, content_cols)
    if scroll_seg > #asegs then scroll_seg = #asegs end
    if scroll_seg < 1 then scroll_seg = 1 end

    -- Cursor above the anchor: snap the anchor up to the cursor
    if (cursor_y - 1 < scroll_y) or
       (cursor_y - 1 == scroll_y and cseg < scroll_seg) then
        scroll_y = cursor_y - 1
        scroll_seg = cseg
        if large_file then update_view_cache() end
        return
    end

    -- Count visual rows from the anchor to the cursor (inclusive)
    local rows = 0
    for l = scroll_y + 1, cursor_y do
        local from_seg = (l == scroll_y + 1) and scroll_seg or 1
        local to_seg = (l == cursor_y) and cseg or #line_seg_starts(l, content_cols)
        rows = rows + (to_seg - from_seg + 1)
    end

    -- Advance the anchor one visual row at a time until the cursor fits
    while rows > TEXT_ROWS do
        local nsegs = #line_seg_starts(scroll_y + 1, content_cols)
        if scroll_seg < nsegs then
            scroll_seg = scroll_seg + 1
        else
            scroll_y = scroll_y + 1
            scroll_seg = 1
            if large_file then update_view_cache() end
        end
        rows = rows - 1
    end
end

-- ── File operations ───────────────────────────────────────────────────────────
local function reset_large_file_state()
    large_file        = false
    large_total_lines = 0
    line_index        = {}
    view_cache        = {}
    view_cache_scroll = -1
end

local function new_buffer()
    reset_large_file_state()
    lines = {""}
    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_seg = 1; scroll_x = 0
    filename = nil; modified = false
    trailing_newline = true
    message = "[New File]  Ctrl+S to save"
    message_time = sys.getTimeMs()
end

local function load_large_file(path)
    term:clear()
    term:setCursor(0, TERM_ROWS // 2)
    term:write("Scanning: " .. path)
    term:render()
    disp.flush()

    line_index = {0}

    local handle = fs.open(path, "r")
    if not handle then
        message      = "Error: Cannot open " .. path
        message_time = sys.getTimeMs()
        return false
    end

    local line_count  = 0
    local byte_offset = 0

    while true do
        local chunk = fs.read(handle, 512)
        if not chunk then break end
        sys.sleep(0)

        local pos = 1
        while pos <= #chunk do
            local nl = chunk:find('\n', pos, true)
            if not nl then break end
            line_count = line_count + 1
            if line_count % INDEX_STRIDE == 0 then
                line_index[#line_index + 1] = byte_offset + nl
            end
            pos = nl + 1
        end
        byte_offset = byte_offset + #chunk
    end

    fs.close(handle)

    large_total_lines = line_count + 1
    large_file        = true
    lines             = {""}
    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_seg = 1; scroll_x = 0
    filename = path
    modified = false
    view_cache        = {}
    view_cache_scroll = -1

    message      = string.format("Read-only  %d lines", large_total_lines)
    message_time = sys.getTimeMs()
    return true
end

local function load_file(path)
    reset_large_file_state()

    if not fs.exists(path) then
        lines = {""}
        cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_seg = 1; scroll_x = 0
        filename = path
        modified = false
        message = "[New File]"
        message_time = sys.getTimeMs()
        return true
    end

    local size = fs.size(path)
    if size and size > LARGE_FILE_THRESHOLD then
        return load_large_file(path)
    end

    local handle = fs.open(path, "r")
    if not handle then
        message = "Error: Cannot open file"
        message_time = sys.getTimeMs()
        return false
    end

    lines = {}
    local pending = ""  -- partial line carried across chunk boundaries
    while true do
        local chunk = fs.read(handle, 512)
        if not chunk or #chunk == 0 then break end
        sys.sleep(0)

        chunk = pending .. chunk
        local start = 1
        while true do
            local nl = chunk:find('\n', start, true)
            if not nl then break end
            lines[#lines + 1] = sanitize_text(chunk:sub(start, nl - 1))
            start = nl + 1
        end
        pending = chunk:sub(start)
    end

    fs.close(handle)

    if pending ~= "" then
        lines[#lines + 1] = sanitize_text(pending)
        trailing_newline = false
    else
        trailing_newline = #lines > 0
    end
    if #lines == 0 then lines = {""} end

    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_seg = 1; scroll_x = 0
    filename = path
    modified = false
    message = "Loaded " .. path
    message_time = sys.getTimeMs()
    return true
end

local function save_file()
    if large_file then
        message = "Read-only: cannot save large file"
        message_time = sys.getTimeMs()
        return false
    end

    if not filename then
        local name = ui.textInputSimple("Save As (filename):", "")
        if not name or name == "" then
            message = "Save cancelled."
            message_time = sys.getTimeMs()
            return false
        end
        filename = fs.appPath(name)
    end

    local handle = fs.open(filename, "w")
    if not handle then
        message = "Error: Cannot write " .. filename
        message_time = sys.getTimeMs()
        return false
    end

    local empty_buffer = (#lines == 1 and lines[1] == "")
    for i = 1, #lines do
        fs.write(handle, lines[i])
        if i < #lines or (trailing_newline and not empty_buffer) then
            fs.write(handle, '\n')
        end
    end

    fs.close(handle)
    modified = false
    message = "Saved " .. filename
    message_time = sys.getTimeMs()
    return true
end

-- ── Input helpers ─────────────────────────────────────────────────────────────
local function clamp_cursor()
    local tl = total_lines()
    if cursor_y < 1  then cursor_y = 1  end
    if cursor_y > tl then cursor_y = tl end

    local line_len = #get_line(cursor_y)
    if cursor_x < 1            then cursor_x = 1 end
    if cursor_x > line_len + 1 then cursor_x = line_len + 1 end

    local content_cols = term:getContentCols()

    if word_wrap then
        scroll_x = 0
        scroll_to_cursor_wrapped(content_cols)
    else
        scroll_seg = 1

        -- Vertical: line-based window
        if cursor_y - 1 < scroll_y then
            scroll_y = cursor_y - 1
        end
        if cursor_y - 1 >= scroll_y + TEXT_ROWS then
            scroll_y = cursor_y - TEXT_ROWS
        end
        if scroll_y < 0 then scroll_y = 0 end

        -- Horizontal scrolling
        if cursor_x - 1 >= scroll_x + content_cols then
            scroll_x = cursor_x - content_cols
        end
        if cursor_x - 1 < scroll_x then
            scroll_x = cursor_x - 1
        end
    end
end

local function move_cursor(dx, dy)
    if dy ~= 0 then
        cursor_y = cursor_y + dy
        clamp_cursor()
    elseif dx ~= 0 then
        cursor_x = cursor_x + dx
        
        local current_len
        if large_file then
            local ln = get_line(cursor_y)
            current_len = ln and #ln or 0
        else
            current_len = #lines[cursor_y]
        end
        
        local total = total_lines()
        
        if cursor_x < 1 and cursor_y > 1 then
            cursor_y = cursor_y - 1
            if large_file then
                update_view_cache()
                local ln = get_line(cursor_y)
                cursor_x = (ln and #ln or 0) + 1
            else
                cursor_x = #lines[cursor_y] + 1
            end
        elseif cursor_x > current_len + 1 and cursor_y < total then
            cursor_y = cursor_y + 1
            cursor_x = 1
        end
        clamp_cursor()
    end
end

local function insert_char(ch)
    local line   = lines[cursor_y]
    local before = line:sub(1, cursor_x - 1)
    local after  = line:sub(cursor_x)
    lines[cursor_y] = before .. ch .. after
    cursor_x = cursor_x + 1
    modified = true
end

local function delete_char()
    local line = lines[cursor_y]
    if cursor_x <= #line then
        lines[cursor_y] = line:sub(1, cursor_x - 1) .. line:sub(cursor_x + 1)
        modified = true
    elseif cursor_y < #lines then
        lines[cursor_y] = line .. lines[cursor_y + 1]
        table.remove(lines, cursor_y + 1)
        modified = true
    end
end

local function backspace()
    if cursor_x > 1 then
        local line = lines[cursor_y]
        lines[cursor_y] = line:sub(1, cursor_x - 2) .. line:sub(cursor_x)
        cursor_x = cursor_x - 1
        modified = true
    elseif cursor_y > 1 then
        local prev_len = #lines[cursor_y - 1]
        lines[cursor_y - 1] = lines[cursor_y - 1] .. lines[cursor_y]
        table.remove(lines, cursor_y)
        cursor_y = cursor_y - 1
        cursor_x = prev_len + 1
        modified = true
        clamp_cursor()
    end
end

local function insert_newline()
    local line   = lines[cursor_y]
    local before = line:sub(1, cursor_x - 1)
    local after  = line:sub(cursor_x)
    lines[cursor_y] = before
    table.insert(lines, cursor_y + 1, after)
    cursor_y = cursor_y + 1
    cursor_x = 1
    modified = true
    clamp_cursor()
end

-- ── Rendering ─────────────────────────────────────────────────────────────────
local function redraw_screen()
    term:clear()

    local tl = total_lines()
    local content_cols = term:getContentCols()

    if show_line_numbers then
        term:setLineNumberStart(scroll_y + 1)
    end

    if word_wrap then
        -- Editor-side word wrap: write exactly one wrap segment per terminal
        -- row so the 24-row cell buffer can never overflow. A row that
        -- continues on the next row is padded to the full terminal width so
        -- the next write auto-wraps via putChar, which marks the terminal's
        -- continuation flag (keeps the line-number gutter blank on
        -- continuation rows).
        local row = 0
        local l = scroll_y + 1
        local seg_i = scroll_seg
        local cursor_row, cursor_col = nil, 0

        while row < TEXT_ROWS and l <= tl do
            local line = get_line(l)
            local segs = wrap_segments(line, content_cols)
            if seg_i > #segs then seg_i = #segs end

            local seg_start = segs[seg_i]
            local seg_end = (seg_i < #segs) and segs[seg_i + 1] or #line
            local text = line:sub(seg_start + 1, seg_end)
            term:write(text)

            if l == cursor_y then
                local cseg, ccol = cursor_segment(segs, #line, content_cols)
                if cseg == seg_i then
                    cursor_row, cursor_col = row, ccol
                end
            end

            local more_segs = seg_i < #segs
            if row < TEXT_ROWS - 1 then
                if more_segs then
                    term:write(string.rep(" ", TERM_COLS - #text))
                else
                    term:write("\n")
                end
            end

            if more_segs then
                seg_i = seg_i + 1
            else
                l = l + 1
                seg_i = 1
            end
            row = row + 1
        end

        term:setCursor(cursor_col, cursor_row or 0)
    else
        -- Horizontal window, one logical line per row
        for i = 1, TEXT_ROWS do
            local line_idx = scroll_y + i
            if line_idx <= tl then
                local line = get_line(line_idx)
                term:write(line:sub(scroll_x + 1, scroll_x + content_cols))
            end
            if i < TEXT_ROWS then
                term:write("\n")
            end
        end
        term:setCursor(cursor_x - 1 - scroll_x, cursor_y - scroll_y - 1)
    end

    -- Update scrollbar position
    if show_line_numbers or tl > TEXT_ROWS then
        term:setScrollInfo(tl, scroll_y)
    end

    -- Render to display
    term:render()
end

local function draw_status_bar()
    local fn = filename or "[Untitled]"
    local suffix
    if large_file then
        suffix = " [R/O]"
    elseif modified then
        suffix = " [+]"
    else
        suffix = ""
    end
    ui.drawHeader(fn .. suffix)
end

local function draw_command_bar()
    local pos = string.format("L%d C%d", cursor_y, cursor_x)

    if message ~= "" and sys.getTimeMs() - message_time < 3000 then
        ui.drawFooter(message, pos)
    elseif large_file then
        ui.drawFooter("Read-only  F1:Help", pos)
    else
        ui.drawFooter("F1: Help", pos)
    end
end

local function draw_help_overlay()
    -- Use terminal to draw help text
    local help_text = [[
=== EDITOR HELP ===

File Operations:
  Ctrl+S  Save file
  Ctrl+Q  Quit
  Ctrl+O  Open file
  Ctrl+N  New file

Editing:
  Ctrl+K  Delete current line
  Arrows  Move cursor
  Ctrl+Left/Right  Word jump
  Ctrl+Up/Dn Page Up/Dn
  Enter   New line
  Bksp    Delete char

View Options:
  F2      Toggle word wrap
  F3      Toggle line numbers
  F4      Cycle font

Press F1 or Esc to close
]]
    
    disp.fillRect(10, 40, 300, 240, disp.rgb(20, 20, 40))
    disp.drawRect(10, 40, 300, 240, disp.YELLOW)
    
    local y = 50
    for line in help_text:gmatch("([^\n]*)\n?") do
        if line ~= "" then
            disp.drawText(20, y, line, disp.WHITE, disp.rgb(20, 20, 40))
            y = y + 12
        end
    end
end

-- ── Main loop ─────────────────────────────────────────────────────────────────
local function main()
    -- Load saved font preference
    current_font = tonumber(pc.sysconfig.get("editor_font")) or 0
    if current_font < 0 or current_font > 1 then current_font = 0 end
    term:setFont(FONT_NAMES[current_font])
    
    -- Setup terminal features
    term:setRenderBounds(21, 302)  -- below ui.drawHeader's 21 rows, above the footer
    term:setLineNumbers(false)
    term:setLineNumberColors(LINE_NUM_FG, LINE_NUM_BG)
    term:setScrollbar(false)
    term:setScrollbarColors(SCROLLBAR_BG, SCROLLBAR_THUMB)
    term:setCursorVisible(true)
    term:setCursorBlink(true)
    -- Word wrap is implemented editor-side (see wrap_segments); the terminal's
    -- own wrap must stay off or long lines overflow the cell buffer.
    term:setWordWrap(false)

    -- On startup, browse for a file
    local startup_file = fs.browse()
    if startup_file then
        load_file(startup_file)
    else
        new_buffer()
    end

    -- System menu items
    sys.addMenuItem("Save & Quit", function()
        if save_file() then sys.exit() end
    end)
    sys.addMenuItem("Quit without saving", function()
        sys.exit()
    end)

    while true do
        update_view_cache()

        input.update()
        local pressed = input.getButtonsPressed()
        local held    = input.getButtons()
        local char    = input.getChar()

        -- Key repeat
        if held ~= 0 and held == last_key then
            repeat_timer = repeat_timer + 1
        else
            repeat_timer = 0
            last_key = held
        end
        local do_repeat = repeat_timer > repeat_delay and
                          (repeat_timer - repeat_delay) % repeat_rate == 0

        -- F1 = Toggle help overlay
        if pressed & input.BTN_F1 ~= 0 then
            show_help = not show_help
        end

        -- F2 = Toggle word wrap (editor-side layout)
        if pressed & input.BTN_F2 ~= 0 then
            word_wrap = not word_wrap
            scroll_x = 0
            scroll_seg = 1
            message = "Word wrap " .. (word_wrap and "enabled" or "disabled")
            message_time = sys.getTimeMs()
        end

        -- F3 = Toggle line numbers
        if pressed & input.BTN_F3 ~= 0 then
            show_line_numbers = not show_line_numbers
            term:setLineNumbers(show_line_numbers)
            term:markAllDirty()
            message = "Line numbers " .. (show_line_numbers and "enabled" or "disabled")
            message_time = sys.getTimeMs()
        end

        -- F4 = Cycle font
        if pressed & input.BTN_F4 ~= 0 then
            current_font = (current_font + 1) % 2
            term:setFont(FONT_NAMES[current_font])
            term:markAllDirty()
            pc.sysconfig.set("editor_font", tostring(current_font))
            pc.sysconfig.save()
            message = "Font: " .. FONT_NAMES[current_font]
            message_time = sys.getTimeMs()
        end

        -- Esc = quit or close help
        if pressed & input.BTN_ESC ~= 0 then
            if show_help then
                show_help = false
            elseif modified then
                local choice = ui.confirm("Save changes before exiting?")
                if choice then
                    if save_file() then
                        return
                    end
                else
                    return
                end
            else
                return
            end
        end

        -- Ctrl+<letter> shortcuts
        if held & input.BTN_CTRL ~= 0 and char then
            local lower = char:lower()
            -- Physical Ctrl+letter arrives as a control byte (0x01-0x1A);
            -- normalize it back to the letter so both forms work.
            local b = string.byte(char)
            if b >= 1 and b <= 26 then
                lower = string.char(b + 96)
            end

            -- Ctrl+S = Save
            if lower == 's' then
                save_file()

            -- Ctrl+Q = Quit
            elseif lower == 'q' then
                if modified then
                    local choice = ui.confirm("Save changes before exiting?")
                    if choice then
                        if save_file() then
                            return
                        end
                    else
                        return
                    end
                else
                    return
                end

            -- Ctrl+O = Open (with confirmation dialog)
            elseif lower == 'o' then
                if modified then
                    local choice = ui.confirm("Save changes before opening?")
                    if choice then
                        if save_file() then
                            local path = fs.browse()
                            if path then load_file(path) end
                        end
                    else
                        -- User chose to discard changes
                        local path = fs.browse()
                        if path then load_file(path) end
                    end
                else
                    local path = fs.browse()
                    if path then load_file(path) end
                end

            -- Ctrl+N = New empty buffer
            elseif lower == 'n' then
                if modified and not large_file then
                    local choice = ui.confirm("Save changes before creating new file?")
                    if choice then
                        if save_file() then new_buffer() end
                    else
                        new_buffer()
                    end
                else
                    new_buffer()
                end

            -- Ctrl+K = Delete current line
            elseif lower == 'k' and not large_file then
                if cursor_y <= #lines then
                    table.remove(lines, cursor_y)
                    if #lines == 0 then lines = {""} end
                    if cursor_y > #lines then cursor_y = #lines end
                    cursor_x = 1
                    modified = true
                    clamp_cursor()
                end
            end
        end

        -- Navigation with word jumping
        if (pressed & input.BTN_UP    ~= 0) or (do_repeat and held & input.BTN_UP    ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                move_cursor(0, -TEXT_ROWS)
            else
                move_cursor(0, -1)
            end
        end
        if (pressed & input.BTN_DOWN  ~= 0) or (do_repeat and held & input.BTN_DOWN  ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                move_cursor(0, TEXT_ROWS)
            else
                -- If on last line, go to end of line instead of moving down
                local total = total_lines()
                if cursor_y == total then
                    local line = get_line(cursor_y)
                    cursor_x = (line and #line or 0) + 1
                else
                    move_cursor(0, 1)
                end
            end
        end
        
        -- Left arrow with word jumping
        if (pressed & input.BTN_LEFT  ~= 0) or (do_repeat and held & input.BTN_LEFT  ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                -- Word jump left
                local line = get_line(cursor_y)
                cursor_x = prev_word_boundary(line, cursor_x)
                clamp_cursor()
            else
                move_cursor(-1, 0)
            end
        end
        
        -- Right arrow with word jumping
        if (pressed & input.BTN_RIGHT ~= 0) or (do_repeat and held & input.BTN_RIGHT ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                -- Word jump right
                local line = get_line(cursor_y)
                cursor_x = next_word_boundary(line, cursor_x)
                clamp_cursor()
            else
                move_cursor(1, 0)
            end
        end

        -- Fn = Home (go to start of line)
        if pressed & input.BTN_FN ~= 0 then
            cursor_x = 1
        end

        -- Tab = Page Up
        if pressed & input.BTN_TAB ~= 0 then move_cursor(0, -TEXT_ROWS) end

        -- Editing (small-file mode only)
        if not large_file then
            if pressed & input.BTN_ENTER ~= 0 then
                insert_newline()
            end

            if (pressed & input.BTN_BACKSPACE ~= 0) or (do_repeat and held & input.BTN_BACKSPACE ~= 0) then
                backspace()
            end

            if pressed & input.BTN_DEL ~= 0 then
                delete_char()
            end

            -- Insert printable characters
            if char and char ~= "" and char ~= "\n" and (held & input.BTN_CTRL == 0) then
                local byte = string.byte(char)
                if byte >= 32 and byte <= 126 then
                    insert_char(char)
                end
            end
        end

        clamp_cursor()

        -- Refresh the pager view cache after any scroll change this frame,
        -- so drawing never reads a stale window
        update_view_cache()

        -- Draw
        draw_status_bar()
        redraw_screen()
        draw_command_bar()

        -- Draw help overlay on top if active
        if show_help then
            draw_help_overlay()
        end

        disp.flush()
    end
end

-- ── Entry point ───────────────────────────────────────────────────────────────

main()
