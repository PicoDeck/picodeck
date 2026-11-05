-- Fire-and-forget sound effects: sample:play() and sample:playAt() with the
-- result dropped, what they hold on to and when they let go. picotest kit.
--
-- The harness stages short.wav (0.05 s) and long.wav (0.25 s), both mono
-- 16-bit 22050 Hz, and runs every case ALONE in a fresh simulator (only.flag
-- names it), so a bad voice count cannot spill into the next case. The same
-- cases run on the device's own heap (--real-umm) and on the device
-- (tests/e2e/test_sound_sfx.py).

local pc = picocalc
local sound = pc.sound
local fs = pc.fs
local sys = pc.sys
local T = sys.loadlib("picotest")

local SHORT = APP_DIR .. "/short.wav"   -- 0.05 s = 1102 frames
local LONG = APP_DIR .. "/long.wav"     -- 0.25 s = 5512 frames
local VOICES = 8

local function collect()
    collectgarbage("collect")
    collectgarbage("collect")
end

-- One case (only.flag), or all of them with nothing left over from the last.
local ONLY = fs.exists(APP_DIR .. "/only.flag") and fs.readFile(APP_DIR .. "/only.flag")
local case = T.case
T.case = function(name, fn)
    if ONLY and name ~= ONLY then return end
    return case(name, function()
        collectgarbage("restart")
        collect()
        for _ = 1, 400 do
            if sound.playingSources() == 0 then break end
            sys.sleep(5)
        end
        fn()
    end)
end

local function weak_ref(v)
    return setmetatable({ v }, { __mode = "v" })
end

-- Runs until pred() holds (the service pass runs inside sys.sleep).
local function wait_until(pred, ms, what)
    local t0 = sys.getTimeMs()
    while not pred() do
        if sys.getTimeMs() - t0 > ms then
            error("timed out after " .. ms .. " ms: " .. what, 2)
        end
        sys.sleep(5)
    end
    return sys.getTimeMs() - t0
end

local function silence_done(ms)
    wait_until(function() return sound.playingSources() == 0 end, ms or 5000,
               "sounds still playing")
    sys.sleep(30)   -- the service pass that frees the voices
end

-- Eight new sampleplayers fit: nothing holds a voice. They are dropped again.
local function all_voices_free(what)
    local held = {}
    for i = 1, VOICES do
        local p, err = sound.sampleplayer()
        T.ok(p, what .. ": voice " .. i .. " still taken (" .. tostring(err) .. ")")
        held[i] = p
    end
end

-- ── The two ways the obvious call used to fail ──────────────────────────────

T.case("no_garbage_no_raise", function()
    -- With the collector off nothing can free a voice but the finish itself.
    local s = T.ok(sound.sample(SHORT), "sample")
    collectgarbage("stop")
    for i = 1, 60 do
        local ok, p, err = pcall(s.play, s)
        T.ok(ok, "shot " .. i .. " raised: " .. tostring(p))
        T.ok(p, "shot " .. i .. " got no voice: " .. tostring(err))
        sys.sleep(20)   -- 2.5 shots overlap: never near 8 voices
    end
    collectgarbage("restart")
end)

T.case("playAt_no_garbage_no_raise", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    collectgarbage("stop")
    for i = 1, 60 do
        local ok, p, err = pcall(s.playAt, s, 0, 50)
        T.ok(ok, "shot " .. i .. " raised: " .. tostring(p))
        T.ok(p, "shot " .. i .. " got no voice: " .. tostring(err))
        sys.sleep(20)
    end
    collectgarbage("restart")
end)

T.case("collector_does_not_cut_a_playing_sound", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local t0 = sys.getTimeMs()
    s:play()
    collect()
    T.eq(sound.playingSources(), 1, "playing right after a full collection")
    while sound.playingSources() > 0 and sys.getTimeMs() - t0 < 5000 do
        collect()
        sys.sleep(5)
    end
    local lasted = sys.getTimeMs() - t0
    T.ok(lasted >= 200, "a 250 ms sound was cut off after " .. lasted .. " ms")
    T.ok(lasted < 5000, "the sound never finished")
end)

T.case("playAt_collector_does_not_cut", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local t0 = sys.getTimeMs()
    s:playAt(0, 40)
    while sound.playingSources() > 0 and sys.getTimeMs() - t0 < 5000 do
        collect()
        sys.sleep(5)
    end
    local lasted = sys.getTimeMs() - t0
    T.ok(lasted >= 200, "a 250 ms sound was cut off after " .. lasted .. " ms")
    T.ok(lasted < 5000, "the sound never finished")
end)

