-- PicoDeck Widget Toolkit
-- Composable, non-blocking UI widgets for Lua apps, on top of picocalc.ui.
-- Load with: local W = picocalc.sys.loadlib("widgets")
-- Reference: docs/Library-Widgets.md

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local ui    = pc.ui
local sys   = pc.sys

local W = {}

local function display_draw_rect(x, y, w, h, c) disp.drawRect(x, y, w, h, c) end
local function display_fill_rect(x, y, w, h, c) disp.fillRect(x, y, w, h, c) end

-- ── Theme ────────────────────────────────────────────────────────────────────

W.theme = {
    bg         = disp.rgb(20, 28, 50),
    bg_input   = disp.rgb(5, 10, 20),
    border     = disp.rgb(80, 100, 150),
    focus      = disp.rgb(100, 140, 220),
    text       = disp.WHITE,
    text_dim   = disp.GRAY,
    accent     = disp.rgb(40, 80, 160),
    success    = disp.GREEN,
    error      = disp.RED,
    font_w     = 6,
    font_h     = 8,
    row_h      = 13,
}

-- ── Helpers ──────────────────────────────────────────────────────────────────

-- Find word boundary for ctrl+arrow navigation
local function find_word_boundary(text, pos, direction)
    local len = #text
    if direction > 0 then
        -- Forward: skip current word chars, then skip spaces
        local p = pos
        while p < len and text:sub(p + 1, p + 1):match("%S") do p = p + 1 end
        while p < len and text:sub(p + 1, p + 1):match("%s") do p = p + 1 end
        return p
    else
        -- Backward: skip spaces, then skip word chars
        local p = pos
        while p > 0 and text:sub(p, p):match("%s") do p = p - 1 end
        while p > 0 and text:sub(p, p):match("%S") do p = p - 1 end
        return p
    end
end

-- The scroll position that keeps row `index` (1-based) in a view of `visible`
-- rows, moving as little as it can. The rows wrap, so it can be any distance away.
local function scroll_to_show(scroll, index, visible)
    if index <= scroll then return index - 1 end
    if index > scroll + visible then return index - visible end
    return scroll
end

-- ── Label ────────────────────────────────────────────────────────────────────

local Label = {}
Label.__index = Label

function W.Label(opts)
    local self = setmetatable({
        x     = opts.x or 0,
        y     = opts.y or 0,
        w     = opts.w,
        text  = opts.text or "",
        color = opts.color or W.theme.text,
        align = opts.align or "left",
        focusable = false,
    }, Label)
    return self
end

function Label:setText(str) self.text = str end

function Label:draw()
    local tw = disp.textWidth(self.text)
    local x = self.x
    if self.w then
        if self.align == "center" then
            x = self.x + math.floor((self.w - tw) / 2)
        elseif self.align == "right" then
            x = self.x + self.w - tw
        end
    end
    disp.drawText(x, self.y, self.text, self.color, false)
end

-- ── Button ───────────────────────────────────────────────────────────────────

local Button = {}
Button.__index = Button

function W.Button(opts)
    local self = setmetatable({
        x       = opts.x or 0,
        y       = opts.y or 0,
        w       = opts.w or 80,
        label   = opts.label or "Button",
        onPress = opts.onPress,
        focused = false,
        pressed = false,
        focusable = true,
    }, Button)
    return self
end

function Button:setLabel(str) self.label = str end
function Button:setFocused(f) self.focused = f end

function Button:handleInput(btns, ch)
    if btns & input.BTN_ENTER ~= 0 then
        self.pressed = true
        if self.onPress then self.onPress() end
        return true
    end
    self.pressed = false
    return false
end

function Button:draw()
    ui.drawButton(self.x, self.y, self.w, self.label, self.focused, self.pressed)
    self.pressed = false  -- reset visual press state after draw
end

-- ── Divider ──────────────────────────────────────────────────────────────────

local Divider = {}
Divider.__index = Divider

function W.Divider(opts)
    return setmetatable({
        x     = opts.x or 0,
        y     = opts.y or 0,
        w     = opts.w or 300,
        color = opts.color or W.theme.border,
        focusable = false,
    }, Divider)
end

function Divider:draw()
    ui.drawDivider(self.x, self.y, self.w, self.color)
