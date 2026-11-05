-- What the sound calls say: the results of play(), clearing a callback with
-- nil, the audio clock's unit and the blank sample's silence. picotest kit.
--
-- The harness stages short.wav (0.05 s) and long.wav (0.25 s), both mono
-- 16-bit 22050 Hz. See tests/e2e/test_sound_calls.py.

local pc = picocalc
local sound = pc.sound
local fs = pc.fs
local sys = pc.sys
local T = sys.loadlib("picotest")

local SHORT = APP_DIR .. "/short.wav"   -- 0.05 s
local LONG = APP_DIR .. "/long.wav"     -- 0.25 s

-- Runs until pred() holds (the service pass runs inside sys.sleep).
local function wait_until(pred, ms, what)
    local t0 = sys.getTimeMs()
    while not pred() do
        if sys.getTimeMs() - t0 > ms then
            error("timed out after " .. ms .. " ms: " .. what, 2)
        end
        sys.sleep(5)
    end
end

local function silence_done()
    wait_until(function() return sound.playingSources() == 0 end, 5000,
               "sounds still playing")
    sys.sleep(30)
end

-- First, in a run of its own: the output has not started yet.
T.case("clock_stands_still_before_the_first_sound", function()
    T.eq(sound.getCurrentTime(), 0, "before any sound")
    sys.sleep(100)
    T.eq(sound.getCurrentTime(), 0, "with the output off")
end)

T.case("getCurrentTime_counts_milliseconds", function()
    T.eq(math.type(sound.getCurrentTime()), "integer", "an integer")
    local s = T.ok(sound.sample(SHORT), "sample")
    s:play()
    sound.resetTime()
    local t0 = sys.getTimeMs()
    sys.sleep(1500)
    local wall = sys.getTimeMs() - t0
    local clock = sound.getCurrentTime()
    T.ok(clock > 1000, "after 1.5 s the clock reads " .. clock)
    T.ok(clock < wall + 500, "after " .. wall .. " ms the clock reads " .. clock)
    sound.resetTime()
    T.ok(sound.getCurrentTime() < 100, "reset: " .. sound.getCurrentTime())
end)

T.done()