T.case("sample_can_be_dropped_too", function()
    -- The playing temporary keeps its sample: drop both.
    local t0 = sys.getTimeMs()
    do
        local s = T.ok(sound.sample(LONG), "sample")
        s:play()
    end
    while sound.playingSources() > 0 and sys.getTimeMs() - t0 < 5000 do
        collect()
        sys.sleep(5)
    end
    local lasted = sys.getTimeMs() - t0
    T.ok(lasted >= 200, "cut off after " .. lasted .. " ms")
end)

T.case("finished_sound_is_collected", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local weak = weak_ref(s:play())
    collect()
    T.ok(weak[1], "collected while it played")
    silence_done()
    collect()
    T.ok(weak[1] == nil, "the player was not collected after it finished")
    all_voices_free("after one finished shot")
end)

-- ── Voices ──────────────────────────────────────────────────────────────────

T.case("a_finished_player_gives_its_voice_back_even_if_referenced", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local kept = {}
    for i = 1, VOICES do
        kept[i] = T.ok(s:play(), "shot " .. i)
    end
    silence_done()
    for i = 1, VOICES do
        T.eq(kept[i]:isPlaying(), false, "kept " .. i .. " isPlaying")
    end
    -- Eight kept, finished players do not block new sounds.
    local more = {}
    for i = 1, VOICES do
        more[i] = T.ok(s:play(0), "looping shot " .. i)
    end
    T.eq(sound.playingSources(), VOICES, "playing")
    for i = 1, VOICES do more[i]:stop() end
end)

T.case("all_voices_playing_returns_nil_err", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local kept = {}
    for i = 1, VOICES do
        kept[i] = T.ok(s:play(0), "shot " .. i)
    end
    local ok, p, err = pcall(s.play, s)
    T.ok(ok, "play raised: " .. tostring(p))
    T.ok(p == nil, "a 9th sound got a voice")
    T.ok(type(err) == "string" and err:find("8"), "error names the limit: " .. tostring(err))
    ok, p, err = pcall(s.playAt, s, 0, 50)
    T.ok(ok, "playAt raised: " .. tostring(p))
    T.ok(p == nil, "a 9th playAt got a voice")
    T.ok(type(err) == "string" and err:find("8"), "error names the limit: " .. tostring(err))
    for i = 1, VOICES do kept[i]:stop() end
    silence_done()
    T.ok(s:play(), "a voice again once they stopped")
end)

T.case("sampleplayers_hold_voices_while_referenced", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local kept = {}
    for i = 1, VOICES do
        kept[i] = T.ok(sound.sampleplayer(s), "player " .. i)
    end
    local p, err = s:play()
    T.ok(p == nil, "a sound got the voice of a live sampleplayer")
    T.ok(err and err:find("8"), "error names the limit: " .. tostring(err))
    local extra, xerr = sound.sampleplayer(s)
    T.ok(extra == nil, "a 9th sampleplayer")
    T.ok(xerr and xerr:find("8"), "error names the limit: " .. tostring(xerr))
end)

T.case("dropped_sampleplayers_are_collected_for_a_voice", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    collectgarbage("stop")
    for i = 1, VOICES do
        T.ok(sound.sampleplayer(s), "player " .. i)
    end
    -- All eight are garbage but nothing has collected them.
    T.ok(s:play(), "no voice although eight sampleplayers were garbage")
    collectgarbage("restart")
end)

T.case("a_finished_shot_does_not_block_a_sampleplayer", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    collectgarbage("stop")
    local kept = {}
    for i = 1, VOICES do kept[i] = T.ok(s:play(), "shot " .. i) end
    wait_until(function() return sound.playingSources() == 0 end, 5000, "still playing")
    -- No service pass has to come first: creating one looks for itself.
    local p, err = sound.sampleplayer(s)
    T.ok(p, "sampleplayer: " .. tostring(err))
    collectgarbage("restart")
end)

-- ── The returned player stays a player ──────────────────────────────────────

T.case("a_finished_player_still_answers", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(s:playAt(0, 40, 0, 2.0), "playAt")
    silence_done()
    all_voices_free("after it finished")
    T.eq(p:isPlaying(), false, "isPlaying")
    T.eq(p:getVolume(), 40, "getVolume")
    T.eq(p:getRate(), 2.0, "getRate")
    T.eq(p:getLength(), 1102, "getLength")
    T.eq(p:getSample(), s, "getSample")
    T.eq(p:getOffset(), 0.0, "getOffset")
    p:setVolume(10)
    T.eq(p:getVolume(), 10, "getVolume after setVolume")
    p:setRate(1.0)
    T.eq(p:getRate(), 1.0, "getRate after setRate")
    p:stop()
    T.eq(p:isPlaying(), false, "isPlaying after stop")
end)

