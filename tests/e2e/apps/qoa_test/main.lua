-- QOA streaming through the fileplayer (the Quite OK Audio format, decoded
-- on Core 1 by src/drivers/qoa.c). picotest kit.
--
-- The harness stages three_s.qoa (3.0 s mono 22050 Hz), one_s.qoa (1.0 s)
-- and stereo_s.qoa (1.0 s stereo 44100 Hz).  The simulator drains the stream ring at the real 44.1 kHz
-- output rate, so wall-clock play time checks the producer's flow control,
-- exactly as tests/e2e/apps/fileplayer_test does for WAV.
-- See tests/e2e/test_qoa.py.

local pc = picocalc
local sound = pc.sound
local sys = pc.sys
local T = pc.sys.loadlib("picotest")

local THREE = APP_DIR .. "/three_s.qoa"
local ONE = APP_DIR .. "/one_s.qoa"
local STEREO = APP_DIR .. "/stereo_s.qoa"

-- Play until the player stops (or `limit` ms); returns the elapsed ms.
local function play_ms(fp, limit)
    local t0 = sys.getTimeMs()
    fp:play()
    while fp:isPlaying() do
        if sys.getTimeMs() - t0 > limit then break end
        sys.sleep(10)
    end
    return sys.getTimeMs() - t0
end

local function log(msg) sys.log("QOA:" .. msg) end

local ONLY = pc.fs.exists(APP_DIR .. "/only.flag") and pc.fs.readFile(APP_DIR .. "/only.flag")
if ONLY then
    local case = T.case
    T.case = function(name, fn)
        if name == ONLY then return case(name, fn) end
    end
end

T.case("qoa_plays_for_its_duration", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    T.eq(fp:getLength(), 3 * 22050)
    local ms = play_ms(fp, 8000)
    log("three_s played " .. ms .. " ms")
    T.ok(ms >= 2500, "a 3 s QOA finished after only " .. ms .. " ms")
    T.ok(ms <= 6000, "a 3 s QOA took " .. ms .. " ms")
    fp:stop()
end)

T.case("qoa_stereo_44100_plays", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(STEREO))
    T.eq(fp:getLength(), 44100)
    local ms = play_ms(fp, 6000)
    log("stereo 44.1 kHz played " .. ms .. " ms")
    T.ok(ms >= 700 and ms <= 3000, "a 1 s stereo QOA took " .. ms .. " ms")
    fp:stop()
end)

T.case("qoa_position_tracks_playback", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    fp:play()
    sys.sleep(1000)
    local off = fp:getOffset()
    log("offset after 1 s: " .. tostring(off))
    T.ok(fp:isPlaying(), "stopped within 1 s of a 3 s QOA")
    T.ok(off >= 0 and off <= 2, "offset " .. tostring(off) .. " s after 1 s of playback")
    fp:stop()
end)

T.case("qoa_seek_skips_forward", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    fp:play()
    fp:setOffset(2)
    T.eq(fp:getOffset(), 2)
    local t0 = sys.getTimeMs()
    while fp:isPlaying() do
        if sys.getTimeMs() - t0 > 6000 then break end
        sys.sleep(10)
    end
    local ms = sys.getTimeMs() - t0
    log("after seek to 2 s, finished in " .. ms .. " ms")
    -- One second of audio remains (plus ring drain), never the whole file.
    T.ok(ms >= 500 and ms <= 2500, "played " .. ms .. " ms after seeking to 2 s of a 3 s QOA")
    fp:stop()
end)

T.case("qoa_stop_and_replay", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    for _ = 1, 5 do
        fp:play()
        sys.sleep(20)
        fp:stop()
        T.ok(not fp:isPlaying(), "still playing after stop")
    end
    T.ok(fp:load(ONE))
    local ms = play_ms(fp, 8000)
    T.ok(ms >= 700, "replayed 1 s QOA finished after " .. ms .. " ms")
end)

T.case("qoa_play_zero_loops_until_stopped", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    fp:play(0)
    sys.sleep(2500)
    T.ok(fp:isPlaying(), "play(0) of a 1 s QOA stopped within 2.5 s")
    fp:stop()
end)

T.case("load_reports_why_a_file_cannot_play", function()
    local fp = sound.fileplayer()
    for _, name in ipairs({"missing.qoa", "three_ch.qoa"}) do
        local ok, err = fp:load(APP_DIR .. "/" .. name)
        T.eq(ok, nil, name .. " load result")
        T.ok(type(err) == "string" and #err > 0, name .. ": no reason (" .. tostring(err) .. ")")
        log(name .. " -> " .. tostring(err))
    end
    -- The sandbox refusal has the same shape.
    local ok, err = fp:load("/data/some.other.app/x.wav")
    T.eq(ok, nil)
    T.eq(err, "access denied")
    -- A refused file leaves nothing to play.
    T.eq(fp:getLength(), 0)
    fp:play()
    T.ok(not fp:isPlaying(), "a refused file plays")
    -- And a good file loads, reports its rate, and plays.
    T.eq(fp:load(ONE), true)
    T.eq(fp:getSampleRate(), 22050)
    T.eq(fp:getLength() / fp:getSampleRate(), 1)
end)

T.case("did_underrun_is_a_boolean_and_clears", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    T.eq(fp:didUnderrun(), false)
    fp:play()
    sys.sleep(300)
    local first = fp:didUnderrun()
    T.eq(type(first), "boolean")
    T.eq(fp:didUnderrun(), false, "a second read still true")
    fp:stop()
end)

T.case("loop_range_loops_the_range", function()
    -- 3 s QOA, range 1-2 s: the loop callback fires at ~2 s and then every
    -- second (without a range it fires once, at 3 s), and the position stays
    -- inside the range once it has wrapped.
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    local loops = 0
    fp:setLoopCallback(function() loops = loops + 1 end)
    fp:setLoopRange(1, 2)
    fp:play()
    local t0 = sys.getTimeMs()
    while sys.getTimeMs() - t0 < 4600 do
        pc.input.update()
        sys.sleep(10)
    end
    local off = fp:getOffset()
    log("loops after 4.6 s: " .. loops .. ", offset " .. off)
    T.ok(fp:isPlaying(), "stopped instead of looping")
    T.ok(loops >= 3, "only " .. loops .. " loop callbacks in 4.6 s of a 1 s loop")
    T.ok(off >= 1 and off <= 2, "offset " .. off .. " s is outside the 1-2 s range")
    fp:stop()
end)

T.case("loop_range_without_args_loops_whole_file", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    local loops = 0
    fp:setLoopCallback(function() loops = loops + 1 end)
    fp:setLoopRange()
    fp:play()
    local t0 = sys.getTimeMs()
    while sys.getTimeMs() - t0 < 2600 do
        pc.input.update()
        sys.sleep(10)
    end
    T.ok(fp:isPlaying(), "stopped instead of looping")
    T.ok(loops >= 1 and loops <= 2, loops .. " loops of a 1 s file in 2.6 s")
    fp:stop()
end)

T.done()
