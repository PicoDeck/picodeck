-- scene_create.lua — draw a puzzle, derive its clues, verify it, save it.
--
-- The uniqueness check runs INCREMENTALLY, a few solver nodes per frame, because
-- a puzzle whose clues admit two solutions is unsolvable in practice and the
-- author needs to know before saving. It cannot use coroutines (the coroutine
-- library is not registered on device), so solver.newJob keeps its own explicit
-- stack and job:step(budget) is called from update().
--
-- Verification restarts on every edit, debounced, so it never blocks drawing.

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local gfx   = pc.graphics
local sys   = pc.sys
local ui    = pc.ui

local T      = require("theme")
local Clues  = require("clues")
local Layout = require("layout")
local Render = require("render")
local Solver = require("solver")
local Store  = require("store")
local Pad    = require("pad")

local S = {}

local SIZES = { 5, 10, 15, 20 }

local grid, w, h, L
local cx, cy = 1, 1
local sizeIdx = 1
local rowClues, colClues
local job, verdict, verifyAt
local blinker
local dirtyClues = true

local VERIFY_DEBOUNCE_MS = 350
local VERIFY_BUDGET = 25          -- solver nodes per frame

local function newGrid(n)
    local g = {}
    for r = 1, n do
        g[r] = {}
        for c = 1, n do g[r][c] = 0 end
    end
    return g
end

local function recomputeClues()
    rowClues, colClues = Clues.derive(grid, w, h)
    dirtyClues = false
end

local function setSize(idx)
    sizeIdx = idx
    w = SIZES[sizeIdx]
    h = w
    grid = newGrid(w)
    cx, cy = 1, 1
    -- Layout is computed WITHOUT clue lists here on purpose: sizing from the
    -- live clues would make the grid resize under the author's cursor on every
    -- edit. The worst-case gutter keeps the geometry stable while drawing.
    L = Layout.compute(w, h)
    job, verdict, verifyAt = nil, nil, nil

    -- Derive immediately rather than deferring to the next update(). A scene's
    -- enter() runs inside the OUTGOING scene's update, so the dispatcher draws
    -- the new scene before ever calling its update — enter() must therefore
    -- leave the scene fully drawable, not merely flagged dirty.
    recomputeClues()
end

local function scheduleVerify()
    verifyAt = sys.getTimeMs() + VERIFY_DEBOUNCE_MS
    job, verdict = nil, nil
end

function S.enter(params)
    setSize((params and params.sizeIdx) or 1)
    blinker = gfx.animation.blinker.new(380, 200, true)
    sys.log(("NG:CREATE %dx%d"):format(w, h))
end

function S.exit()
    grid, L, job, blinker = nil, nil, nil, nil
end

-- ── Actions ──────────────────────────────────────────────────────────────────

local function toggle()
    grid[cy][cx] = (grid[cy][cx] == 1) and 0 or 1
    dirtyClues = true
    scheduleVerify()
end

local function filledCount()
    local n = 0
    for r = 1, h do
        for c = 1, w do n = n + grid[r][c] end
    end
    return n
end

local function doSave()
    if filledCount() == 0 then
        ui.toast("Nothing drawn", ui.TOAST_WARNING)
        return
    end
    if verdict == "multi" then
        if not ui.confirm("Clues are ambiguous. Save anyway?") then
            input.clearState()
            return
        end
    end
    input.clearState()

    local name = ui.textInput("Puzzle name:", "")
    input.clearState()
    if not name or #name == 0 then return end

    if not Store.available() then
        ui.toast("picocalc.json missing", ui.TOAST_ERROR)
        return
    end

    local id, err = Store.save({
        w = w, h = h, solution = grid, name = name:upper(), source = "local",
    })
    if id then
        ui.toast("Saved " .. name, ui.TOAST_SUCCESS)
        sys.log(("NG:CREATE_SAVED %s %s %dx%d"):format(id, name, w, h))
    else
        ui.toast("Save failed: " .. tostring(err), ui.TOAST_ERROR)
        sys.log("NG:CREATE_SAVE_FAIL " .. tostring(err))
    end
end

local function mirrorX()
    for r = 1, h do
        for c = 1, w // 2 do
            grid[r][c], grid[r][w - c + 1] = grid[r][w - c + 1], grid[r][c]
        end
    end
    dirtyClues = true
    scheduleVerify()
end

local function invert()
    for r = 1, h do
        for c = 1, w do grid[r][c] = 1 - grid[r][c] end
    end
    dirtyClues = true
    scheduleVerify()
end

-- ── Update ───────────────────────────────────────────────────────────────────

