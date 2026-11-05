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

T.case("play_reports_nothing_to_play", function()
    local empty = T.ok(sound.sample(), "empty sample")
    local sp = T.ok(sound.sampleplayer(), "sampleplayer")
    local r, err = sp:play()
    T.ok(r == nil and err, "sampleplayer:play with no sample: " .. tostring(err))
    sp:setSample(empty)
    r, err = sp:play()
    T.ok(r == nil and err, "sampleplayer:play with an empty sample: " .. tostring(err))
    sp:setSample(T.ok(sound.sample(SHORT), "sample"))
    T.eq(sp:play(), true, "sampleplayer:play with a sample")
    sp:stop()

    local fp = T.ok(sound.fileplayer(), "fileplayer")
    r, err = fp:play()
    T.ok(r == nil and err, "fileplayer:play with no file: " .. tostring(err))
    T.eq(fp:isPlaying(), false, "fileplayer isPlaying")
    T.ok(fp:load(LONG), "fileplayer load")
    T.eq(fp:play(), true, "fileplayer:play with a file")
    fp:stop()

    local mp = T.ok(sound.mp3player(), "mp3player")
    r, err = mp:play()
    T.ok(r == nil and err, "mp3player:play with no file: " .. tostring(err))
    T.eq(mp:isPlaying(), false, "mp3player isPlaying")
end)

T.case("sampleplayer_callback_cleared_with_nil", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(sound.sampleplayer(s), "player")
    local fired = 0
    p:setFinishCallback(function() fired = fired + 1 end)
    p:setLoopCallback(function() fired = fired + 1 end)
    p:setFinishCallback(nil)
    p:setLoopCallback(nil)
    p:play(2)
    silence_done()
    T.eq(fired, 0, "a cleared callback fired")
    -- Set and clear never run out of slots.
    for i = 1, 40 do
        p:setFinishCallback(function() end)
        p:setFinishCallback(nil)
        p:setLoopCallback(function() end)
        p:setLoopCallback(nil)
    end
    p:setFinishCallback(function() fired = fired + 1 end)
    p:play()
    silence_done()
    T.eq(fired, 1, "set again after clearing")
    T.ok(not pcall(p.setFinishCallback, p, 42), "a non-function is still refused")
end)

T.case("fileplayer_callback_cleared_with_nil", function()
    local fp = T.ok(sound.fileplayer(), "fileplayer")
    local fired = 0
    fp:setFinishCallback(function() fired = fired + 1 end)
    fp:setLoopCallback(function() fired = fired + 1 end)
    fp:setFinishCallback(nil)
    fp:setLoopCallback(nil)
    T.ok(fp:load(SHORT), "load")
    fp:play()
    wait_until(function() return not fp:isPlaying() end, 5000, "fileplayer")
    sys.sleep(30)
    T.eq(fired, 0, "a cleared callback fired")
    for i = 1, 10 do
        fp:setFinishCallback(function() end)
        fp:setFinishCallback(nil)
        fp:setLoopCallback(function() end)
        fp:setLoopCallback(nil)
    end
    fp:setFinishCallback(function() fired = fired + 1 end)
    fp:play()
    wait_until(function() return fired == 1 end, 5000, "set again after clearing")
    T.ok(not pcall(fp.setFinishCallback, fp, 42), "a non-function is still refused")
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

T.case("blank_samples_are_silent", function()
    -- The 8-bit silence is 128: zeros would be full-scale negative.
    local cases = {
        { bits = 8,  channels = 1, sampleRate = 8000, silence = 128 },
        { bits = 8,  channels = 2, sampleRate = 8000, silence = 128 },
        { bits = 16, channels = 1, sampleRate = 8000, silence = 0 },
    }
    for _, c in ipairs(cases) do
        local s = T.ok(sound.sample(0.1, c), "blank")
        local fmt = s:getFormat()
        T.eq(fmt.bits, c.bits, "bits")
        T.eq(fmt.channels, c.channels, "channels")
        T.eq(fmt.sampleRate, c.sampleRate, "sampleRate")
        T.eq(s:getLength(), 800, "frames")
        local path = fs.appPath("blank.wav")
        T.ok(s:save(path), "save")
        local data = T.ok(fs.readFile(path), "read back")
        T.eq(#data, 44 + 800 * c.bits // 8 * c.channels, "file size")
        for i = 45, #data do
            T.ok(data:byte(i) == c.silence, "byte " .. i .. " is " .. data:byte(i))
        end
    end
end)

T.done()
