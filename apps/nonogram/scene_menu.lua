-- scene_menu.lua — title screen and puzzle picker.
--
-- With 130 bundled puzzles a flat scrolling list is unusable, so the list is
-- filtered by grid size (LEFT/RIGHT switches ALL/5/10/15/20). Completed puzzles
-- carry a tick and their best time.

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local gfx   = pc.graphics
local sys   = pc.sys

local T       = require("theme")
local Render  = require("render")
local Builtin = require("builtin")
local Records = require("records")
local Pad     = require("pad")

local S = {}

local items, sel, scroll = {}, 1, 0
local logo = nil
local sizes, sizeIdx = nil, 1     -- sizeIdx 1 == "ALL"

local LIST_TOP_ART, LIST_TOP_TEXT = 118, 94
local listTop, VISIBLE = LIST_TOP_TEXT, 9
local ROW_H = 14

local function currentSize()
    if sizeIdx <= 1 then return nil end
    return sizes[sizeIdx - 1]
end

local function rebuild()
    local want = currentSize()
    items = {}
    for _, p in ipairs(Builtin.list()) do
        if want == nil or p.w == want then
            items[#items + 1] = { kind = "puzzle", puzzle = p }
        end
    end
    items[#items + 1] = { kind = "create", label = "CREATE NEW PUZZLE" }
    items[#items + 1] = { kind = "browse", label = "LIBRARY / SHARE CODES" }

    if sel > #items then sel = #items end
    if sel < 1 then sel = 1 end
    -- Keep the selection in view after a filter change.
    if sel - 1 < scroll then scroll = sel - 1 end
    if sel > scroll + VISIBLE then scroll = sel - VISIBLE end
    if scroll < 0 then scroll = 0 end
end

function S.enter()
    Records.load()
    sizes = Builtin.sizes()

    if logo == nil then
        local ok, img = pcall(gfx.image.load, APP_DIR .. "/sprites/logo.png")
        logo = (ok and img) or false
    end

    listTop = logo and LIST_TOP_ART or LIST_TOP_TEXT
    VISIBLE = (320 - T.FOOTER_H - 4 - listTop) // ROW_H

    rebuild()

    local solved, played = Records.summary()
    sys.log(("NG:MENU items=%d logo=%s visible=%d solved=%d plays=%d")
        :format(#items, tostring(logo ~= false), VISIBLE, solved, played))
end

function S.update(dt)
    local _, padPressed = Pad.read()
    local edges = Pad.dpadEdges()

    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
        Records.save()
        NG_QUIT()
        return
    end

    if edges & Pad.DOWN ~= 0 then
        sel = sel + 1; if sel > #items then sel = 1; scroll = 0 end
    end
    if edges & Pad.UP ~= 0 then
        sel = sel - 1
        if sel < 1 then
            sel = #items
            scroll = math.max(0, #items - VISIBLE)
        end
    end

    -- Size filter. Uses press edges rather than repeat edges so holding LEFT
    -- does not race through every filter.
    if padPressed & Pad.LEFT ~= 0 then
        sizeIdx = sizeIdx - 1
        if sizeIdx < 1 then sizeIdx = #sizes + 1 end
        sel, scroll = 1, 0
        rebuild()
    end
    if padPressed & Pad.RIGHT ~= 0 then
        sizeIdx = sizeIdx + 1
        if sizeIdx > #sizes + 1 then sizeIdx = 1 end
        sel, scroll = 1, 0
        rebuild()
    end

    if sel - 1 < scroll then scroll = sel - 1 end
    if sel > scroll + VISIBLE then scroll = sel - VISIBLE end
    if scroll < 0 then scroll = 0 end

    if Pad.confirmed(padPressed) then
        local it = items[sel]
        if it.kind == "puzzle" then
            NG_SCENES.switch("play", { puzzle = it.puzzle })
        elseif it.kind == "create" then
            NG_SCENES.switch("create")
        elseif it.kind == "browse" then
            NG_SCENES.switch("browse")
        end
    end
end

-- Filter chips: ALL 5 10 15 20, with the active one lit.
local function drawFilter(y)
    disp.setFont(T.FONT_6X8)
    local labels = { "ALL" }
    for _, n in ipairs(sizes) do labels[#labels + 1] = tostring(n) end

    local x = T.MARGIN_L + 2
    for i, lbl in ipairs(labels) do
        local active = (i == sizeIdx)
        local w = disp.textWidth(lbl) + 8
        if active then
            gfx.fillBorderedRect(x, y, w, 12, T.LANE_DONE, T.FILL)
        else
            disp.drawRect(x, y, w, 12, T.GRID_MAJOR)
        end
        disp.drawText(x + 4, y + 2, lbl, active and T.FILL or T.TEXT_DIM, false)
        x = x + w + 4
    end

    -- Progress for the current filter, right-aligned on the same row.
    local done, total = 0, 0
    for _, it in ipairs(items) do
        if it.kind == "puzzle" then
            total = total + 1
            if Records.isSolved(it.puzzle.id) then done = done + 1 end
        end
    end
    local prog = ("%d/%d"):format(done, total)
    local pw = disp.textWidth(prog)
    disp.drawText(320 - T.MARGIN_R - pw, y + 2, prog, T.CLUE, false)
end

function S.draw()
    Render.backdrop()
    disp.fillRect(0, 20, 320, (logo and 76 or 52), T.BG_PANEL)
    Render.scanlines()

    if logo then
        local w, h = logo:getSize()
        logo:draw((320 - w) // 2, 24)
    else
        disp.setFont(T.FONT_SCI_BOLD)
        local title = "NEUROGRAM"
        local w = disp.textWidth(title) + (#title - 1) * 4
        local x = (320 - w) // 2
        for pass = 1, 2 do
            local dx = (pass == 1) and 2 or 0
            local col = (pass == 1) and T.ALERT or T.FILL
            local px = x + dx
            for i = 1, #title do
                local chs = title:sub(i, i)
                disp.drawText(px, 34, chs, col, false)
                px = px + disp.textWidth(chs) + 4
            end
        end
        disp.setFont(T.FONT_6X8)
        local sub = "BREAK THE ICE"
        disp.drawText((320 - disp.textWidth(sub)) // 2, 54, sub, T.TEXT_DIM, false)
    end

    drawFilter(listTop - 16)

    disp.setFont(T.FONT_6X8)
    local y = listTop
    for i = scroll + 1, math.min(#items, scroll + VISIBLE) do
        local it = items[i]
        local focused = (i == sel)
        local label, right, solved = nil, nil, false

        if it.kind == "puzzle" then
            label = it.puzzle.name
            solved = Records.isSolved(it.puzzle.id)
            if solved then
                right = Records.formatTime(Records.bestSecs(it.puzzle.id))
            else
                right = ("%dx%d"):format(it.puzzle.w, it.puzzle.h)
            end
        else
            label, right = it.label, ">"
        end

        if focused then
            disp.fillRect(T.MARGIN_L, y - 2, 320 - T.MARGIN_L - T.MARGIN_R, ROW_H,
                          T.LANE_DONE)
            disp.fillVLine(T.MARGIN_L, y - 2, y + ROW_H - 3, T.FILL)
        end

        -- Tick occupies a fixed gutter so names stay aligned whether or not the
        -- puzzle is done.
        if solved then
            Render.tick(T.MARGIN_L + 5, y, 8, T.FILL)
        end
        disp.drawText(T.MARGIN_L + 17, y + 1, label,
                      focused and T.FILL or (solved and T.TEXT_DIM or T.TEXT),
                      false)

        local rw = disp.textWidth(right)
        disp.drawText(320 - T.MARGIN_R - rw - 2, y + 1, right,
                      solved and T.OK or (focused and T.CLUE or T.TEXT_DIM),
                      false)
        y = y + ROW_H
    end

    local solved = select(1, Records.summary())
    Render.header("NEUROGRAM", ("%d done"):format(solved))
    Render.footer("L/R size  UP/DN pick  " .. Pad.label(Pad.A) .. " open", "ESC quit")
end

return S
