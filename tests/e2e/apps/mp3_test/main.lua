-- MP3 decode loop termination (review: Audio/storage High "The MP3 decode
-- loop busy-spins on Core 1 when the SD try-lock fails and a partial frame
-- remains"; Task 14). picotest kit.
--
-- The harness stages whole.mp3 (8 silent frames) and truncated.mp3 (the
-- same with its last frame cut short). At the end of truncated.mp3 a
-- partial frame stays buffered and no new data comes: the old loop retried
-- the refill forever, so the player never finished and Core 1 (fileplayer,
-- MOD, network) hung with it. The simulator mirrors the firmware loop.
-- See tests/e2e/test_mp3player.py.

local pc = picocalc
local sound = pc.sound
local sys = pc.sys
local T = pc.sys.loadlib("picotest")

local ONLY = pc.fs.exists(APP_DIR .. "/only.flag") and pc.fs.readFile(APP_DIR .. "/only.flag")
if ONLY then
    local case = T.case
    T.case = function(name, fn)
        if name == ONLY then return case(name, fn) end
    end
end

local function play_until_done(path, limit)
    local mp = sound.mp3player()
    T.ok(mp:load(path), "load " .. path)
    local t0 = sys.getTimeMs()
    mp:play()
    while mp:isPlaying() and sys.getTimeMs() - t0 < limit do
        sys.sleep(20)
    end
    local ms = sys.getTimeMs() - t0
    local still = mp:isPlaying()
    mp:stop()
    return ms, still
end

T.case("whole_mp3_finishes", function()
    local ms, still = play_until_done(APP_DIR .. "/whole.mp3", 4000)
    sys.log("MP3:whole " .. ms .. " ms")
    T.ok(not still, "8-frame MP3 still playing after " .. ms .. " ms")
end)

T.case("truncated_mp3_finishes", function()
    local ms, still = play_until_done(APP_DIR .. "/truncated.mp3", 4000)
    sys.log("MP3:truncated " .. ms .. " ms")
    T.ok(not still, "truncated MP3 still playing after " .. ms .. " ms (decode loop stuck)")
end)

T.case("looping_truncated_mp3_keeps_core1_alive", function()
    -- A looping file that ends mid-frame: every end is a rewind, and the
    -- loop must still yield Core 1 between updates. A WAV fileplayer
    -- (Core 1 too) must keep making progress meanwhile.
    local mp = sound.mp3player()
    T.ok(mp:load(APP_DIR .. "/truncated.mp3"))
    mp:play(0)   -- 0 = loop forever (play() sets the loop flag from it)
    local fp = sound.fileplayer()
    T.ok(fp:load(APP_DIR .. "/tone.wav"))
    fp:play()
    local t0 = sys.getTimeMs()
    while fp:isPlaying() and sys.getTimeMs() - t0 < 4000 do
        sys.sleep(20)
    end
    local ms = sys.getTimeMs() - t0
    T.ok(not fp:isPlaying(), "0.5 s WAV still playing after " .. ms .. " ms: Core 1 is stuck")
    T.ok(mp:isPlaying(), "looping MP3 stopped")
    mp:stop()
    fp:stop()
end)

T.case("load_failure_is_reported", function()
    local mp = sound.mp3player()
    local ok, err = mp:load(APP_DIR .. "/missing.mp3")
    T.ok(ok == nil and type(err) == "string", "load of a missing file returned " .. tostring(ok))
    ok, err = mp:load(APP_DIR .. "/tone.wav")
    T.ok(ok == nil and type(err) == "string", "load of a WAV returned " .. tostring(ok))
end)

T.case("play_after_finish_replays", function()
    local mp = sound.mp3player()
    T.ok(mp:load(APP_DIR .. "/whole.mp3"))
    for round = 1, 2 do
        mp:play()
        T.ok(mp:isPlaying(), "playing right after play() in round " .. round)
        local t0 = sys.getTimeMs()
        while mp:isPlaying() and sys.getTimeMs() - t0 < 4000 do sys.sleep(20) end
        T.ok(not mp:isPlaying(), "round " .. round .. " never finished")
    end
    mp:stop()
end)

T.case("idle_controls_return_at_once", function()
    -- The output must actually be running (a tone keeps it going, so
    -- audio_output_running() is true) or fade_out_and_wait returns at once
    -- regardless of whether anything mixes the MP3, and the test proves
    -- nothing. With the output running, nothing mixes the MP3 in any of
    -- these idle controls, so none may wait for a fade.
    pc.audio.playTone(440)
    local mp = sound.mp3player()

    local t0 = sys.getTimeUs()
    mp:pause()
    mp:stop()
    mp:stop()
    local us1 = sys.getTimeUs() - t0

    T.ok(mp:load(APP_DIR .. "/whole.mp3"))

    local t1 = sys.getTimeUs()
    mp:pause()
    mp:resume()
    local us2 = sys.getTimeUs() - t1

    local us = us1 + us2
    -- Generous bound under -n auto on a loaded host: this catches a hang or
    -- a repeated 50 ms fade-wait timeout (fade_out_and_wait), not scheduler
    -- jitter.
    T.ok(us < 200000, "idle controls took " .. us .. " us")

    pc.audio.stopTone()
    mp:stop()
end)

T.case("position_counts_frames_and_holds_while_paused", function()
    local mp = sound.mp3player()
    T.ok(mp:load(APP_DIR .. "/whole.mp3"))
    mp:play(0)
    sys.sleep(300)
    local p1 = mp:getPosition()
    T.ok(p1 > 0, "position after 300 ms: " .. p1)
    mp:pause()
    local p2 = mp:getPosition()
    sys.sleep(200)
    T.eq(mp:getPosition(), p2, "position moved while paused")
    mp:resume()
    sys.sleep(200)
    T.ok(mp:getPosition() > p2, "position did not move after resume")
    mp:stop()
    T.eq(mp:getPosition(), 0, "position after stop")
end)

T.case("mp3_and_samples_play_together", function()
    local mp = sound.mp3player()
    T.ok(mp:load(APP_DIR .. "/whole.mp3"))
    mp:play(0)
    local s = sound.sample(APP_DIR .. "/tone.wav")
    local p = sound.sampleplayer(s)
    p:play()
    sys.sleep(100)
    T.ok(mp:isPlaying() and p:isPlaying(), "both play")
    local t0 = sys.getTimeMs()
    while p:isPlaying() and sys.getTimeMs() - t0 < 3000 do sys.sleep(20) end
    T.ok(not p:isPlaying(), "the 0.5 s sample never finished")
    T.ok(mp:isPlaying(), "the looping MP3 stopped with the sample")
    mp:stop()
end)

T.done()
