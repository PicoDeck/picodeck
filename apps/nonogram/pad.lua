-- pad.lua — one place that reads the controls.
--
-- Uses the logical gamepad (picocalc.gamepad, rebindable in Settings >
-- Controls) when the firmware has it. Older firmware has no gamepad: there it
-- reads keys directly, the ones this game used before the gamepad (Enter for
-- A, F4 for Start) and the default bindings for the rest. Esc and text entry
-- stay on picocalc.input.
--
--   Pad.read()             -> held, pressed   masks of Pad.* buttons
--   Pad.dpadEdges()        -> mask of d-pad presses plus auto-repeat (call once a frame)
--   Pad.confirmed(pressed) -> A pressed, or Enter (menus), see below
--   Pad.label(btn)         -> the name of the key bound to btn, for on-screen hints

local input = picocalc.input
local gp    = picocalc.gamepad

local Pad = {}

local DPAD_DELAY, DPAD_RATE = 200, 70
local lastDpad, dpadMs = 0, 0

if gp then
    Pad.UP, Pad.DOWN, Pad.LEFT, Pad.RIGHT = gp.PAD_UP, gp.PAD_DOWN, gp.PAD_LEFT, gp.PAD_RIGHT
    Pad.A, Pad.B, Pad.X, Pad.Y = gp.PAD_A, gp.PAD_B, gp.PAD_X, gp.PAD_Y
    Pad.L, Pad.R, Pad.START, Pad.SELECT = gp.PAD_L, gp.PAD_R, gp.PAD_START, gp.PAD_SELECT

    function Pad.read() return gp.getButtons(), gp.getButtonsPressed() end
    function Pad.label(btn) return (gp.getLabel(btn) or "?"):upper() end
else
    -- The default bindings, as the keys they alias.
    Pad.UP, Pad.DOWN, Pad.LEFT, Pad.RIGHT = input.BTN_UP, input.BTN_DOWN, input.BTN_LEFT, input.BTN_RIGHT
    Pad.A, Pad.B, Pad.X, Pad.Y = input.BTN_ENTER, input.BTN_F5, input.BTN_DEL, input.BTN_BACKSPACE
    Pad.L, Pad.R, Pad.START, Pad.SELECT = input.BTN_F2, input.BTN_F3, input.BTN_F4, input.BTN_TAB

    local names = {
        [Pad.A] = "ENTER", [Pad.B] = "F5", [Pad.X] = "DEL", [Pad.Y] = "BKSP",
        [Pad.L] = "F2", [Pad.R] = "F3", [Pad.START] = "F4", [Pad.SELECT] = "TAB",
    }
    function Pad.read() return input.getButtons(), input.getButtonsPressed() end
    function Pad.label(btn) return names[btn] or "?" end
end

local DPAD = Pad.UP | Pad.DOWN | Pad.LEFT | Pad.RIGHT

-- Menus take A, or Enter as before the gamepad. Enter counts only when it
-- pressed no gamepad button: a player who bound it to one gets that button's
-- meaning alone. `pressed` is this frame's Pad.read() press mask.
function Pad.confirmed(pressed)
    if pressed & Pad.A ~= 0 then return true end
    return gp ~= nil and pressed == 0
        and input.getButtonsPressed() & input.BTN_ENTER ~= 0
end

function Pad.dpadEdges()
    if not gp and type(input.getButtonsRepeated) == "function" then
        return input.getButtonsRepeated() & DPAD
    end
    local held, pressed = Pad.read()
    held, pressed = held & DPAD, pressed & DPAD
    local now = picocalc.sys.getTimeMs()
    local out = pressed
    if held ~= 0 then
        if held ~= lastDpad then
            dpadMs = now
        elseif now - dpadMs >= DPAD_DELAY then
            out = out | held
            dpadMs = now - DPAD_DELAY + DPAD_RATE
        end
    end
    lastDpad = held
    return out
end

return Pad