function S.update(dt)
    local pressed = input.getButtonsPressed()   -- raw keys: Esc only
    local _, padPressed = Pad.read()
    local ch = input.getChar()
    local edges = Pad.dpadEdges()

    if pressed & input.BTN_ESC ~= 0 then
        NG_SCENES.switch("menu")
        return
    end

    local moved = false
    if edges & Pad.UP    ~= 0 then cy = math.max(1, cy - 1); moved = true end
    if edges & Pad.DOWN  ~= 0 then cy = math.min(h, cy + 1); moved = true end
    if edges & Pad.LEFT  ~= 0 then cx = math.max(1, cx - 1); moved = true end
    if edges & Pad.RIGHT ~= 0 then cx = math.min(w, cx + 1); moved = true end
    if moved then L:ensureVisible(cx, cy) end

    if padPressed & Pad.A ~= 0 then toggle() end
    if ch == " " then
        if grid[cy][cx] ~= 0 then grid[cy][cx] = 0; dirtyClues = true; scheduleVerify() end
    end

    if padPressed & Pad.START ~= 0 then
        setSize((sizeIdx % #SIZES) + 1)
        ui.toast(("%dx%d"):format(w, h))
    end
    if padPressed & Pad.B ~= 0 then scheduleVerify(); verifyAt = sys.getTimeMs() end
    if padPressed & Pad.X ~= 0 then mirrorX() end
    if padPressed & Pad.Y ~= 0 then invert() end
    if padPressed & Pad.SELECT ~= 0 then doSave() end

    if dirtyClues then recomputeClues() end

    -- Debounced, incremental verification.
    if verifyAt and sys.getTimeMs() >= verifyAt then
        if not job then
            if filledCount() == 0 then
                verdict, verifyAt = nil, nil
            else
                job = Solver.newJob(w, h, rowClues, colClues, 6000)
            end
        end
        if job then
            if job:step(VERIFY_BUDGET) == "done" then
                verdict = job.result
                job, verifyAt = nil, nil
                sys.log("NG:VERIFY " .. tostring(verdict))
            end
        end
    end
end

-- ── Draw ─────────────────────────────────────────────────────────────────────

local VERDICT_TEXT = {
    ["unique-line"]   = "LOGIC OK",
    ["guess"]         = "NEEDS A GUESS",
    ["multi"]         = "AMBIGUOUS",
    ["contradiction"] = "IMPOSSIBLE",
    ["timeout"]       = "TOO COMPLEX",
}

local VERDICT_COLOR = {
    ["unique-line"]   = T.OK,
    ["guess"]         = T.MAYBE,
    ["multi"]         = T.ALERT,
    ["contradiction"] = T.ALERT,
    ["timeout"]       = T.TEXT_DIM,
}

function S.draw()
    Render.backdrop()

    -- Grid lines plus every filled cell in one C call.
    local pf = {}
    for vr = 1, L.vrows do
        local r = vr + L.oy
        local row = {}
        for vc = 1, L.vcols do
            local c = vc + L.ox
            if grid[r][c] == 1 then row[vc] = T.FILL end
        end
        pf[vr] = row
    end
    gfx.drawPlayfield(pf, L.gridX, L.gridY, L.cell, L.vcols, L.vrows, T.GRID)

    for vc = 0, L.vcols do
        if (vc + L.ox) % 5 == 0 then
            disp.fillVLine(L.gridX + vc * L.cell, L.gridY, L.gridBottom - 1,
                           T.GRID_MAJOR)
        end
    end
    for vr = 0, L.vrows do
        if (vr + L.oy) % 5 == 0 then
            disp.fillHLine(L.gridY + vr * L.cell, L.gridX, L.gridRight - 1,
                           T.GRID_MAJOR)
        end
    end

    Render.scanlines()

    -- Live clue preview, crisp over the scanlined board.
    disp.setFont(T.CLUE_FONT)
    for vr = 1, L.vrows do
        local r = vr + L.oy
        local list = rowClues[r]
        local total = 0
        for i = 1, #list do
            total = total + (#tostring(list[i]) + 1) * T.CLUE_CHAR_W
        end
        local x, y = L:rowClueOrigin(r, total)
        if x then
            for i = 1, #list do
                local s = tostring(list[i])
                disp.drawText(x, y, s, T.CLUE, false)
                x = x + (#s + 1) * T.CLUE_CHAR_W
            end
        end
    end
    for vc = 1, L.vcols do
        local c = vc + L.ox
        local list = colClues[c]
        local k = #list
        for j = 1, k do
            local s = tostring(list[j])
            local x, y = L:colClueSlot(c, j, k, #s)
            if x then disp.drawText(x, y, s, T.CLUE, false) end
        end
    end

    Render.cursor(L, cx, cy, blinker:update())
    Render.scrollbars(L)

    -- Verification status
    local label, col
    if job then
        label = ("ANALYZING %d%%"):format(math.floor((job.progress or 0) * 100))
        col = T.TEXT_DIM
    elseif verifyAt then
        label, col = "...", T.TEXT_DIM
    elseif verdict then
        label, col = VERDICT_TEXT[verdict] or verdict, VERDICT_COLOR[verdict] or T.TEXT
    else
        label, col = "DRAW A SHAPE", T.TEXT_DIM
    end

    Render.header(("CREATE %dx%d"):format(w, h), ("%d cells"):format(filledCount()))

    local y = 320 - T.FOOTER_H
    disp.fillRect(0, y, 320, T.FOOTER_H, T.BG_PANEL)
    disp.fillHLine(y, 0, 319, T.GRID_MAJOR)
    disp.setFont(T.FONT_6X8)
    disp.drawText(T.MARGIN_L, y + 6, Pad.label(Pad.START) .. " size  " .. Pad.label(Pad.X) .. " flip  " ..
                  Pad.label(Pad.Y) .. " inv  " .. Pad.label(Pad.SELECT) .. " save",
                  T.TEXT_DIM, false)
    local lw = disp.textWidth(label)
    disp.drawText(320 - T.MARGIN_R - lw, y + 6, label, col, false)
end

return S
