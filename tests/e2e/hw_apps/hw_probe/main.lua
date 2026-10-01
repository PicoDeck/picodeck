-- Hardware-only probes (tests/e2e/test_hw_device.py, --target hw).
--
-- Not staged on simulator SD cards (it lives in tests/e2e/hw_apps/, not
-- apps/); the hardware backend pushes it once per session. The test writes
-- /data/com.test.hw_probe/mode.json ({"mode": "color"|"cstack"|"keys"|
-- "audio"}) before each launch, and reads the verdict back from the picotest
-- results file, the channel that survives serial capture dropping [APP] lines.
--
-- color, cstack and audio hold after T.done() so the host can take a
-- screenshot, ask the dev console for `stack`, or capture [MOD] serial lines
-- while the app still runs; the host's `exit` ends the hold.

local pc = picocalc
local d = pc.display
local input = pc.input
local T = pc.sys.loadlib("picotest")

local function now() return pc.sys.getTimeMs() end

local cfg = pc.json.decode(pc.fs.readFile("/data/" .. APP_ID .. "/mode.json"))
local mode = cfg.mode

d.clear(d.BLACK)
d.drawText(4, 300, "hw_probe: " .. tostring(mode), d.GRAY or d.WHITE)
d.flush()

-- Keep the app alive (and the panel lit: idle dim swallows a wake key)
-- until the host sends `exit`, which raises the exit sentinel in sleep.
local function hold(ms)
    local deadline = now() + (ms or 60000)
    while now() < deadline do
        pc.sys.resetIdleTimer()
        pc.sys.sleep(50)
    end
end

-- ── color: display.c byte order ─────────────────────────────────────────────
-- The same blocks as apps/color_test (test_display_colors.py checks them in
-- the simulator); here the host reads them back from the device framebuffer.
local function mode_color()
    d.clear(d.BLACK)
    d.fillRect(0, 0, 100, 100, d.RED)
    d.fillRect(110, 0, 100, 100, d.GREEN)
    d.fillRect(220, 0, 100, 100, d.BLUE)
    d.fillRect(0, 110, 100, 100, d.WHITE)
    d.fillRect(110, 110, 100, 100, d.YELLOW)
    -- x=220-319, y=110-209 stays black
    d.flush()
    T.case("drawn", function() end)
    T.done()
    hold()
end

-- ── cstack: Lua -> C -> Lua recursion on the real app stack ────────────────
-- Then unbounded pure-Lua recursion, ~976 frames deep at its "stack
-- overflow": the timer-armed service hook used to walk every frame on each
-- 1 ms tick there, never left the timer IRQ and the watchdog reset the
-- device (issue #21). A hang shows in test_results.json as the cases before
-- the one that hung.
local function gsub_to(n)
    if n == 0 then return "x" end
    return (string.gsub("a", "a", function() return gsub_to(n - 1) end))
end

local function index_chain(depth)
    local cur = { v = 42 }
    for _ = 1, depth do
        local nxt = cur
        cur = setmetatable({}, { __index = function(_, k) return nxt[k] end })
    end
    return cur
end

local function mode_cstack()
    T.case("gsub_40_deep", function()
        T.eq(gsub_to(40), "x")
    end)
    T.case("gsub_runaway_is_c_stack_overflow", function()
        local function grec(n)
            return (string.gsub("a", "a", function() return grec(n + 1) end))
        end
        local ok, err = pcall(grec, 1)
        T.eq(ok, false)
        T.ok(tostring(err):find("C stack overflow"), tostring(err))
    end)
    T.case("index_chain_40", function()
        T.eq(index_chain(40).v, 42)
    end)
    T.case("index_chain_250_is_c_stack_overflow", function()
        local ok, err = pcall(function() return index_chain(250).v end)
        T.eq(ok, false, "250-deep __index chain returned " .. tostring(err))
        T.ok(tostring(err):find("stack overflow"), tostring(err))
    end)
    T.case("lua_recursion_is_stack_overflow", function()
        local depth = 0
        local function recurse()
            depth = depth + 1
            return 1 + recurse()
        end
        local ok, err = pcall(recurse)
        T.eq(ok, false)
        T.ok(tostring(err):find("stack overflow"), tostring(err))
    end)
    T.done()
    hold()
end

-- ── keys: keyboard.c edges for injected taps and a chord ───────────────────
-- The host waits for case "ready", then injects: a, enter, ctrl+x, z.
local KEY_ENTER, KEY_CTRL = 0x0A, 0xA5

local function fmt(e)
    return string.format("%s:%d%s%s", e.type, e.key,
        e.char and ("'" .. e.char .. "'") or "", e["repeat"] and ":R" or "")
end

local function mode_keys()
    input.clearState()
    T.case("ready", function() end)
    local events, ctrl_down_seen, finished = {}, false, false
    local deadline = now() + 30000
    while now() < deadline and not finished do
        pc.sys.resetIdleTimer()
        input.update()
        while true do
            local e = input.pollEvent()
            if not e then break end
            events[#events + 1] = e
            if e.type == "char" and e.char == "z" then finished = true end
        end
        if input.isKeyDown(KEY_CTRL) then ctrl_down_seen = true end
        pc.sys.sleep(10)
    end
    local all = {}
    for i, e in ipairs(events) do all[i] = fmt(e) end
    local dump = " events=[" .. table.concat(all, " ") .. "]"

    local function of_key(k)
        local out = {}
        for _, e in ipairs(events) do
            if e.key == k then out[#out + 1] = fmt(e) end
        end
        return table.concat(out, " ")
    end
    local function index_of(pred)
        for i, e in ipairs(events) do if pred(e) then return i end end
    end

    T.case("sentinel_arrived", function() T.ok(finished, "no 'z'" .. dump) end)
    T.case("tap_is_one_down_char_up", function()
        T.eq(of_key(97), "down:97 char:97'a' up:97", "a" .. dump)
    end)
    T.case("button_tap_is_one_down_up", function()
        T.eq(of_key(KEY_ENTER), "down:10 up:10", "enter" .. dump)
    end)
    T.case("chord_orders_modifier_around_key", function()
        T.eq(of_key(KEY_CTRL), "down:165 up:165", "ctrl" .. dump)
        T.ok(ctrl_down_seen, "isKeyDown(ctrl) never true" .. dump)
        local cd = index_of(function(e) return e.key == KEY_CTRL and e.type == "down" end)
        local xc = index_of(function(e) return e.type == "char" and e.char == "x" end)
        local cu = index_of(function(e) return e.key == KEY_CTRL and e.type == "up" end)
        T.ok(cd and xc and cu and cd < xc and xc < cu,
             "ctrl down < x < ctrl up" .. dump)
    end)
    T.case("no_repeat_edges_for_taps", function()
        for _, e in ipairs(events) do
            T.ok(not e["repeat"], "repeat event " .. fmt(e) .. dump)
        end
    end)
    T.done()
end

-- ── audio: stream underruns while the MOD player runs (host reads [MOD]) ────
local function mode_audio()
    local mp = pc.modplayer.create()
    T.case("mod_loads", function()
        T.ok(mp:load(pc.fs.appPath("silence.mod")), "load failed")
    end)
    mp:play(true)
    T.case("mod_plays", function()
        pc.sys.sleep(300)
        T.ok(mp:isPlaying(), "not playing")
    end)
    T.done()
    hold()
    mp:stop()
end

local modes = { color = mode_color, cstack = mode_cstack, keys = mode_keys,
                audio = mode_audio }
T.case("mode_known", function() T.ok(modes[mode], "unknown mode " .. tostring(mode)) end)
if modes[mode] then modes[mode]() else T.done() end
