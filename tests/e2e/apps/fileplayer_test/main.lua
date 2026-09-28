-- WAV fileplayer flow control and per-player files (review: Audio/storage
-- Critical row 1 "The WAV fileplayer f_reads 4 KB on every tick ... a long
-- WAV ends in seconds"; High row "One s_current_file is shared by all
-- fileplayer instances"; Task 14). picotest kit.
--
-- The harness stages three_s.wav (3.0 s) and one_s.wav (1.0 s), mono 16-bit
-- 22050 Hz. A fileplayer must take about as long as its audio to finish:
-- reading faster than the output drains means the stream drops what it
-- can't hold and the track "ends" in a fraction of its length. The
-- simulator drains the stream ring at the real 44.1 kHz output rate, so the
-- wall-clock play time is the check.
-- See tests/e2e/test_fileplayer.py.

local pc = picocalc
local sound = pc.sound
local sys = pc.sys
local T = pc.sys.loadlib("picotest")

local THREE = APP_DIR .. "/three_s.wav"
local ONE = APP_DIR .. "/one_s.wav"
local EMPTY = APP_DIR .. "/empty.wav"

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

local function log(msg) sys.log("FP:" .. msg) end

local ONLY = pc.fs.exists(APP_DIR .. "/only.flag") and pc.fs.readFile(APP_DIR .. "/only.flag")
if ONLY then
    local case = T.case
    T.case = function(name, fn)
        if name == ONLY then return case(name, fn) end
    end
end

T.case("wav_plays_for_its_duration", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    T.eq(fp:getLength(), 3 * 22050)
    local ms = play_ms(fp, 8000)
    log("three_s played " .. ms .. " ms")
    T.ok(ms >= 2500, "a 3 s WAV finished after only " .. ms .. " ms")
    T.ok(ms <= 6000, "a 3 s WAV took " .. ms .. " ms")
    fp:stop()
end)

T.case("position_tracks_playback", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(THREE))
    fp:play()
    sys.sleep(1000)
    -- Seconds of data consumed: the producer leads the output by at most
    -- the stream ring (well under a second), never the whole file.
    local off = fp:getOffset()
    log("offset after 1 s: " .. tostring(off))
    T.ok(fp:isPlaying(), "stopped within 1 s of a 3 s WAV")
    T.ok(off <= 2, "offset " .. tostring(off) .. " s after 1 s of playback")
    fp:stop()
end)

T.case("players_keep_their_own_files", function()
    local a = sound.fileplayer()
    local b = sound.fileplayer()
    T.ok(a:load(THREE))
    T.ok(b:load(ONE))       -- must not take a's file away
    local ms = play_ms(a, 8000)
    log("a after b:load played " .. ms .. " ms")
    T.ok(ms >= 2500, "player a played " .. ms .. " ms of its 3 s WAV")
    a:stop()
    ms = play_ms(b, 8000)
    log("b played " .. ms .. " ms")
    T.ok(ms >= 700 and ms <= 3000, "player b played " .. ms .. " ms of its 1 s WAV")
    b:stop()
end)

T.case("stop_and_replay", function()
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
    T.ok(ms >= 700, "replayed 1 s WAV finished after " .. ms .. " ms")
end)

T.case("play_zero_loops_until_stopped", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    fp:play(0)
    sys.sleep(2500)
    T.ok(fp:isPlaying(), "play(0) of a 1 s WAV stopped within 2.5 s")
    fp:stop()
end)

T.case("play_n_plays_n_times", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    local t0 = sys.getTimeMs()
    fp:play(2)
    while fp:isPlaying() and sys.getTimeMs() - t0 < 8000 do sys.sleep(10) end
    local ms = sys.getTimeMs() - t0
    T.ok(ms >= 1700 and ms <= 5000, "play(2) of a 1 s WAV took " .. ms .. " ms")
    fp:stop()
end)

T.case("play_zero_on_empty_wav_finishes", function()
    -- A pass that reads nothing must end the play, not rewind forever.
    local fp = sound.fileplayer()
    T.ok(fp:load(EMPTY))
    fp:play(0)
    local t0 = sys.getTimeMs()
    while fp:isPlaying() and sys.getTimeMs() - t0 < 1500 do sys.sleep(10) end
    T.ok(not fp:isPlaying(), "play(0) of an empty WAV never finished")
    fp:stop()
end)

T.case("loop_and_finish_callbacks_per_play", function()
    local fp = sound.fileplayer()
    T.ok(fp:load(ONE))
    local loops, finished = 0, false
    fp:setLoopCallback(function() loops = loops + 1 end)
    fp:setFinishCallback(function() finished = true end)
    fp:play(3)
    local t0 = sys.getTimeMs()
    while not finished and sys.getTimeMs() - t0 < 6000 do
        pc.input.update()
        sys.sleep(10)
    end
    T.ok(finished, "the finish callback never fired")
    T.eq(loops, 2, "loop callbacks for play(3)")
    fp:stop()
end)

T.done()