end

-- ── ProgressBar ──────────────────────────────────────────────────────────────

local ProgressBar = {}
ProgressBar.__index = ProgressBar

function W.ProgressBar(opts)
    return setmetatable({
        x        = opts.x or 0,
        y        = opts.y or 0,
        w        = opts.w or 200,
        h        = opts.h or 12,
        progress = opts.progress or 0,
        color    = opts.color or W.theme.success,
        focusable = false,
    }, ProgressBar)
end

function ProgressBar:setProgress(p) self.progress = p end

function ProgressBar:draw()
    ui.drawProgress(self.x, self.y, self.w, self.h, self.progress, self.color)
end

-- ── Checkbox ─────────────────────────────────────────────────────────────────

local Checkbox = {}
Checkbox.__index = Checkbox

function W.Checkbox(opts)
    return setmetatable({
        x        = opts.x or 0,
        y        = opts.y or 0,
        label    = opts.label or "",
        checked  = opts.checked or false,
        onChange = opts.onChange,
        focused  = false,
        focusable = true,
    }, Checkbox)
end

function Checkbox:isChecked() return self.checked end
function Checkbox:setChecked(v) self.checked = v end
function Checkbox:setFocused(f) self.focused = f end

function Checkbox:handleInput(btns, ch)
    if btns & input.BTN_ENTER ~= 0 then
        self.checked = not self.checked
        if self.onChange then self.onChange(self.checked) end
        return true
    end
    return false
end

function Checkbox:draw()
    ui.drawCheckbox(self.x, self.y, self.checked, self.focused)
    local label_x = self.x + 14  -- 10px box + 4px gap
    local color = self.focused and W.theme.text or W.theme.text_dim
    disp.drawText(label_x, self.y + 1, self.label, color, false)
end

-- ── RadioGroup ───────────────────────────────────────────────────────────────

local RadioGroup = {}
RadioGroup.__index = RadioGroup

function W.RadioGroup(opts)
    return setmetatable({
        x        = opts.x or 0,
        y        = opts.y or 0,
        options  = opts.options or {},
        selected = opts.selected or 1,
        spacing  = opts.spacing or W.theme.row_h,
        onChange  = opts.onChange,
        focused  = false,
        focusable = true,
        highlight = opts.selected or 1,  -- visual cursor within group
    }, RadioGroup)
end

function RadioGroup:getSelected() return self.selected, self.options[self.selected] end
function RadioGroup:setSelected(i) self.selected = i; self.highlight = i end
function RadioGroup:setFocused(f) self.focused = f end

function RadioGroup:handleInput(btns, ch)
    local n = #self.options
    if n == 0 then return false end

    if btns & input.BTN_UP ~= 0 then
        self.highlight = self.highlight - 1
        if self.highlight < 1 then self.highlight = n end
        return true
    elseif btns & input.BTN_DOWN ~= 0 then
        self.highlight = self.highlight + 1
        if self.highlight > n then self.highlight = 1 end
        return true
    elseif btns & input.BTN_ENTER ~= 0 then
        self.selected = self.highlight
        if self.onChange then self.onChange(self.selected, self.options[self.selected]) end
        return true
    end
    return false
end

function RadioGroup:draw()
    for i, opt in ipairs(self.options) do
        local ry = self.y + (i - 1) * self.spacing
        local is_sel = (i == self.selected)
        local is_hl  = self.focused and (i == self.highlight)
        ui.drawRadio(self.x, ry, is_sel, is_hl)
        local color = (self.focused and is_hl) and W.theme.text or W.theme.text_dim
        disp.drawText(self.x + 14, ry + 1, opt, color, false)
    end
end

-- ── TextField ────────────────────────────────────────────────────────────────

local TextField = {}
TextField.__index = TextField

function W.TextField(opts)
    return setmetatable({
        x           = opts.x or 0,
        y           = opts.y or 0,
        w           = opts.w or 200,
        text        = opts.text or "",
        placeholder = opts.placeholder or "",
        maxlen      = opts.maxlen or 256,
        cursor      = #(opts.text or ""),
        scroll      = 0,
        onChange     = opts.onChange,
        onSubmit    = opts.onSubmit,
        focused     = false,
        focusable   = true,
        blink_time  = 0,
        blink_on    = true,
    }, TextField)
