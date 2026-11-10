-- A held when the confirm dialog opens must not answer it
-- (tests/e2e/test_pad_modals.py): wait for A to actually be held, so the
-- test can prove it and A never reaches the launcher first, then run the
-- same confirm/textInput sequence as pad_modal_test.
local pc = picocalc
local input, gp, log = pc.input, pc.gamepad, pc.sys.log

pc.display.clear(pc.display.BLACK)
pc.display.flush()
log("PM:WAIT")
while gp.getButtons() & gp.PAD_A == 0 do
    input.update()
    pc.sys.sleep(16)
end
log("PM:HELD")
for i = 1, 2 do
    log("PM:CONFIRM " .. i)
    log("PM:RESULT " .. tostring(pc.ui.confirm("Pad test?")))
end
for i = 1, 2 do
    log("PM:TEXT " .. i)
    log("PM:RESULT " .. tostring(pc.ui.textInput("Name?", "abc")))
end
log("PM:DONE")
