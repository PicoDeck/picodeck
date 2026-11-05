-- Calculator — Scientific calculator for PicoDeck
-- Full-featured: Basic, Scientific, Statistics, Base conversion

local pc = picocalc
local disp = pc.display
local input = pc.input
local ui = pc.ui
local fs = pc.fs
local sys = pc.sys

-- ── Load modules ────────────────────────────────────────────────────────────

local Engine = require("calc_engine")
local Functions = require("calc_functions")
local Memory = require("calc_memory")
local Base = require("calc_base")
local Stats = require("calc_stats")
local Complex = require("calc_complex")
local UISolve = require("ui_solve")
local UIGraph = require("ui_graph")

-- ── Initialize ──────────────────────────────────────────────────────────────

local engine = Engine.new()
Functions.register_all(engine)

local memory = Memory.new()
local stats = Stats.new()

-- Seed RNG using system clock
local _clk = sys.getClock()
math.randomseed((_clk.hour or 0) * 3600 + (_clk.min or 0) * 60 + (_clk.sec or 0))

-- Register ANS as dynamic variable
engine:register_variable("ANS", function()
    return memory:get_last_answer() or 0
end)

-- ── Colors ──────────────────────────────────────────────────────────────────

local BG          = disp.BLACK
local FG          = disp.WHITE
local DIM         = disp.GRAY
local ACCENT      = disp.CYAN
local ERROR_COL   = disp.RED
local HEADER_BG   = disp.rgb(20, 25, 40)
local BTN_BG      = disp.rgb(30, 35, 50)
local BTN_FG      = disp.WHITE
local BTN_HL_BG   = disp.rgb(50, 70, 120)
local BTN_OP_BG   = disp.rgb(40, 50, 80)
local BTN_EQ_BG   = disp.rgb(40, 100, 80)
local BTN_FUNC_BG = disp.rgb(50, 40, 70)
local TAB_BG      = disp.rgb(15, 18, 30)
local DISPLAY_BG  = disp.rgb(10, 12, 20)
local RESULT_COL  = disp.rgb(100, 220, 160)
local EXPR_COL    = disp.rgb(180, 190, 210)
local MODE_COL    = disp.rgb(120, 150, 200)
local MEM_COL     = disp.YELLOW

-- ── Layout constants ────────────────────────────────────────────────────────