end

function TextField:getText() return self.text end
function TextField:setCursor(p) self.cursor = p end
function TextField:setFocused(f) self.focused = f; self.blink_on = true; self.blink_time = sys.getTimeMs() end

function TextField:setText(str)
    self.text = str
    if self.cursor > #str then self.cursor = #str end
end

function TextField:_updateScroll()
    local max_vis = math.floor((self.w - 8) / W.theme.font_w)
    if self.cursor - self.scroll >= max_vis then
        self.scroll = self.cursor - max_vis + 1
    end
    if self.cursor < self.scroll then
        self.scroll = self.cursor
    end
    if self.scroll < 0 then self.scroll = 0 end
end

function TextField:_updateBlink()
    local now = sys.getTimeMs()
    if now - self.blink_time >= 500 then
        self.blink_on = not self.blink_on
        self.blink_time = now
    end
end

function TextField:handleInput(btns, ch)
    local consumed = false
    local t = self.text
    local c = self.cursor
    local ctrl = (btns & input.BTN_CTRL ~= 0)

    -- Arrow keys
    if btns & input.BTN_LEFT ~= 0 then
        if ctrl then
            self.cursor = find_word_boundary(t, c, -1)
        elseif c > 0 then
            self.cursor = c - 1
        end
        consumed = true
    elseif btns & input.BTN_RIGHT ~= 0 then
        if ctrl then
            self.cursor = find_word_boundary(t, c, 1)
        elseif c < #t then
            self.cursor = c + 1
        end
        consumed = true
    end

    -- Home/End (F1 = Home, F2 = End as convention, or use Ctrl+Left/Right)
    if btns & input.BTN_F1 ~= 0 and ctrl then
        self.cursor = 0; consumed = true
    elseif btns & input.BTN_F2 ~= 0 and ctrl then
        self.cursor = #t; consumed = true
    end

    -- Backspace
    if btns & input.BTN_BACKSPACE ~= 0 and c > 0 then
        self.text = t:sub(1, c - 1) .. t:sub(c + 1)
        self.cursor = c - 1
        consumed = true
        if self.onChange then self.onChange(self.text) end
    end

    -- Delete
    if btns & input.BTN_DEL ~= 0 and c < #t then
        self.text = t:sub(1, c) .. t:sub(c + 2)
        consumed = true
        if self.onChange then self.onChange(self.text) end
    end

    -- Enter
    if btns & input.BTN_ENTER ~= 0 then
        if self.onSubmit then self.onSubmit(self.text) end
        consumed = true
    end

    -- Character input
    if ch and ch ~= "" and #self.text < self.maxlen then
        local b = ch:byte()
        if b >= 0x20 and b < 0x7F then
            local c2 = self.cursor
            self.text = self.text:sub(1, c2) .. ch .. self.text:sub(c2 + 1)
            self.cursor = c2 + 1
            consumed = true
            if self.onChange then self.onChange(self.text) end
        end
    end

    if consumed then
        self.blink_on = true
        self.blink_time = sys.getTimeMs()
    end
    self:_updateScroll()
    return consumed
end