T.case("a_finished_player_replays", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(s:play(), "play")
    p:setVolume(33)
    silence_done()
    for round = 1, 3 do
        T.eq(p:play(), true, "replay " .. round)
        T.eq(p:isPlaying(), true, "isPlaying, round " .. round)
        T.eq(sound.playingSources(), 1, "on a voice, round " .. round)
        T.eq(p:getVolume(), 33, "volume kept, round " .. round)
        silence_done()
    end
end)

T.case("replay_with_no_voice_returns_nil_err", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(s:play(), "play")
    silence_done()
    local held = {}
    for i = 1, VOICES do held[i] = T.ok(sound.sampleplayer(s), "player " .. i) end
    local ok, err = p:play()
    T.ok(ok == nil, "played with no voice")
    T.ok(err and err:find("8"), "error names the limit: " .. tostring(err))
    T.eq(p:isPlaying(), false, "isPlaying")
    held = nil
    collect()
    T.eq(p:play(), true, "plays once a voice is free")
end)

T.case("setSample_on_a_finished_player", function()
    local a = T.ok(sound.sample(SHORT), "short")
    local b = T.ok(sound.sample(LONG), "long")
    local p = T.ok(a:play(), "play")
    silence_done()
    T.eq(p:setSample(b), true, "setSample")
    T.eq(p:getLength(), 5512, "length of the new sample")
    T.eq(p:getSample(), b, "getSample")
    T.eq(p:play(), true, "play")
    T.eq(sound.playingSources(), 1, "playing")
    p:stop()
end)

T.case("a_sample_replaced_on_a_finished_player_is_let_go", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local weak
    local p
    do
        local first = T.ok(sound.sample(SHORT), "first")
        weak = weak_ref(first)
        p = T.ok(first:play(), "play")
    end
    silence_done()
    collect()
    T.ok(weak[1], "the player's sample was collected")
    T.eq(p:getLength(), 1102, "length")
    p:setSample(s)
    collect()
    T.ok(weak[1] == nil, "the replaced sample was kept")
    T.eq(p:getLength(), 1102, "length of the new sample")
end)

T.case("looping_sound_lives_until_stopped", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local weak = weak_ref(s:play(0))
    for _ = 1, 5 do
        collect()
        sys.sleep(20)
    end
    T.ok(weak[1], "a looping sound was collected")
    T.eq(weak[1]:isPlaying(), true, "isPlaying")
    weak[1]:stop()
    silence_done()
    collect()
    T.ok(weak[1] == nil, "not collected after stop")
    all_voices_free("after the looping sound stopped")
end)

T.case("paused_player_holds_no_voice", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local p = T.ok(s:play(), "play")
    sys.sleep(20)
    p:setPaused(true)
    sys.sleep(30)
    T.eq(p:isPlaying(), true, "isPlaying while paused")
    local where = p:getOffset()
    T.ok(where > 0, "position while paused: " .. tostring(where))
    do
        local held = {}
        for i = 1, VOICES do held[i] = T.ok(sound.sampleplayer(s), "player " .. i) end
    end
    collect()
    p:setPaused(false)
    T.eq(sound.playingSources(), 1, "playing again")
    T.ok(p:getOffset() >= where, "resumed from where it paused")
    local lasted = wait_until(function() return not p:isPlaying() end, 5000, "resumed sound")
    T.ok(lasted >= 100, "resumed sound ended after " .. lasted .. " ms")
end)

T.case("a_dropped_paused_player_is_collected", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local weak
    do
        local p = T.ok(s:play(), "play")
        weak = weak_ref(p)
        p:setPaused(true)
    end
    sys.sleep(30)
    collect()
    T.ok(weak[1] == nil, "a dropped paused sound stayed alive")
    all_voices_free("after a dropped paused sound")
end)

T.case("resume_with_no_voice_returns_nil_err", function()
    local s = T.ok(sound.sample(LONG), "sample")
    local p = T.ok(s:play(), "play")
    p:setPaused(true)
    sys.sleep(30)
    local held = {}
    for i = 1, VOICES do held[i] = T.ok(sound.sampleplayer(s), "player " .. i) end
    local r, err = p:setPaused(false)
    T.ok(r == nil and err and err:find("8"), "no voice: " .. tostring(err))
    T.eq(p:isPlaying(), true, "still paused, not lost")
    held = nil
    collect()
    p:setPaused(false)
    T.eq(sound.playingSources(), 1, "playing once a voice is free")
    p:stop()
end)

T.case("finish_callback_fires_after_the_player_was_dropped", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local fired = 0
    for i = 1, 20 do
        do
            local p = T.ok(s:play(), "shot " .. i)
            p:setFinishCallback(function() fired = fired + 1 end)
        end
        collect()
        wait_until(function() return fired == i end, 5000, "finish callback " .. i)
    end
    silence_done()
    all_voices_free("after 20 shots with callbacks")
end)