local SCREEN_W = 320
local SCREEN_H = 320
local HEADER_H = 18
local TAB_H = 16
local DISPLAY_H = 52
local FOOTER_H = 12
local GRID_TOP = HEADER_H + TAB_H + DISPLAY_H
local GRID_H = SCREEN_H - GRID_TOP - FOOTER_H
local GRID_COLS = 6
local GRID_GAP = 2
local GRID_PAD = 4  -- outer padding around the grid
local BTN_W = math.floor((SCREEN_W - 2 * GRID_PAD - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS)
local BTN_H = 16

-- ── State ───────────────────────────────────────────────────────────────────

local expression = ""
local result_text = ""
local result_error = false
local active_tab = 1  -- 1=Basic, 2=Sci, 3=Graph, 4=Solve, 5=Stat, 6=Base
local tab_names = {"Basic", "Sci", "Graph", "Solve", "Stat", "Base"}

local angle_mode = "deg"  -- "deg", "rad", "grad"
local angle_labels = {deg = "DEG", rad = "RAD", grad = "GRAD"}
local number_base = 10
local shift_active = false  -- 2nd function layer
local complex_mode = false

local grid_row = 1
local grid_col = 1
local grid_buttons = {}  -- rebuilt when tab changes

local show_history = false
local history_scroll = 0

-- Stat mode state
local stat_input = ""
local stat_scroll = 0

-- ── Button grid definitions ─────────────────────────────────────────────────

-- Each button: {label, action, [label2], [bg_color]}
-- action: "d:X" digit, "op:X" operator, "f:X" function, "c:X" constant,
--         "cmd:X" command, "ins:X" insert text

local function basic_grid()
    return {
        {{"(","ins:("},  {")",  "ins:)"},  {"C",  "cmd:clear"}, {"CE","cmd:ce"},  {"<-","cmd:bs"},  {"^","ins:^"}},
        {{"7","d:7"},    {"8",  "d:8"},    {"9",  "d:9"},       {"/", "ins:/"},   {"M+","cmd:m+"}, {"M-","cmd:m-"}},
        {{"4","d:4"},    {"5",  "d:5"},    {"6",  "d:6"},       {"*", "ins:*"},   {"MS","cmd:ms"}, {"MR","cmd:mr"}},
        {{"1","d:1"},    {"2",  "d:2"},    {"3",  "d:3"},       {"-", "ins:-"},   {"MC","cmd:mc"}, {"%", "ins:%"}},
        {{"+/-","cmd:neg"},{"0","d:0"},    {".",  "d:."},       {"+", "ins:+"},   {"ANS","c:ANS"}, {"=","cmd:eval"}},
    }
end

local function sci_grid()
    if shift_active then
        return {
            {{"asin","ins:asin("},{"acos","ins:acos("},{"atan","ins:atan("},{"ln",  "ins:ln("},  {"log2","ins:log2("},{"x^y","ins:^"}},
            {{"sinh","ins:sinh("},{"cosh","ins:cosh("},{"tanh","ins:tanh("},{"e^x", "ins:exp("}, {"2^x", "ins:2^"},   {"cbrt","ins:cbrt("}},
            {{"n!",  "ins:fact("},{"nPr", "ins:nPr("}, {"nCr", "ins:nCr("},{"flr", "ins:floor("},{"ceil","ins:ceil("},{"rnd", "ins:round("}},
            {{"gcd","ins:gcd("},{"lcm","ins:lcm("},{"mod","ins:mod("},{"max","ins:max("},{"min","ins:min("},{"CMPLX","cmd:complex"}},
            {{"7","d:7"},  {"8","d:8"},  {"9","d:9"},  {"/","ins:/"},  {"1/x","ins:recip("},{"pi","c:pi"}},
            {{"4","d:4"},  {"5","d:5"},  {"6","d:6"},  {"*","ins:*"},  {"|x|","ins:abs("},  {"e","c:e"}},
            {{"1","d:1"},  {"2","d:2"},  {"3","d:3"},  {"-","ins:-"},  {"(","ins:("},  {")","ins:)"}},
            {{"+/-","cmd:neg"},{"0","d:0"},{".",  "d:."},{"+","ins:+"}, {"CE","cmd:ce"}, {"=","cmd:eval"}},
        }
    else
        return {
            {{"sin","ins:sin("},{"cos","ins:cos("},{"tan","ins:tan("},{"ln",  "ins:ln("},  {"log","ins:log("},{"x^y","ins:^"}},
            {{"sqrt","ins:sqrt("},{"sqr","ins:sqr("},{"cube","ins:cube("},{"e^x","ins:exp("},{"10^x","ins:10^"},{"x^2","ins:^2"}},
            {{"7","d:7"},  {"8","d:8"},  {"9","d:9"},  {"/","ins:/"},  {"(","ins:("},  {")","ins:)"}},
            {{"4","d:4"},  {"5","d:5"},  {"6","d:6"},  {"*","ins:*"},  {"1/x","ins:recip("},{"pi","c:pi"}},
            {{"1","d:1"},  {"2","d:2"},  {"3","d:3"},  {"-","ins:-"},  {"|x|","ins:abs("},  {"e","c:e"}},
            {{"+/-","cmd:neg"},{"0","d:0"},{".",  "d:."},{"+","ins:+"}, {"CE","cmd:ce"}, {"=","cmd:eval"}},
        }
    end
end

local function stat_grid()
    return {
        {{"Add","cmd:stat_add"},{"Del","cmd:stat_del"},{"Clr","cmd:stat_clr"},{"n","cmd:stat_n"},  {"Sum","cmd:stat_sum"}, {"x","cmd:stat_mean"}},
        {{"Med","cmd:stat_med"},{"Mod","cmd:stat_mode"},{"Min","cmd:stat_min"},{"Max","cmd:stat_max"},{"oP","cmd:stat_sdp"},{"oS","cmd:stat_sds"}},
        {{"7","d:7"},  {"8","d:8"},  {"9","d:9"},  {"/","ins:/"},  {"(","ins:("},  {")","ins:)"}},
        {{"4","d:4"},  {"5","d:5"},  {"6","d:6"},  {"*","ins:*"},  {".","d:."},    {"<-","cmd:bs"}},
        {{"1","d:1"},  {"2","d:2"},  {"3","d:3"},  {"-","ins:-"},  {"CE","cmd:ce"},{"=","cmd:eval"}},
        {{"+/-","cmd:neg"},{"0","d:0"},{".",  "d:."},{"+","ins:+"}, {"C","cmd:clear"},{"OK","cmd:stat_add"}},
    }
end

local function base_grid()
    local btns = {
        {{"DEC","cmd:base10"},{"HEX","cmd:base16"},{"OCT","cmd:base8"},{"BIN","cmd:base2"},{"<-","cmd:bs"},{"C","cmd:clear"}},
        {{"AND","ins:AND("},{"OR","ins:OR("},  {"XOR","ins:XOR("},{"NOT","ins:NOT("},{"<<","ins:SHL("},{">>","ins:SHR("}},
        {{"7","d:7"},  {"8","d:8"},  {"9","d:9"},  {"A","d:A"},  {"B","d:B"},  {"C","d:C"}},  -- note: "C" here is hex digit
        {{"4","d:4"},  {"5","d:5"},  {"6","d:6"},  {"D","d:D"},  {"E","d:E"},  {"F","d:F"}},
        {{"1","d:1"},  {"2","d:2"},  {"3","d:3"},  {"/","ins:/"},{"*","ins:*"}, {"=","cmd:eval"}},
        {{"+/-","cmd:neg"},{"0","d:0"},{".",  "d:."},  {"+","ins:+"},{"-","ins:-"},{"CE","cmd:ce"}},
    }
    return btns
end

local function rebuild_grid()
    if active_tab == 1 then
        grid_buttons = basic_grid()
    elseif active_tab == 2 then
        grid_buttons = sci_grid()
    elseif active_tab == 3 then
        grid_buttons = UIGraph.get_grid()
    elseif active_tab == 4 then
        grid_buttons = UISolve.get_grid()
    elseif active_tab == 5 then
        grid_buttons = stat_grid()
    elseif active_tab == 6 then
        grid_buttons = base_grid()
    end
    -- Clamp cursor
    if grid_row > #grid_buttons then grid_row = #grid_buttons end
    if grid_row < 1 then grid_row = 1 end
    if grid_buttons[grid_row] then
        if grid_col > #grid_buttons[grid_row] then grid_col = #grid_buttons[grid_row] end
        if grid_col < 1 then grid_col = 1 end
    end
end

rebuild_grid()

-- ── Action execution ────────────────────────────────────────────────────────

local function do_evaluate()
    if expression == "" then return end
    engine:set_angle_mode(angle_mode)
    local result, err = engine:evaluate(expression)
    if result then
        result_text = engine:format_result(result)
        result_error = false
        memory:push_history(expression, result)
    else
        result_text = err or "Error"
        result_error = true
    end
end

local function execute_action(action)
    if not action then return end
    local kind, val = action:match("^(%a+):(.+)$")
    if not kind then return end

    if kind == "d" then
        expression = expression .. val

    elseif kind == "ins" then
        expression = expression .. val

    elseif kind == "op" then
        expression = expression .. val

    elseif kind == "c" then
        if val == "ANS" then
            expression = expression .. "ANS"
        elseif val == "pi" then
            expression = expression .. "pi"
        elseif val == "e" then
            expression = expression .. "e"
        else
            expression = expression .. val
        end

    elseif kind == "cmd" then
        -- Route graph-specific actions when on Graph tab
        if active_tab == 3 then
            local handled, new_expr = UIGraph.handle_action(action, expression)
            if handled then
                expression = new_expr or expression
                return
            end
        end

        -- Route solver-specific actions when on Solve tab
        if active_tab == 4 then
            local handled, new_expr = UISolve.handle_action(action, expression)
            if handled then
                expression = new_expr or expression
                if val == "solve_run" or (val == "eval" and active_tab == 4) then
                    UISolve.solve_with_engine(engine, complex_mode, Complex)
                end
                return
            end
        end

        if val == "eval" then
            do_evaluate()
        elseif val == "clear" then
            expression = ""
            result_text = ""
            result_error = false
        elseif val == "ce" then
            expression = ""
        elseif val == "bs" then
            -- Remove last character, or last function name
            if #expression > 0 then
                -- Check if last chars are a function call like "sin("
                local func_end = expression:match("[a-zA-Z]+%($")
                if func_end then
                    expression = expression:sub(1, -(#func_end + 1))
                else
                    expression = expression:sub(1, -2)
                end
            end
        elseif val == "neg" then
            if expression == "" and result_text ~= "" and not result_error then
                expression = "-" .. result_text
            elseif expression:sub(1, 1) == "-" then
                expression = expression:sub(2)
            else
                expression = "-" .. expression
            end

        -- Memory
        elseif val == "ms" then
            do_evaluate()
            if not result_error and result_text ~= "" then
                memory:store(tonumber(result_text) or 0)
            end
        elseif val == "mr" then
            expression = expression .. tostring(memory:recall())
        elseif val == "mc" then
            memory:clear()
        elseif val == "m+" then
            do_evaluate()
            if not result_error and result_text ~= "" then
                memory:add(tonumber(result_text) or 0)
            end
        elseif val == "m-" then
            do_evaluate()
            if not result_error and result_text ~= "" then
                memory:subtract(tonumber(result_text) or 0)
            end

        -- Complex mode toggle
        elseif val == "complex" then
            complex_mode = not complex_mode
            engine:set_complex_mode(complex_mode, Complex)
            if complex_mode then
                Functions.register_complex(engine, Complex)
            else
                Functions.unregister_complex(engine)
            end

        -- Number base
        elseif val == "base10" then number_base = 10; engine:set_number_base(10)
        elseif val == "base16" then number_base = 16; engine:set_number_base(16)
        elseif val == "base8" then  number_base = 8;  engine:set_number_base(8)
        elseif val == "base2" then  number_base = 2;  engine:set_number_base(2)

        -- Statistics
        elseif val == "stat_add" then
            if expression ~= "" then
                local n = tonumber(expression)
                if n then
                    stats:add(n)
                    expression = ""
                    result_text = "Added: " .. tostring(n) .. " (n=" .. stats:count() .. ")"
                    result_error = false
                end
            end
        elseif val == "stat_del" then
            local n = stats:count()
            if n > 0 then
                stats:remove(n)
                result_text = "Removed last (n=" .. stats:count() .. ")"
                result_error = false
            end
        elseif val == "stat_clr" then
            stats:clear()
            result_text = "Data cleared"
            result_error = false
        elseif val == "stat_n" then
            result_text = "n = " .. stats:count()
            result_error = false
        elseif val == "stat_sum" then
            local s = stats:sum()
            result_text = "Sum = " .. engine:format_result(s)
            result_error = false
        elseif val == "stat_mean" then
            local m = stats:mean()
            if m then
                result_text = "Mean = " .. engine:format_result(m)
            else
                result_text = "No data"
                result_error = true
            end
        elseif val == "stat_med" then
            local m = stats:median()
            if m then
                result_text = "Median = " .. engine:format_result(m)
            else
                result_text = "No data"
                result_error = true
            end
        elseif val == "stat_mode" then
            local m = stats:mode()
            if m then
                result_text = "Mode = " .. engine:format_result(m)
            else
                result_text = "No mode"
                result_error = false
            end
        elseif val == "stat_min" then
            local m = stats:min()
            if m then
                result_text = "Min = " .. engine:format_result(m)
            else
                result_text = "No data"
                result_error = true
            end
        elseif val == "stat_max" then
            local m = stats:max()
            if m then
                result_text = "Max = " .. engine:format_result(m)
            else
                result_text = "No data"
                result_error = true
            end
        elseif val == "stat_sdp" then
            local s = stats:stddev_pop()
            if s then
                result_text = "oP = " .. engine:format_result(s)
            else
                result_text = "Need data"
                result_error = true
            end
        elseif val == "stat_sds" then
            local s = stats:stddev_sample()
            if s then
                result_text = "oS = " .. engine:format_result(s)
            else
                result_text = "Need 2+ values"
                result_error = true
            end
        end
    end
end

-- ── Drawing ─────────────────────────────────────────────────────────────────

local function draw_status_bar()
    disp.fillRect(0, 0, SCREEN_W, HEADER_H, HEADER_BG)
    disp.setFont(0)  -- 6x8

    -- Angle mode
    disp.drawText(4, 5, angle_labels[angle_mode], MODE_COL, HEADER_BG)

    -- Memory indicator
    if memory:has_value() then
        disp.drawText(36, 5, "M", MEM_COL, HEADER_BG)
    end

    -- Complex mode indicator
    local next_x = 50
    if complex_mode then
        disp.drawText(next_x, 5, "CMPLX", MODE_COL, HEADER_BG)
        next_x = next_x + 36
    end

    -- Base indicator (only in base mode)
    if active_tab == 6 then
        local base_labels = {[10]="DEC", [16]="HEX", [8]="OCT", [2]="BIN"}
        disp.drawText(next_x, 5, base_labels[number_base] or "DEC", MODE_COL, HEADER_BG)
    end

    -- Title
    local title = "Calculator"
    local tw = disp.textWidth(title)
    disp.drawText(math.floor((SCREEN_W - tw) / 2), 5, title, FG, HEADER_BG)

    -- Battery + time
    local bat = sys.getBattery()
    local clock = sys.getClock()
    local right_text = ""
    if clock and clock.synced then
        right_text = string.format("%02d:%02d", clock.hour or 0, clock.min or 0)
    end
    if bat >= 0 then
        right_text = right_text .. " " .. bat .. "%"
    end
    if right_text ~= "" then
        local rw = disp.textWidth(right_text)
        disp.drawText(SCREEN_W - rw - 4, 5, right_text, DIM, HEADER_BG)
    end
end

local function draw_tabs()
    disp.fillRect(0, HEADER_H, SCREEN_W, TAB_H, TAB_BG)
    disp.setFont(0)
    local tab_w = math.floor(SCREEN_W / #tab_names)
    for i, name in ipairs(tab_names) do
        local x = (i - 1) * tab_w
        if i == active_tab then
            disp.fillRect(x, HEADER_H, tab_w, TAB_H, disp.rgb(40, 50, 80))
            disp.drawText(x + math.floor((tab_w - disp.textWidth(name)) / 2), HEADER_H + 4, name, ACCENT, disp.rgb(40, 50, 80))
        else
            disp.drawText(x + math.floor((tab_w - disp.textWidth(name)) / 2), HEADER_H + 4, name, DIM, TAB_BG)
        end
    end
    -- Underline active tab
    local ax = (active_tab - 1) * tab_w
    disp.fillRect(ax, HEADER_H + TAB_H - 2, tab_w, 2, ACCENT)
end

local function draw_display()
    local y = HEADER_H + TAB_H
    disp.fillRect(0, y, SCREEN_W, DISPLAY_H, DISPLAY_BG)

    -- Expression
    disp.setFont(1)  -- 8x12
    local expr_show = expression
    -- Horizontal scroll: show the rightmost portion that fits
    local max_chars = math.floor((SCREEN_W - 16) / 8)
    if #expr_show > max_chars then
        expr_show = expr_show:sub(-max_chars)
    end
    if expr_show == "" then expr_show = "0" end
    local ew = disp.textWidth(expr_show)
    disp.drawText(SCREEN_W - ew - 8, y + 6, expr_show, EXPR_COL, DISPLAY_BG)

    -- Result
    disp.setFont(3)  -- Scientifica Bold
    local res_show = result_text
    local res_col = result_error and ERROR_COL or RESULT_COL
    if res_show ~= "" then
        -- Show "= result" or error
        local prefix = result_error and "" or "= "
        local full = prefix .. res_show
        local rw = disp.textWidth(full)
        disp.drawText(SCREEN_W - rw - 8, y + 26, full, res_col, DISPLAY_BG)
    end

    -- Graph input mode: show y1..y4 slots
    if active_tab == 3 then
        UIGraph.draw_input_display(disp, {
            display_bg = DISPLAY_BG, expr = EXPR_COL, result = RESULT_COL,
            error = ERROR_COL, dim = DIM, accent = ACCENT, fg = FG,
        }, {display_top = y, display_h = DISPLAY_H})
    end

    -- Solve mode: show solver fields and results
    if active_tab == 4 then
        UISolve.draw(disp, {
            display_bg = DISPLAY_BG, expr = EXPR_COL, result = RESULT_COL,
            error = ERROR_COL, dim = DIM, accent = ACCENT, fg = FG,
        }, {display_top = y, display_h = DISPLAY_H})
    end

    -- Base mode: show all bases
    if active_tab == 6 and result_text ~= "" and not result_error then
        local num = tonumber(result_text)
        if num and num == math.floor(num) then
            disp.setFont(0)
            local bases = Base.display_all(num)
            local by = y + 42
            local info = "H:" .. bases.hex .. " O:" .. bases.oct .. " B:" .. bases.bin
            disp.drawText(8, by, info, DIM, DISPLAY_BG)
        end
    end

    -- Stat mode: show data count
    if active_tab == 5 then
        disp.setFont(0)
        local info = "Data: n=" .. stats:count()
        disp.drawText(8, y + 42, info, DIM, DISPLAY_BG)
    end

    -- Divider
    disp.fillRect(0, y + DISPLAY_H - 1, SCREEN_W, 1, disp.rgb(40, 50, 70))
end

local function draw_button_grid()
    disp.setFont(0)  -- 6x8
    local num_rows = #grid_buttons
    -- Calculate button height to fill all available space
    local avail_h = GRID_H - 2 * GRID_PAD
    local btn_h = math.floor((avail_h - (num_rows - 1) * GRID_GAP) / num_rows)
    if btn_h < 14 then btn_h = 14 end

    for r = 1, num_rows do
        local row = grid_buttons[r]
        for c = 1, #row do
            local btn = row[c]
            local label = btn[1]
            local action = btn[2]

            local x = GRID_PAD + (c - 1) * (BTN_W + GRID_GAP)
            local y = GRID_TOP + GRID_PAD + (r - 1) * (btn_h + GRID_GAP)

            -- Choose background color based on button type
            local bg = BTN_BG
            local fg = BTN_FG
            if action then
                local kind = action:match("^(%a+):")
                if kind == "ins" or kind == "op" then
                    bg = BTN_OP_BG
                elseif kind == "cmd" and action:match("eval$") then
                    bg = BTN_EQ_BG
                elseif kind == "f" or (kind == "ins" and action:match("[a-z]+%(")) then
                    bg = BTN_FUNC_BG
                end
            end

            -- Highlight focused button
            local focused = (r == grid_row and c == grid_col)
            if focused then
                bg = BTN_HL_BG
                fg = disp.WHITE
            end

            -- Draw button
            disp.fillRect(x, y, BTN_W, btn_h, bg)
            if focused then
                disp.drawRect(x, y, BTN_W, btn_h, ACCENT)
            end

            -- Center label
            local lw = disp.textWidth(label)
            local lx = x + math.floor((BTN_W - lw) / 2)
            local ly = y + math.floor((btn_h - 8) / 2)
            disp.drawText(lx, ly, label, fg, bg)
        end
    end
end

local function draw_footer()
    local y = SCREEN_H - FOOTER_H
    disp.fillRect(0, y, SCREEN_W, FOOTER_H, HEADER_BG)
    disp.setFont(0)

    local left = "F1:Angle"
    if active_tab == 2 then
        left = left .. " F2:" .. (shift_active and "1st" or "2nd")
    end
    left = left .. " F3:Hist"

    disp.drawText(4, y + 2, left, DIM, HEADER_BG)
    disp.drawText(SCREEN_W - disp.textWidth("Esc:Exit") - 4, y + 2, "Esc:Exit", DIM, HEADER_BG)
end

local function draw_history_overlay()
    if not show_history then return end

    -- Darken background
    disp.applyEffect("darken", 180)

    -- Panel
    local px, py, pw, ph = 20, 30, 280, 250
    disp.fillRect(px, py, pw, ph, disp.rgb(15, 18, 30))
    disp.drawRect(px, py, pw, ph, ACCENT)

    disp.setFont(0)
    disp.drawText(px + 8, py + 4, "History", ACCENT, disp.rgb(15, 18, 30))
    disp.drawText(px + pw - 60, py + 4, "Esc:Close", DIM, disp.rgb(15, 18, 30))

    local history = memory:get_history()
    local visible_rows = math.floor((ph - 24) / 18)
    local start = math.max(1, #history - visible_rows - history_scroll + 1)
    local y = py + 18

    if #history == 0 then
        disp.drawText(px + 8, y + 20, "No history yet", DIM, disp.rgb(15, 18, 30))
    else
        for i = start, math.min(start + visible_rows - 1, #history) do
            local entry = history[i]
            if entry then
                local line_bg = disp.rgb(15, 18, 30)
                disp.drawText(px + 8, y, entry.expr, EXPR_COL, line_bg)
                local res_str = "= " .. engine:format_result(entry.result)
                local rw = disp.textWidth(res_str)
                disp.drawText(px + pw - rw - 8, y, res_str, RESULT_COL, line_bg)
                y = y + 18
            end
        end
    end

    -- Scroll indicators
    if start > 1 then
        disp.drawText(px + pw - 16, py + 16, "^", ACCENT, disp.rgb(15, 18, 30))
    end
    if start + visible_rows - 1 < #history then
        disp.drawText(px + pw - 16, py + ph - 12, "v", ACCENT, disp.rgb(15, 18, 30))
    end
end

-- ── Input handling ──────────────────────────────────────────────────────────

local function handle_char_input(ch)
    if not ch then return false end

    -- Graph fullscreen char input (i=input, t=table, r=reset, +/-=zoom, Esc=back)
    if active_tab == 3 and UIGraph.is_fullscreen() then
        local handled, new_expr = UIGraph.handle_view_char(ch, expression)
        if handled then
            if new_expr then expression = new_expr end
            return true
        end
    end

    -- Direct digit/operator input from physical keyboard
    if ch >= "0" and ch <= "9" then
        expression = expression .. ch
        return true
    elseif ch == "+" or ch == "-" or ch == "*" or ch == "/" or ch == "." then
        expression = expression .. ch
        return true
    elseif ch == "(" or ch == ")" then
        expression = expression .. ch
        return true
    elseif ch == "=" or ch == "\r" or ch == "\n" then
        do_evaluate()
        return true
    elseif ch == "%" then
        expression = expression .. "%"
        return true
    elseif ch == "^" then
        expression = expression .. "^"
        return true
    -- Hex digits in base mode
    elseif active_tab == 6 and ((ch >= "a" and ch <= "f") or (ch >= "A" and ch <= "F")) then
        expression = expression .. ch:upper()
        return true
    end

    return false
end

local function handle_input(pressed)
    -- ESC exits history overlay or app
    if pressed & input.BTN_ESC ~= 0 then
        if show_history then
            show_history = false
            history_scroll = 0
            return true
        end
        -- ESC in graph fullscreen: return to input mode
        if active_tab == 3 and UIGraph.is_fullscreen() then
            UIGraph.handle_action("cmd:graph_input", expression)
            expression = UIGraph.get_active_expression()
            return true
        end
        -- Save history before exit
        local data_dir = "/data/net.picodeck.calculator"
        fs.mkdir(data_dir)
        local json = memory:serialize()
        local h = fs.open(data_dir .. "/history.json", "w")
        if h then
            fs.write(h, json)
            fs.close(h)
        end
        return false  -- signal exit
    end

    -- History overlay navigation
    if show_history then
        if pressed & input.BTN_UP ~= 0 then
            history_scroll = history_scroll + 1
        elseif pressed & input.BTN_DOWN ~= 0 then
            if history_scroll > 0 then history_scroll = history_scroll - 1 end
        elseif pressed & input.BTN_ENTER ~= 0 then
            -- Recall the most recent visible entry
            local history = memory:get_history()
            if #history > 0 then
                local idx = #history - history_scroll
                if idx >= 1 and idx <= #history then
                    expression = tostring(history[idx].result)
                end
            end
            show_history = false
            history_scroll = 0
        end
        return true
    end

    -- F1: cycle angle mode
    if pressed & input.BTN_F1 ~= 0 then
        if angle_mode == "deg" then angle_mode = "rad"
        elseif angle_mode == "rad" then angle_mode = "grad"
        else angle_mode = "deg" end
        return true
    end

    -- F2: toggle 2nd function layer (Sci tab only)
    if pressed & input.BTN_F2 ~= 0 then
        shift_active = not shift_active
        rebuild_grid()
        return true
    end

    -- F3: toggle history
    if pressed & input.BTN_F3 ~= 0 then
        show_history = not show_history
        history_scroll = 0
        return true
    end

    -- Tab switching: F4/F5 or Shift+Left/Right
    if pressed & input.BTN_F4 ~= 0 then
        -- Save expression state when leaving special tabs
        if active_tab == 3 then UIGraph.set_active_expression(expression) end
        if active_tab == 4 then UISolve.set_active_expression(expression) end
        active_tab = active_tab - 1
        if active_tab < 1 then active_tab = #tab_names end
        -- Load expression state when entering special tabs
        if active_tab == 3 then expression = UIGraph.get_active_expression() end
        if active_tab == 4 then expression = UISolve.get_active_expression() end
        rebuild_grid()
        return true
    end
    if pressed & input.BTN_F5 ~= 0 then
        if active_tab == 3 then UIGraph.set_active_expression(expression) end
        if active_tab == 4 then UISolve.set_active_expression(expression) end
        active_tab = active_tab + 1
        if active_tab > #tab_names then active_tab = 1 end
        if active_tab == 3 then expression = UIGraph.get_active_expression() end
        if active_tab == 4 then expression = UISolve.get_active_expression() end
        rebuild_grid()
        return true
    end

    -- Graph fullscreen mode: intercept arrow keys, enter
    if active_tab == 3 and UIGraph.is_fullscreen() then
        if UIGraph.handle_view_input(pressed, input, expression) then
            return true
        end
        -- ESC exits graph fullscreen back to input
        if pressed & input.BTN_ENTER ~= 0 then
            -- Already handled by handle_view_input
        end
    end

    -- Arrow key grid navigation
    if pressed & input.BTN_UP ~= 0 then
        grid_row = grid_row - 1
        if grid_row < 1 then grid_row = #grid_buttons end
        if grid_col > #grid_buttons[grid_row] then grid_col = #grid_buttons[grid_row] end
        return true
    end
    if pressed & input.BTN_DOWN ~= 0 then
        grid_row = grid_row + 1
        if grid_row > #grid_buttons then grid_row = 1 end
        if grid_col > #grid_buttons[grid_row] then grid_col = #grid_buttons[grid_row] end
        return true
    end
    if pressed & input.BTN_LEFT ~= 0 then
        grid_col = grid_col - 1
        if grid_col < 1 then grid_col = #grid_buttons[grid_row] end
        return true
    end
    if pressed & input.BTN_RIGHT ~= 0 then
        grid_col = grid_col + 1
        if grid_col > #grid_buttons[grid_row] then grid_col = 1 end
        return true
    end

    -- Enter = activate focused grid button
    if pressed & input.BTN_ENTER ~= 0 then
        local row = grid_buttons[grid_row]
        if row then
            local btn = row[grid_col]
            if btn then
                execute_action(btn[2])
            end
        end
        return true
    end

    -- Backspace
    if pressed & input.BTN_BACKSPACE ~= 0 then
        execute_action("cmd:bs")
        return true
    end

    -- DEL = clear expression
    if pressed & input.BTN_DEL ~= 0 then
        execute_action("cmd:ce")
        return true
    end

    return true
end

-- ── Load saved history ──────────────────────────────────────────────────────

local function load_history()
    local path = "/data/net.picodeck.calculator/history.json"
    if fs.exists(path) then
        local json = fs.readFile(path)
        if json then
            memory:deserialize(json)
        end
    end
end

load_history()

-- ── Main loop ───────────────────────────────────────────────────────────────

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    -- Character input (physical keyboard)
    local ch = input.getChar()
    if ch then
        handle_char_input(ch)
    end

    -- Button input
    if pressed ~= 0 then
        local cont = handle_input(pressed)
        if not cont then return end  -- exit app
    end

    -- ── Draw ────────────────────────────────────────────────────────────
    disp.clear(BG)
    if active_tab == 3 and UIGraph.is_fullscreen() then
        -- Graph view/trace/table: fullscreen rendering
        draw_status_bar()
        draw_tabs()
        UIGraph.draw_fullscreen(disp, engine)
    else
        draw_status_bar()
        draw_tabs()
        draw_display()
        draw_button_grid()
        draw_footer()
        draw_history_overlay()
    end
    disp.flush()
end
