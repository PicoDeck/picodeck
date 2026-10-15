-- Input test fixture
-- Logs each button press/character received for E2E verification.
-- Exits on ESC key.

local pc = picocalc
local input = pc.input
local log = pc.sys.log

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Input Test - press keys", pc.display.WHITE)
pc.display.flush()

log("INPUT_READY")

-- Map button masks to names
local buttons = {
    {input.BTN_UP, "up"},
    {input.BTN_DOWN, "down"},
    {input.BTN_LEFT, "left"},
    {input.BTN_RIGHT, "right"},
    {input.BTN_ENTER, "enter"},
    {input.BTN_F1, "f1"},
    {input.BTN_F2, "f2"},
    {input.BTN_F3, "f3"},
    {input.BTN_F4, "f4"},
    {input.BTN_F5, "f5"},
}

local prev_held = 0

while true do
    input.update()
    local held = input.getButtons()

    -- Check for ESC exit
    if (held & input.BTN_ESC) ~= 0 then
        log("INPUT_EXIT")
        return
    end

    -- Detect newly pressed buttons (edge detection)
    local newly_pressed = held & ~prev_held
    for _, btn in ipairs(buttons) do
        if (newly_pressed & btn[1]) ~= 0 then
            log("BTN:" .. btn[2])
        end
    end

    -- Check for character input
    local ch = input.getChar()
    if ch == "#" then
        -- '#' opens a text box: Enter (a '\n' char) accepts, Backspace ('\b') edits
        log("TEXT_PROMPT")
        local v = pc.ui.textInput("Text:")
        log("TEXT:" .. (v or "<cancelled>"))
    elseif ch and ch ~= "" and ch:byte() < 32 then
        log("CHARCODE:" .. ch:byte())  -- control chars (Enter, Backspace) by code
    elseif ch and ch ~= "" then
        log("CHAR:" .. ch)
    end

    prev_held = held
    pc.sys.sleep(16)
end
