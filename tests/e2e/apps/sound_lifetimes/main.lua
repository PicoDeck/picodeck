-- Sound object lifetimes (review: Audio/storage High "A sampleplayer doesn't
-- keep a reference to its sample…", Docs-vs-implementation volume range).
-- picotest kit.
--
-- The harness stages short.wav (0.05 s) and long.wav (0.2 s), both mono
-- 16-bit 22050 Hz, and runs every case ALONE in a fresh simulator (only.flag
-- names it), under the default collector and under a very aggressive
-- incremental one (stress.flag). Before the fix a case can be a
-- use-after-free (the simulator's Core 1 thread mixes the freed sample), so
-- one case's crash must not cost the others their result. The ASan
-- simulator reports the bad access; the release simulator sees the weak-table
-- survival checks, the identity checks and the slot counts.
--
-- Samples have no fixed limit; the driver has SOUND_MAX_PLAYERS = 8 player
-- slots, so a leaked player slot shows up as a failed create within a few
-- iterations.
-- See tests/e2e/test_lifetimes_sound.py.

local pc = picocalc
local sound = pc.sound
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local SHORT = APP_DIR .. "/short.wav"   -- 0.05 s = 1102 frames
local LONG = APP_DIR .. "/long.wav"     -- 0.2 s  = 4410 frames
local SLOTS = 8

local STRESS = fs.exists(APP_DIR .. "/stress.flag")
local ONLY = fs.exists(APP_DIR .. "/only.flag") and fs.readFile(APP_DIR .. "/only.flag")
if ONLY then
    local case = T.case
    T.case = function(name, fn)
        if name == ONLY then return case(name, fn) end
    end
end
if STRESS then
    collectgarbage("incremental", 0, 1000)
end
pc.sys.log("SL:MODE " .. (STRESS and "stress" or "normal"))

local function collect()
    collectgarbage("collect")
    collectgarbage("collect")
end

-- Collect ten times and churn ~1 MB so freed blocks get reused, letting the
-- mixer run in between.
local function churn()
    for _ = 1, 10 do collectgarbage("collect") end
    local junk = {}
    for i = 1, 1000 do junk[i] = string.rep("j", 1000) .. i end
    junk = nil
    collectgarbage("collect")
    pc.sys.sleep(20)
end

local function weak_ref(v)
    return setmetatable({ v }, { __mode = "v" })
end

local function survived(weak, what)
    T.ok(weak[1] ~= nil, what .. " was collected while still referenced from C")
end

-- Every sample and player slot is free again: SLOTS path samples and SLOTS
-- players can be created at once.
local function all_slots_free(what)
    collect()
    local held = {}
    for i = 1, SLOTS do
        local s, err = sound.sample(SHORT)
        T.ok(s, what .. ": sample slot " .. i .. " leaked (" .. tostring(err) .. ")")
        held[#held + 1] = s
        local p, perr = sound.sampleplayer()
        T.ok(p, what .. ": player slot " .. i .. " leaked (" .. tostring(perr) .. ")")
        held[#held + 1] = p
    end
    held = nil
    collect()
end

local function heap_free()
    collect()
    return pc.sys.getMemInfo().psram_free
end

T.case("path_player_leak_loop", function()
    local base = heap_free()
    for i = 1, 100 do
        local p, err = sound.sampleplayer(SHORT)
        T.ok(p, "iteration " .. i .. ": " .. tostring(err))
        p:play()
        p = nil
        collect()
    end
    all_slots_free("after 100 sampleplayer(path)")
    local lost = base - heap_free()
    T.ok(lost < 32 * 1024, ("100 sampleplayer(path) lost %d bytes of heap"):format(lost))
end)

T.case("path_player_reseat_leak_loop", function()
    -- A path player that is given another sample must still let its
    -- path-loaded sample go (it used to be leaked until the app exited).
    for i = 1, 20 do
        local p, err = sound.sampleplayer(SHORT)
        T.ok(p, "iteration " .. i .. ": " .. tostring(err))
        p:setSample(T.ok(sound.sample(0.01), "blank sample " .. i))
        p = nil
        collect()
    end
    all_slots_free("after 20 reseated sampleplayer(path)")
end)

T.case("gc_while_playing", function()
    local p = T.ok(sound.sampleplayer(), "player")
    local weak
    do
        local s = T.ok(sound.sample(LONG), "sample")
        weak = weak_ref(s)
        p:setSample(s)
    end
    p:play(0)                      -- loop forever: the mixer keeps reading
    for _ = 1, 3 do churn() end
    T.ok(p:isPlaying(), "player stopped")
    T.eq(p:getLength(), 4410, "length")
    survived(weak, "the sample of a playing sampleplayer")
    p:stop()
end)

T.case("sample_play_keeps_sample", function()
    local weak
    local p
    do
        local s = T.ok(sound.sample(LONG), "sample")
        weak = weak_ref(s)
        p = T.ok(s:play(0), "sample:play")
    end
    for _ = 1, 3 do churn() end
    T.ok(p:isPlaying(), "player stopped")
    T.eq(p:getLength(), 4410, "length")
    survived(weak, "the sample behind sample:play()")
    p:stop()
end)

T.case("collect_player_and_sample_while_playing", function()
    -- Both die in the same cycle; the sample is newer, so its finalizer
    -- runs first while the player is still mixing it. The sample must
    -- detach the player before its data goes.
    for i = 1, 30 do
        local p = T.ok(sound.sampleplayer(), "player " .. i)
        local s = T.ok(sound.sample(LONG), "sample " .. i)
        p:setSample(s)
        p:play(0)
        pc.sys.sleep(2)
        p, s = nil, nil
        collect()
        pc.sys.sleep(2)
    end
    T.eq(sound.playingSources(), 0, "players still playing")
    all_slots_free("after 30 collections while playing")
end)

T.case("empty_players_are_distinct", function()
    -- Players created without a sample used to share one slot.
    local a = T.ok(sound.sampleplayer(), "player a")
    local b = T.ok(sound.sampleplayer(), "player b")
    a:setSample(T.ok(sound.sample(SHORT), "short"))
    b:setSample(T.ok(sound.sample(LONG), "long"))
    T.eq(a:getLength(), 1102, "a length")
    T.eq(b:getLength(), 4410, "b length")
    b = nil
    collect()
    T.eq(a:getLength(), 1102, "a length after b was collected")
    a:play(0)
    T.ok(a:isPlaying(), "a plays after b was collected")
    a:stop()
end)

T.case("getSample_returns_the_sample", function()
    local s = T.ok(sound.sample(SHORT), "sample")
    local p = T.ok(sound.sampleplayer(s), "player")
    T.eq(p:getSample(), s, "getSample")
    local q = T.ok(sound.sampleplayer(LONG), "path player")
    local qs = q:getSample()
    T.eq(qs:getLength(), 4410, "path sample length")
    p:setSample(qs)
    T.eq(p:getLength(), 4410, "reseated length")
    T.eq(sound.sampleplayer():getSample(), nil, "empty player")
end)

T.case("finish_callback_slots_recycled", function()
    for i = 1, 20 do
        local p = T.ok(sound.sampleplayer(), "player " .. i)
        local ok, err = pcall(p.setFinishCallback, p, function() end)
        T.ok(ok, "iteration " .. i .. ": " .. tostring(err))
        ok, err = pcall(p.setLoopCallback, p, function() end)
        T.ok(ok, "iteration " .. i .. ": " .. tostring(err))
        p = nil
        collect()
    end
end)

T.case("volume_range_0_100", function()
    local sp = T.ok(sound.sampleplayer(SHORT), "sampleplayer")
    sp:setVolume(300); T.eq(sp:getVolume(), 100, "sampleplayer 300")
    sp:setVolume(-5);  T.eq(sp:getVolume(), 0, "sampleplayer -5")
    sp:setVolume(42);  T.eq(sp:getVolume(), 42, "sampleplayer 42")

    local fp = T.ok(sound.fileplayer(), "fileplayer")
    fp:setVolume(300); T.eq(fp:getVolume(), 100, "fileplayer 300")
    fp:setVolume(-5);  T.eq(fp:getVolume(), 0, "fileplayer -5")
    fp:setVolume(42);  T.eq(fp:getVolume(), 42, "fileplayer 42")
    fp:setVolume(150, 200)
    local l, r = fp:getVolume()
    T.eq(l, 100, "fileplayer left 150")
    T.eq(r, 100, "fileplayer right 200")

    local mp = T.ok(sound.mp3player(), "mp3player")
    mp:setVolume(300); T.eq(mp:getVolume(), 100, "mp3player 300")
    mp:setVolume(42);  T.eq(mp:getVolume(), 42, "mp3player 42")
end)

T.case("samples_are_not_capped_players_are", function()
    local held = {}
    for i = 1, 40 do
        local s, err = sound.sample(SHORT)
        T.ok(s, "sample " .. i .. " (" .. tostring(err) .. ")")
        held[i] = s
    end
    local players = {}
    for i = 1, SLOTS do
        players[i] = sound.sampleplayer(held[i])
        T.ok(players[i], "player " .. i)
    end
    local extra, err = sound.sampleplayer(held[SLOTS + 1])
    T.ok(extra == nil, "a 9th sampleplayer was created")
    T.ok(err and err:find("8"), "the error names the limit: " .. tostring(err))
    held, players, extra = nil, nil, nil
    all_slots_free("after 40 samples")
end)

T.done()