T.case("many_finished_players_with_callbacks", function()
    -- A callback slot goes with the voice, or the ninth kept, finished player
    -- with a callback would raise "too many sampleplayer finish callbacks".
    local s = T.ok(sound.sample(SHORT), "sample")
    local kept, fired = {}, 0
    collectgarbage("stop")
    for i = 1, 20 do
        local p = T.ok(s:play(), "shot " .. i)
        p:setFinishCallback(function() fired = fired + 1 end)
        p:setLoopCallback(function() end)
        kept[i] = p
        sys.sleep(70)   -- each finishes before the next
    end
    silence_done()
    T.eq(fired, 20, "finish callbacks")
    collectgarbage("restart")
end)

-- Every play reaps, and the pump hands callbacks over on the other core meanwhile:
-- a park landing in the middle of that lost the callback.
T.case("no_callback_is_lost_to_a_reap", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local fired, played = 0, 0
    local t0 = sys.getTimeMs()
    while played < 240 and sys.getTimeMs() - t0 < 20000 do
        local p = s:play()
        if p then
            played = played + 1
            p:setFinishCallback(function() fired = fired + 1 end)
        end
        sys.sleep(1)
    end
    wait_until(function() return fired == played end, 5000,
               "fired " .. fired .. " of " .. played)
    T.ok(played > 0, "played")
end)

T.case("callback_survives_a_replay", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(s:play(), "play")
    local fired = 0
    p:setFinishCallback(function() fired = fired + 1 end)
    wait_until(function() return fired == 1 end, 5000, "first finish")
    sys.sleep(30)
    T.eq(p:play(), true, "replay")
    wait_until(function() return fired == 2 end, 5000, "second finish")
    sys.sleep(30)
    -- Set while it holds no voice, cleared while it holds no voice.
    p:setFinishCallback(function() fired = fired + 100 end)
    T.eq(p:play(), true, "third play")
    wait_until(function() return fired == 102 end, 5000, "third finish")
    sys.sleep(30)
    p:setFinishCallback(nil)
    T.eq(p:play(), true, "fourth play")
    silence_done()
    T.eq(fired, 102, "a cleared callback fired")
end)

T.case("a_callback_can_play_again", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local plays = 0
    local p = T.ok(s:play(), "play")
    p:setFinishCallback(function()
        plays = plays + 1
        if plays < 3 then p:play() end
    end)
    wait_until(function() return plays == 3 end, 5000, "three plays")
    silence_done()
    all_voices_free("after replays from the callback")
end)

T.case("a_callback_can_fire_another_shot", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local shots = 0
    local function shot()
        shots = shots + 1
        if shots <= 5 then
            local p = T.ok(s:play(), "shot " .. shots)
            p:setFinishCallback(shot)
        end
    end
    shot()
    wait_until(function() return shots > 5 end, 5000, "chain of shots")
    silence_done()
    all_voices_free("after a chain of shots")
end)

T.case("nothing_to_play_returns_nil_err", function()
    local empty = T.ok(sound.sample(), "empty sample")
    local p, err = empty:play()
    T.ok(p == nil and err, "sample:play with no data: " .. tostring(err))
    p, err = empty:playAt(0)
    T.ok(p == nil and err, "sample:playAt with no data: " .. tostring(err))
    all_voices_free("after nothing played")
end)

T.case("a_finaliser_can_reach_a_finished_player", function()
    -- Finalisers run newest first. The sample set last dies first, the holder
    -- runs next and reaches the finished player, whose own finaliser has not
    -- run yet: it must find the sample gone, not read freed memory.
    local result
    do
        local a = T.ok(sound.sample(SHORT), "a")
        local p = T.ok(a:play(), "play")
        wait_until(function() return not p:isPlaying() end, 5000, "the shot")
        sys.sleep(30)
        local holder = setmetatable({ p = p }, { __gc = function(self)
            result = { pcall(function()
                return self.p:getLength(), self.p:getSample()
            end) }
        end })
        p:setSample(T.ok(sound.sample(SHORT), "newer sample"))
        holder, a, p = nil, nil, nil
    end
    collect()
    collect()
    T.ok(result, "the finaliser ran")
    T.eq(result[1], true, "the method raised: " .. tostring(result[2]))
    T.eq(result[2], 0, "length of a freed sample")
    T.eq(result[3], nil, "getSample of a freed sample")
end)

T.case("left_playing_at_exit", function()
    -- The harness checks a clean exit; ASan sees teardown touching freed data.
    local s = T.ok(sound.sample(LONG), "sample")
    s:play(0)
    s:playAt(0, 30)
    local p = T.ok(s:play(), "kept")
    p:setPaused(true)
    sys.sleep(30)
    T.ok(s:play(), "one more")
end)

T.done()
