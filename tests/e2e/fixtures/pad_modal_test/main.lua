-- OS modals on a controller (tests/e2e/test_pad_modals.py): ui.confirm
-- twice, then ui.textInput twice, logging each answer.
local pc = picocalc
local log = pc.sys.log

pc.display.clear(pc.display.BLACK)
pc.display.flush()
for i = 1, 2 do
    log("PM:CONFIRM " .. i)
    log("PM:RESULT " .. tostring(pc.ui.confirm("Pad test?")))
end
for i = 1, 2 do
    log("PM:TEXT " .. i)
    log("PM:RESULT " .. tostring(pc.ui.textInput("Name?", "abc")))
end
log("PM:DONE")
