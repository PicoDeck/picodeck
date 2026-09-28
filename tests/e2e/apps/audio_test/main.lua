-- Audio test fixture (picotest kit): picocalc.audio calls must not raise.
-- (The sim's audio is sim_audio.c, so this is API coverage, not DSP.)

local pc = picocalc
local audio = pc.audio
local T = pc.sys.loadlib("picotest")

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running audio tests...", pc.display.WHITE)
pc.display.flush()

T.case("setVolume", function()
    audio.setVolume(50)
end)

T.case("playTone", function()
    audio.playTone(440, 500)
end)

T.case("stopTone", function()
    audio.playTone(880, 2000)
    pc.sys.sleep(50)
    audio.stopTone()
end)

T.case("volume_range", function()
    audio.setVolume(0)
    audio.setVolume(100)
    audio.setVolume(50)
end)

-- Left turned down on purpose: test_audio.py checks the exit resets it.
T.case("leave_volume_low", function()
    audio.setVolume(20)
end)

T.done()