function TextField:draw()
    self:_updateBlink()
    local display_text = self.text
    local show_placeholder = (#self.text == 0 and not self.focused)
    if show_placeholder then
        display_text = self.placeholder
    end
    ui.drawTextField(self.x, self.y, self.w, display_text,
                     self.cursor, self.scroll, self.focused,
                     self.focused and self.blink_on)
    -- Draw placeholder in dim color over the field if needed
    if show_placeholder and #self.placeholder > 0 then
        local max_vis = math.floor((self.w - 8) / W.theme.font_w)
        local vis = self.placeholder:sub(1, max_vis)
        disp.drawText(self.x + 4, self.y + 3, vis, W.theme.text_dim, W.theme.bg_input)
    end
end

-- ── TextArea ─────────────────────────────────────────────────────────────────

local TextArea = {}
TextArea.__index = TextArea

function W.TextArea(opts)
    local self = setmetatable({
        x           = opts.x or 0,
        y           = opts.y or 0,
        w           = opts.w or 300,
        h           = opts.h or 100,
        text        = opts.text or "",
        placeholder = opts.placeholder or "",
        wordWrap    = (opts.wordWrap ~= false),  -- default true
        multiline   = (opts.multiline ~= false), -- default true
        maxlen      = opts.maxlen or 4096,
        cursor      = #(opts.text or ""),
        scroll_y    = 0,
        onChange     = opts.onChange,
        onSubmit    = opts.onSubmit,
        focused     = false,
        focusable   = true,
        blink_time  = 0,
        blink_on    = true,
        -- Cached wrap state
        _segments   = nil,
        _num_segs   = 0,
        _dirty      = true,
    }, TextArea)
    self:_rewrap()
    return self
end

function TextArea:getText() return self.text end
function TextArea:setFocused(f) self.focused = f; self.blink_on = true; self.blink_time = sys.getTimeMs() end

function TextArea:setText(str)
    self.text = str
    if self.cursor > #str then self.cursor = #str end
    self._dirty = true
end

function TextArea:_maxCols()
    return math.floor((self.w - 12) / W.theme.font_w)  -- 4px pad + 4px scrollbar margin
end

function TextArea:_visibleRows()
    return math.floor((self.h - 6) / W.theme.font_h)
end

function TextArea:_rewrap()
    local max_cols = self:_maxCols()
    if max_cols < 1 then max_cols = 1 end
    local text = self.text

    if #text == 0 then
        self._segments = {0}
        self._num_segs = 1
        self._dirty = false
        return
    end

    -- Split text on newlines, then optionally word-wrap each line
    self._segments = {}
    local line_start = 0  -- 0-based offset into full text

    -- Iterate over lines separated by \n
    -- We work with 0-based offsets matching the C convention
    local pos = 1
    while true do
        local nl = text:find("\n", pos, true)
        local line_end = nl and (nl - 1) or #text  -- end of this line's content
        local line = text:sub(pos, line_end)

        if self.wordWrap and #line > max_cols then
            -- Word-wrap this line; wrapText returns 0-based offsets within `line`
            local segs, n = ui.wrapText(line, max_cols)
            for i = 1, n do
                self._segments[#self._segments + 1] = line_start + segs[i]
            end
        else
            -- No wrap needed for this line
            self._segments[#self._segments + 1] = line_start
        end

        if not nl then break end
        -- Advance past the \n character
        line_start = nl  -- offset of char after \n (0-based: nl in 1-based = nl-1+1 = nl in 0-based)
        pos = nl + 1
        -- Handle trailing newline (empty last line)
        if pos > #text then
            self._segments[#self._segments + 1] = line_start
            break
        end
    end

    self._num_segs = #self._segments
    self._dirty = false
end

-- Convert linear cursor position to (visual_row, visual_col)
function TextArea:_cursorToRowCol()
    if self._dirty then self:_rewrap() end
    local segs = self._segments
    local n = self._num_segs
    for i = n, 1, -1 do
        if self.cursor >= segs[i] then
            return i - 1, self.cursor - segs[i]  -- 0-based row, 0-based col
        end
    end
    return 0, 0
end

-- Convert (visual_row, col) back to linear cursor position
function TextArea:_rowColToCursor(row, col)
    if self._dirty then self:_rewrap() end
    local segs = self._segments
    local n = self._num_segs
    local seg_idx = row + 1  -- 1-based
    if seg_idx < 1 then seg_idx = 1 end
    if seg_idx > n then seg_idx = n end
    local seg_start = segs[seg_idx]
    local seg_end
    if seg_idx < n then
        seg_end = segs[seg_idx + 1]
    else
        seg_end = #self.text
    end
    local max_col = seg_end - seg_start
    if col > max_col then col = max_col end
    if col < 0 then col = 0 end
    return seg_start + col
end

function TextArea:_updateScroll()
    local row, _ = self:_cursorToRowCol()
    local vis = self:_visibleRows()
    if row < self.scroll_y then
        self.scroll_y = row
    end
    if row >= self.scroll_y + vis then
        self.scroll_y = row - vis + 1
    end
    if self.scroll_y < 0 then self.scroll_y = 0 end
end

function TextArea:handleInput(btns, ch)
    local consumed = false
    local t = self.text
    local c = self.cursor
    local ctrl = (btns & input.BTN_CTRL ~= 0)

    -- Up/Down
    if btns & input.BTN_UP ~= 0 then
        if ctrl then
            -- Page up
            self.scroll_y = self.scroll_y - self:_visibleRows()
            if self.scroll_y < 0 then self.scroll_y = 0 end
        else
            local row, col = self:_cursorToRowCol()
            if row > 0 then
                self.cursor = self:_rowColToCursor(row - 1, col)
            end
        end
        consumed = true
    elseif btns & input.BTN_DOWN ~= 0 then
        if ctrl then
            -- Page down
            local max_scroll = self._num_segs - self:_visibleRows()
            if max_scroll < 0 then max_scroll = 0 end
            self.scroll_y = self.scroll_y + self:_visibleRows()
            if self.scroll_y > max_scroll then self.scroll_y = max_scroll end
        else
            local row, col = self:_cursorToRowCol()
            if row < self._num_segs - 1 then
                self.cursor = self:_rowColToCursor(row + 1, col)
            end
        end
        consumed = true
    end

    -- Left/Right
    if btns & input.BTN_LEFT ~= 0 then
        if ctrl then
            self.cursor = find_word_boundary(t, c, -1)
        elseif c > 0 then
            self.cursor = c - 1
        end
        consumed = true
    elseif btns & input.BTN_RIGHT ~= 0 then
        if ctrl then
            self.cursor = find_word_boundary(t, c, 1)
        elseif c < #t then
            self.cursor = c + 1
        end
        consumed = true
    end

    -- Backspace
    if btns & input.BTN_BACKSPACE ~= 0 and c > 0 then
        self.text = t:sub(1, c - 1) .. t:sub(c + 1)
        self.cursor = c - 1
        self._dirty = true
        consumed = true
        if self.onChange then self.onChange(self.text) end
    end

    -- Delete
    if btns & input.BTN_DEL ~= 0 and c < #t then
        self.text = t:sub(1, c) .. t:sub(c + 2)
        self._dirty = true
        consumed = true
        if self.onChange then self.onChange(self.text) end
    end

    -- Enter
    if btns & input.BTN_ENTER ~= 0 then
        if self.multiline and #self.text < self.maxlen then
            self.text = self.text:sub(1, c) .. "\n" .. self.text:sub(c + 1)
            self.cursor = c + 1
            self._dirty = true
            consumed = true
            if self.onChange then self.onChange(self.text) end
        elseif self.onSubmit then
            self.onSubmit(self.text)
            consumed = true
        end
    end

    -- Character input
    if ch and ch ~= "" and #self.text < self.maxlen then
        local b = ch:byte()
        if b >= 0x20 and b < 0x7F then
            self.text = self.text:sub(1, c) .. ch .. self.text:sub(c + 1)
            self.cursor = c + 1
            self._dirty = true
            consumed = true
            if self.onChange then self.onChange(self.text) end
        end
    end

    if consumed then
        self.blink_on = true
        self.blink_time = sys.getTimeMs()
        if self._dirty then self:_rewrap() end
        self:_updateScroll()
    end
    return consumed
end

function TextArea:draw()
    -- Update blink
    local now = sys.getTimeMs()
    if now - self.blink_time >= 500 then
        self.blink_on = not self.blink_on
        self.blink_time = now
    end

    if self._dirty then self:_rewrap() end

    local crow, ccol = self:_cursorToRowCol()

    if #self.text == 0 and not self.focused and #self.placeholder > 0 then
        -- Draw empty with placeholder
        ui.drawTextArea(self.x, self.y, self.w, self.h,
                        self.placeholder, self._segments, self.scroll_y,
                        -1, -1, self.focused, false)
    else
        ui.drawTextArea(self.x, self.y, self.w, self.h,
                        self.text, self._segments, self.scroll_y,
                        crow, ccol, self.focused,
                        self.focused and self.blink_on)
    end
end

-- ── Select ───────────────────────────────────────────────────────────────────

local Select = {}
Select.__index = Select

function W.Select(opts)
    return setmetatable({
        x          = opts.x or 0,
        y          = opts.y or 0,
        w          = opts.w or 200,
        options    = opts.options or {},
        selected   = opts.selected or 1,
        maxVisible = opts.maxVisible or 6,
        onChange    = opts.onChange,
        focused    = false,
        focusable  = true,
        expanded   = false,
        filter     = "",
        filtered   = {},
        highlight  = 1,
        scroll     = 0,
    }, Select)
end

function Select:getSelected() return self.selected, self.options[self.selected] end
function Select:setSelected(i) self.selected = i end
function Select:setOptions(t) self.options = t; self.selected = 1 end
function Select:isExpanded() return self.expanded end
function Select:setFocused(f) self.focused = f; if not f then self.expanded = false end end

function Select:_updateFilter()
    self.filtered = {}
    local f = self.filter:lower()
    for i, opt in ipairs(self.options) do
        if f == "" or opt:lower():find(f, 1, true) then
            self.filtered[#self.filtered + 1] = {index = i, text = opt}
        end
    end
    self.highlight = 1
    self.scroll = 0
end

function Select:handleInput(btns, ch)
    if not self.expanded then
        if btns & input.BTN_ENTER ~= 0 then
            self.expanded = true
            self.filter = ""
            self:_updateFilter()
            return true
        end
        return false
    end

    -- Expanded mode
    if btns & input.BTN_ESC ~= 0 then
        self.expanded = false
        return true
    end

    if btns & input.BTN_UP ~= 0 then
        self.highlight = self.highlight - 1
        if self.highlight < 1 then self.highlight = #self.filtered end
        self.scroll = scroll_to_show(self.scroll, self.highlight, self.maxVisible)
        return true
    elseif btns & input.BTN_DOWN ~= 0 then
        self.highlight = self.highlight + 1
        if self.highlight > #self.filtered then self.highlight = 1 end
        self.scroll = scroll_to_show(self.scroll, self.highlight, self.maxVisible)
        return true
    end

    if btns & input.BTN_ENTER ~= 0 then
        if self.filtered[self.highlight] then
            self.selected = self.filtered[self.highlight].index
            if self.onChange then
                self.onChange(self.selected, self.options[self.selected])
            end
        end
        self.expanded = false
        return true
    end

    -- Backspace in filter
    if btns & input.BTN_BACKSPACE ~= 0 and #self.filter > 0 then
        self.filter = self.filter:sub(1, -2)
        self:_updateFilter()
        return true
    end

    -- Character input for filter
    if ch and ch ~= "" then
        local b = ch:byte()
        if b >= 0x20 and b < 0x7F then
            self.filter = self.filter .. ch
            self:_updateFilter()
            return true
        end
    end

    return false
end

function Select:draw()
    -- Collapsed view: show selected value
    local display_text = self.options[self.selected] or ""
    local border = self.focused and W.theme.focus or W.theme.border
    local h = W.theme.row_h

    display_draw_rect(self.x, self.y, self.w, h, border)
    display_fill_rect(self.x + 1, self.y + 1, self.w - 2, h - 2, W.theme.bg_input)

    -- Text (truncated if needed)
    local max_chars = math.floor((self.w - 20) / W.theme.font_w)  -- leave room for arrow
    local vis = display_text:sub(1, max_chars)
    disp.drawText(self.x + 4, self.y + 3, vis, W.theme.text, W.theme.bg_input)

    -- Down arrow indicator
    disp.drawText(self.x + self.w - 10, self.y + 3, "v", W.theme.text_dim, W.theme.bg_input)
end

function Select:drawDropdown()
    if not self.expanded then return end

    local dd_x = self.x
    local dd_y = self.y + W.theme.row_h + 1
    local dd_w = self.w
    local filter_h = W.theme.row_h
    local item_count = math.min(#self.filtered, self.maxVisible)
    local dd_h = filter_h + item_count * W.theme.row_h + 2  -- +2 for borders

    -- Flip upward if near bottom of screen
    if dd_y + dd_h > 310 then
        dd_y = self.y - dd_h - 1
    end

    -- Background and border
    display_draw_rect(dd_x, dd_y, dd_w, dd_h, W.theme.focus)
    display_fill_rect(dd_x + 1, dd_y + 1, dd_w - 2, dd_h - 2, W.theme.bg)

    -- Filter field
    local filter_text = self.filter
    if #filter_text == 0 then filter_text = "Type to filter..." end
    disp.drawText(dd_x + 4, dd_y + 3, filter_text:sub(1, math.floor((dd_w - 8) / W.theme.font_w)),
                  #self.filter > 0 and W.theme.text or W.theme.text_dim, W.theme.bg)
    ui.drawDivider(dd_x + 1, dd_y + filter_h, dd_w - 2, W.theme.border)

    -- Items
    for vi = 1, item_count do
        local idx = self.scroll + vi
        if idx <= #self.filtered then
            local item = self.filtered[idx]
            local iy = dd_y + filter_h + 1 + (vi - 1) * W.theme.row_h
            local is_hl = (idx == self.highlight)
            ui.drawListItem(dd_x + 1, iy, dd_w - 2, item.text, is_hl, true)
        end
    end
end

-- ── ListView ─────────────────────────────────────────────────────────────────

local ListView = {}
ListView.__index = ListView

function W.ListView(opts)
    return setmetatable({
        x          = opts.x or 0,
        y          = opts.y or 0,
        w          = opts.w or 300,
        h          = opts.h or 200,
        items      = opts.items or {},
        selected   = 1,
        scroll     = 0,
        onSelect   = opts.onSelect,
        onActivate = opts.onActivate,
        focused    = false,
        focusable  = true,
    }, ListView)
end

function ListView:getSelected() return self.selected, self.items[self.selected] end
function ListView:setItems(t) self.items = t; self.selected = 1; self.scroll = 0 end
function ListView:setFocused(f) self.focused = f end

function ListView:_visibleCount()
    return math.floor((self.h - 2) / W.theme.row_h)
end

function ListView:handleInput(btns, ch)
    local n = #self.items
    if n == 0 then return false end

    if btns & input.BTN_UP ~= 0 then
        self.selected = self.selected - 1
        if self.selected < 1 then self.selected = n end
        if self.onSelect then self.onSelect(self.selected, self.items[self.selected]) end
        self.scroll = scroll_to_show(self.scroll, self.selected, self:_visibleCount())
        return true
    elseif btns & input.BTN_DOWN ~= 0 then
        self.selected = self.selected + 1
        if self.selected > n then self.selected = 1 end
        if self.onSelect then self.onSelect(self.selected, self.items[self.selected]) end
        self.scroll = scroll_to_show(self.scroll, self.selected, self:_visibleCount())
        return true
    elseif btns & input.BTN_ENTER ~= 0 then
        if self.onActivate then self.onActivate(self.selected, self.items[self.selected]) end
        return true
    end
    return false
end

function ListView:draw()
    -- Border and background
    local border = self.focused and W.theme.focus or W.theme.border
    display_draw_rect(self.x, self.y, self.w, self.h, border)
    display_fill_rect(self.x + 1, self.y + 1, self.w - 2, self.h - 2, W.theme.bg)

    local vis = self:_visibleCount()
    local n = #self.items

    for vi = 1, vis do
        local idx = self.scroll + vi
        if idx > n then break end
        local iy = self.y + 1 + (vi - 1) * W.theme.row_h
        local is_sel = (idx == self.selected)
        ui.drawListItem(self.x + 1, iy, self.w - 2, self.items[idx], is_sel, self.focused)
    end

    -- Scrollbar
    if n > vis then
        local sb_x = self.x + self.w - 4
        local sb_y = self.y + 2
        local sb_h = self.h - 4
        display_fill_rect(sb_x, sb_y, 2, sb_h, W.theme.border)

        local thumb_h = math.floor(sb_h * vis / n)
        if thumb_h < 4 then thumb_h = 4 end
        local max_scroll = n - vis
        if max_scroll < 1 then max_scroll = 1 end
        local thumb_y = sb_y + math.floor((sb_h - thumb_h) * self.scroll / max_scroll)
        display_fill_rect(sb_x, thumb_y, 2, thumb_h, disp.WHITE)
    end
end

-- ── Panel ────────────────────────────────────────────────────────────────────

local Panel = {}
Panel.__index = Panel

function W.Panel(opts)
    return setmetatable({
        x     = opts.x or 0,
        y     = opts.y or 0,
        w     = opts.w or 310,
        h     = opts.h or 260,
        title = opts.title,
        focusable = false,
    }, Panel)
end

function Panel:draw()
    ui.drawPanel(self.x, self.y, self.w, self.h, self.title)
end

-- ── Toast ────────────────────────────────────────────────────────────────────

local active_toasts = {}

function W.toast(msg, duration_sec)
    active_toasts[#active_toasts + 1] = {
        text   = msg,
        expire = sys.getTimeMs() + (duration_sec or 2) * 1000,
    }
end

function W.drawToasts()
    local now = sys.getTimeMs()
    local y = 280  -- above typical footer
    local i = #active_toasts
    while i >= 1 do
        local t = active_toasts[i]
        if now >= t.expire then
            table.remove(active_toasts, i)
        else
            ui.drawToast(y, t.text)
            y = y - 18
        end
        i = i - 1
    end
end

-- ── Loading ──────────────────────────────────────────────────────────────────

local Loading = {}
Loading.__index = Loading

function W.Loading(opts)
    return setmetatable({
        text     = opts.text or "Loading...",
        subtext  = opts.subtext,
        progress = opts.progress,  -- nil = spinner, number = progress bar
        frame    = 0,
        focusable = false,
    }, Loading)
end

function Loading:setText(str) self.text = str end
function Loading:setProgress(p) self.progress = p end

function Loading:draw()
    -- Dark overlay
    disp.clear(disp.BLACK)

    local cx = 160
    local cy = 140

    if self.progress then
        -- Progress bar mode
        ui.drawProgress(60, cy, 200, 14, self.progress, W.theme.success)
        cy = cy + 24
    else
        -- Spinner mode
        ui.drawSpinner(cx, cy, 12, self.frame)
        self.frame = self.frame + 1
        cy = cy + 24
    end

    -- Text
    local tw = disp.textWidth(self.text)
    disp.drawText(cx - math.floor(tw / 2), cy, self.text, W.theme.text)

    if self.subtext then
        local sw = disp.textWidth(self.subtext)
        disp.drawText(cx - math.floor(sw / 2), cy + 14, self.subtext, W.theme.text_dim)
    end
end

-- ── FocusGroup ───────────────────────────────────────────────────────────────

local FocusGroup = {}
FocusGroup.__index = FocusGroup

function W.FocusGroup(widgets)
    local self = setmetatable({
        widgets     = widgets or {},
        focus_index = 0,
    }, FocusGroup)
    -- Find first focusable widget
    for i, w in ipairs(self.widgets) do
        if w.focusable then
            self.focus_index = i
            w:setFocused(true)
            break
        end
    end
    return self
end

function FocusGroup:getFocused()
    if self.focus_index > 0 then return self.widgets[self.focus_index] end
    return nil
end

function FocusGroup:getFocusIndex() return self.focus_index end

function FocusGroup:setFocus(index)
    -- Unfocus old
    if self.focus_index > 0 and self.widgets[self.focus_index] then
        local old = self.widgets[self.focus_index]
        if old.setFocused then old:setFocused(false) end
    end
    -- Focus new
    self.focus_index = index
    if index > 0 and self.widgets[index] then
        local w = self.widgets[index]
        if w.setFocused then w:setFocused(true) end
    end
end

function FocusGroup:_findNext(from, dir)
    local n = #self.widgets
    if n == 0 then return 0 end
    local idx = from
    for _ = 1, n do
        idx = idx + dir
        if idx > n then idx = 1 end
        if idx < 1 then idx = n end
        if self.widgets[idx].focusable then return idx end
    end
    return from
end

function FocusGroup:handleInput(btns, ch)
    -- Tab / Shift+Tab
    if btns & input.BTN_TAB ~= 0 then
        local dir = (btns & input.BTN_SHIFT ~= 0) and -1 or 1
        local next_idx = self:_findNext(self.focus_index, dir)
        if next_idx ~= self.focus_index then
            self:setFocus(next_idx)
        end
        return true
    end

    -- Delegate to focused widget
    local w = self:getFocused()
    if w and w.handleInput then
        return w:handleInput(btns, ch)
    end
    return false
end

return W
