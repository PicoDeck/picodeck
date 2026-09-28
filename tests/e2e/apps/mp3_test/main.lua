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

T.done()
