-- scene_browse.lua — the local library plus share-code import and export.
--
-- Blocking modals (ui.textInput, ui.confirm) run their own poll/draw loop in C,
-- so on return the framebuffer is dirty AND the back buffer is two frames stale.
-- The frame loop fully repaints every frame so that resolves itself, but input
-- state has to be cleared or the key that dismissed the modal leaks into here.

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local sys   = pc.sys
local ui    = pc.ui

local T      = require("theme")
local Render = require("render")
local Store  = require("store")
local Share  = require("sharecode")
local Pad    = require("pad")

local S = {}

local items, sel, scroll = {}, 1, 0
local status = nil
local VISIBLE = 8

local function refresh()
    items = {}
    items[1] = { kind = "import", label = "IMPORT SHARE CODE" }
    for _, rec in ipairs(Store.list()) do
        items[#items + 1] = { kind = "puzzle", rec = rec }
    end
    if sel > #items then sel = #items end
    if sel < 1 then sel = 1 end
end

function S.enter()
    if not Store.available() then
        status = "picocalc.json missing — library unavailable"
        items = {}
        sys.log("NG:BROWSE unavailable (no picocalc.json)")
        return
    end
    Store.init()
    refresh()
    status = nil
    sys.log(("NG:BROWSE items=%d"):format(#items))
end

local function doImport()
    local code = ui.textInput("Share code:", "")
    input.clearState()
    if not code or #code == 0 then return end

    local id, err = Store.importCode(code)
    if not id then
        status = "import failed: " .. tostring(err)
        ui.toast(status, ui.TOAST_ERROR)
        sys.log("NG:IMPORT_FAIL " .. tostring(err))
        return
    end

    refresh()
    status = "imported " .. id
    ui.toast("Imported", ui.TOAST_SUCCESS)
    sys.log("NG:IMPORT_OK " .. id)
end

local function doExport(rec)
    local p, err = Store.load(rec.id)
    if not p then
        status = "load failed: " .. tostring(err)
        return
    end
    local code = Store.exportCode(p)
    local grouped = Share.group(code, 10)
    sys.log("NG:EXPORT " .. grouped)

    -- Shown in a text field so it can be read off the screen and retyped; the
    -- device has no clipboard.
    ui.textInput("Share code (copy):", grouped)
    input.clearState()
    status = ("exported %d chars"):format(#grouped)
end

function S.update(dt)
    local _, padPressed = Pad.read()
    local edges = Pad.dpadEdges()

    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
        NG_SCENES.switch("menu")
        return
    end

    if #items == 0 then return end

    if edges & Pad.DOWN ~= 0 then
        sel = sel + 1; if sel > #items then sel = 1 end
    end
    if edges & Pad.UP ~= 0 then
        sel = sel - 1; if sel < 1 then sel = #items end
    end
    if sel - 1 < scroll then scroll = sel - 1 end
    if sel > scroll + VISIBLE then scroll = sel - VISIBLE end
    if scroll < 0 then scroll = 0 end

    local it = items[sel]

    if Pad.confirmed(padPressed) then
        if it.kind == "import" then
            doImport()
        elseif it.kind == "puzzle" then
            local p, err = Store.load(it.rec.id)
            if p then
                NG_SCENES.switch("play", { puzzle = p })
            else
                status = "load failed: " .. tostring(err)
            end
        end
    end

    if padPressed & Pad.B ~= 0 and it.kind == "puzzle" then
        doExport(it.rec)
    end

    if padPressed & Pad.X ~= 0 and it.kind == "puzzle" then
        if ui.confirm("Delete " .. it.rec.name .. "?") then
            Store.delete(it.rec.id)
            refresh()
            status = "deleted"
        end
        input.clearState()
    end
end

function S.draw()
    Render.backdrop()
    Render.scanlines()

    disp.setFont(T.FONT_6X8)

    if #items == 0 then
        local msg = status or "library empty"
        disp.drawText((320 - disp.textWidth(msg)) // 2, 150, msg, T.ALERT, false)
        Render.header("LIBRARY", "")
        Render.footer("ESC back", "")
        return
    end

    local y = 30
    for i = scroll + 1, math.min(#items, scroll + VISIBLE) do
        local it = items[i]
        local focused = (i == sel)
        local label, right

        if it.kind == "import" then
            label, right = it.label, "+"
        else
            label = it.rec.name
            right = ("%dx%d"):format(it.rec.w or 0, it.rec.h or 0)
        end

        if focused then
            disp.fillRect(T.MARGIN_L, y - 2, 320 - T.MARGIN_L - T.MARGIN_R, 16,
                          T.LANE_DONE)
            disp.fillVLine(T.MARGIN_L, y - 2, y + 13, T.FILL)
        end
        disp.drawText(T.MARGIN_L + 6, y + 2, label,
                      focused and T.FILL or T.TEXT, false)
        local rw = disp.textWidth(right)
        disp.drawText(320 - T.MARGIN_R - rw - 4, y + 2, right,
                      focused and T.CLUE or T.TEXT_DIM, false)
        y = y + 18
    end

    if status then
        disp.drawText(T.MARGIN_L + 6, 320 - T.FOOTER_H - 12, status,
                      T.TEXT_DIM, false)
    end

    Render.header("LIBRARY", ("%d"):format(#items - 1))
    Render.footer(Pad.label(Pad.A) .. " open  " .. Pad.label(Pad.B) .. " export  " ..
                  Pad.label(Pad.X) .. " delete", "ESC back")
end

return S
