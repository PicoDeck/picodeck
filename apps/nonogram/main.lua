-- Neurogram — cyberpunk nonogram for PicoDeck
--
-- Scene dispatch is hand-rolled rather than using picocalc.game.scene, for two
-- verified reasons (see src/os/lua_bridge_game_scene.c):
--
--   1. l_scene_update and l_scene_draw catch Lua errors, printf them to serial,
--      pop and carry on. On a handheld with no serial attached that turns any
--      bug into a silently black screen. The dispatcher below wraps the frame in
--      one xpcall and shows the traceback on the display.
--
--   2. They only update/draw the TOP of the scene stack, so an overlay cannot
--      render the screen behind it — which a modal panel over the board needs.

local pc     = picocalc
local disp   = pc.display
local input  = pc.input
local gfx    = pc.graphics
local sys    = pc.sys

-- ── Module loader ───────────────────────────────────────────────────────────
-- require/dofile/package are all blocked in the sandbox; this is the in-tree
-- pattern (see apps/calculator/main.lua). Declared GLOBAL so submodules can
-- require each other.

local _loaded = {}
function require(name)
    if _loaded[name] then return _loaded[name] end
    local path = APP_DIR .. "/" .. name:gsub("%.", "/") .. ".lua"
    local src = pc.fs.readFile(path)
    if not src then error("require: file not found: " .. path) end
    local fn, err = load(src, "@" .. path)
    if not fn then error("require: " .. err) end
    local result = fn()
    if result == nil then result = true end
    _loaded[name] = result
    return result
end

local T       = require("theme")
local Render  = require("render")
local Builtin = require("builtin")

Render.init(pc)

-- ── Capability probe ────────────────────────────────────────────────────────
-- The SDK additions this app was built alongside may not be present on older
-- firmware. Probe once and let the modules degrade rather than crash.

local CAP = {
    json      = (type(pc.json) == "table"),
    repeatIn  = (type(input.getButtonsRepeated) == "function"),
    fillHLine = (type(disp.fillHLine) == "function"),
    fillTri   = (type(disp.fillTriangle) == "function"),
}

-- Transparent text has no feature flag: it is a new argument value on an
-- existing function. Probe it by drawing off-screen and checking for a throw.
CAP.textTransparent = pcall(function()
    disp.drawText(-100, -100, " ", T.TEXT, false)
end)

_G.NG_CAP = CAP
sys.log(("NG:CAP json=%s repeat=%s hline=%s tri=%s textT=%s")
    :format(tostring(CAP.json), tostring(CAP.repeatIn),
            tostring(CAP.fillHLine), tostring(CAP.fillTri),
            tostring(CAP.textTransparent)))

-- ── Compatibility shims ─────────────────────────────────────────────────────
--
-- Fill in the missing SDK functions so every call site can use the new API
-- unconditionally. Patching the picocalc tables is safe here: each app runs in a
-- fresh lua_State, so nothing leaks to the next app.
--
-- Without these the app hard-fails on firmware predating the additions, because
-- render.lua draws all of its text with a transparent background.

if not CAP.textTransparent then
    -- Best effort: paint the theme background instead of leaving pixels alone.
    -- Correct wherever text sits on a flat backdrop (which is every case except
    -- clue numbers over a gutter wash, where the wash colour shows as a box).
    local orig = disp.drawText
    disp.drawText = function(x, y, text, fg, bg)
        if bg == false then return orig(x, y, text, fg, T.BG) end
        if bg == nil then return orig(x, y, text, fg) end
        return orig(x, y, text, fg, bg)
    end
end

if not CAP.fillHLine then
    disp.fillHLine = function(y, x0, x1, color)
        if x0 > x1 then x0, x1 = x1, x0 end
        disp.fillRect(x0, y, x1 - x0 + 1, 1, color)
    end
end

if not CAP.fillTri then
    -- Flat-bottom/flat-top decomposition is overkill for what this app draws,
    -- so approximate with horizontal spans between the edges.
    disp.fillTriangle = function(x0, y0, x1, y1, x2, y2, color)
        local minY = math.min(y0, y1, y2)
        local maxY = math.max(y0, y1, y2)
        local function edgeX(ax, ay, bx, by, sy)
            if ay == by then return nil end
            if sy < math.min(ay, by) or sy > math.max(ay, by) then return nil end
            return ax + (bx - ax) * (sy - ay) // (by - ay)
        end
        for sy = minY, maxY do
            local xs = {}
            for _, e in ipairs({ { x0, y0, x1, y1 }, { x1, y1, x2, y2 },
                                 { x2, y2, x0, y0 } }) do
                local xv = edgeX(e[1], e[2], e[3], e[4], sy)
                if xv then xs[#xs + 1] = xv end
            end
            if #xs >= 2 then
                table.sort(xs)
                disp.fillHLine(sy, xs[1], xs[#xs], color)
            end
        end
    end
end

if not CAP.repeatIn then
    -- No shim: pad.lua's dpadEdges() repeats the d-pad with its own
    -- delay/rate timer, as it always does with the gamepad (getButtonsRepeated
    -- follows raw keys, not gamepad buttons), because repeat needs per-button
    -- state that a drop-in wrapper cannot hold correctly.
    sys.log("NG:CAP using hand-rolled key repeat")
end

-- graphics.animation.blinker.new is broken on firmware predating the fix in
-- lua_bridge_graphics.c: it reads its argument count AFTER lua_newuserdata has
-- pushed the object, so the object is counted as a trailing argument and the
-- constructor raises at every arity. Verified on a device running the older
-- build, where it threw "bad argument #4 to 'new' (number expected, got
-- userdata)".
--
-- Probe it and substitute a pure-Lua equivalent when it fails, so the cursor
-- still blinks without requiring a firmware update.
CAP.blinker = pcall(function()
    return gfx.animation.blinker.new(400, 200, true)
end)

if not CAP.blinker then
    local Blinker = {}
    Blinker.__index = Blinker

    function Blinker.new(onMs, offMs, loop)
        return setmetatable({
            onMs = onMs or 500, offMs = offMs or 500,
            loop = (loop ~= false), t0 = sys.getTimeMs(),
        }, Blinker)
    end

    function Blinker:update()
        local period = self.onMs + self.offMs
        if period <= 0 then return true end
        local elapsed = sys.getTimeMs() - self.t0
        if not self.loop and elapsed > period then return false end
        return (elapsed % period) < self.onMs
    end

    function Blinker:start() self.t0 = sys.getTimeMs() end
    function Blinker:stop() end
    function Blinker:isRunning() return true end

    gfx.animation = gfx.animation or {}
    gfx.animation.blinker = gfx.animation.blinker or {}
    gfx.animation.blinker.new = Blinker.new
    sys.log("NG:CAP using Lua blinker fallback")
end

sys.log("NG:CAP blinker=" .. tostring(CAP.blinker))

-- Only the no-gamepad path of pad.lua reads getButtonsRepeated.
if CAP.repeatIn then input.setRepeat(200, 70) end

-- ── Scene dispatcher ────────────────────────────────────────────────────────

local Scenes = { registry = {}, stack = {} }

function Scenes.add(name, scene) Scenes.registry[name] = scene end

local function top() return Scenes.stack[#Scenes.stack] end

function Scenes.switch(name, params)
    local s = assert(Scenes.registry[name], "unknown scene: " .. tostring(name))
    for i = #Scenes.stack, 1, -1 do
        local e = Scenes.stack[i]
        if e.scene.exit then e.scene.exit() end
        Scenes.stack[i] = nil
    end
    Scenes.stack[1] = { scene = s, name = name }
    if s.enter then s.enter(params) end
    sys.log("NG:SCENE " .. name)
end

-- Overlay: the scene underneath stays on screen, which is what game.scene
-- cannot do.
function Scenes.push(name, params)
    local s = assert(Scenes.registry[name], "unknown scene: " .. tostring(name))
    Scenes.stack[#Scenes.stack + 1] = { scene = s, name = name }
    if s.enter then s.enter(params) end
    sys.log("NG:PUSH " .. name)
end

function Scenes.pop()
    local e = table.remove(Scenes.stack)
    if e and e.scene.exit then e.scene.exit() end
    local t = top()
    if t and t.scene.resume then t.scene.resume() end
end

function Scenes.depth() return #Scenes.stack end

_G.NG_SCENES = Scenes

-- ── Error scene ─────────────────────────────────────────────────────────────
-- Shown when a frame raises. Without this the app would just stop drawing.

local errorInfo = nil

local function drawError()
    disp.clear(T.BG)
    disp.fillRect(0, 0, 320, 14, T.ALERT)
    disp.setFont(T.FONT_6X8)
    disp.drawText(4, 3, "NEUROGRAM FAULT", T.BG, T.ALERT)

    disp.setFont(T.FONT_6X8)
    local y = 20
    for line in tostring(errorInfo or "unknown"):gmatch("[^\n]+") do
        if y > 290 then break end
        -- 6px glyphs: 52 characters fit the 320px width.
        for i = 1, #line, 52 do
            if y > 290 then break end
            disp.drawText(3, y, line:sub(i, i + 51), T.TEXT, T.BG)
            y = y + 9
        end
    end
    disp.drawText(4, 302, "ESC exit", T.TEXT_DIM, T.BG)
    disp.flush()
end

-- ── Scene registration ──────────────────────────────────────────────────────

Scenes.add("menu",   require("scene_menu"))
Scenes.add("play",   require("scene_play"))
Scenes.add("browse", require("scene_browse"))
Scenes.add("create", require("scene_create"))

Scenes.switch("menu")

-- ── System menu items (max 4, auto-cleared on exit) ─────────────────────────
-- These fire from the 256-opcode hook, i.e. mid-frame, so they must only set
-- flags — never draw and never open a blocking modal.

local pending = {}
_G.NG_PENDING = pending

sys.clearMenuItems()
sys.addMenuItem("Restart puzzle", function() pending.restart = true end)
sys.addMenuItem("Toggle auto-X",  function() pending.toggleAutoX = true end)
sys.addMenuItem("Main menu",      function() pending.mainMenu = true end)
sys.addMenuItem("Quit",           function() pending.quit = true end)

-- ── Frame loop ──────────────────────────────────────────────────────────────

local quit = false
_G.NG_QUIT = function() quit = true end

local lastMs = sys.getTimeMs()

while not quit do
    pc.perf.beginFrame()
    input.update()

    if errorInfo then
        if input.getButtonsPressed() & input.BTN_ESC ~= 0 then break end
        drawError()
        pc.perf.endFrame()
        goto continue
    end

    do
        local now = sys.getTimeMs()
        local dt = (now - lastMs) / 1000.0
        lastMs = now
        if dt > 0.05 then dt = 0.05 end   -- clamp after a blocking modal

        local ok, err = xpcall(function()
            if pending.quit then quit = true; return end
            if pending.mainMenu then
                pending.mainMenu = nil
                Scenes.switch("menu")
            end

            local t = top()
            if t and t.scene.update then t.scene.update(dt) end

            -- Draw the whole stack bottom-to-top so overlays composite over the
            -- scene behind them.
            for i = 1, #Scenes.stack do
                local e = Scenes.stack[i]
                if e.scene.draw then e.scene.draw(i == #Scenes.stack) end
            end
            disp.flush()
        end, function(e)
            -- The OS requests a clean app exit by RAISING a light-userdata
            -- sentinel as the error object (lua_bridge_raise_exit in
            -- lua_bridge.h) — that is how the Sym menu's Quit, sys.exit() and
            -- the host's `exit` dev command all work. Stringifying it here
            -- destroys its identity, so the app treats "please quit" as a crash,
            -- shows the error scene and never exits; the OS then force-resets
            -- the device. Pass any non-string error object through untouched.
            if type(e) ~= "string" then return e end

            -- No debug.traceback: lua_bridge.c registers only _G, table, string
            -- and math, so `debug` is nil and touching it would raise inside the
            -- message handler ("error in error handling"), hiding the original
            -- error entirely. Same for utf8 and coroutine, which CLAUDE.md lists
            -- as available but are not registered.
            local trace = ""
            if type(debug) == "table" and debug.traceback then
                trace = "\n" .. debug.traceback("", 2)
            end
            return tostring(e) .. trace
        end)

        if not ok then
            -- Re-raise the exit sentinel so it reaches lua_runner.c, which
            -- recognises it and unwinds the app cleanly. level 0 keeps the error
            -- object intact instead of prefixing position information.
            if type(err) ~= "string" then
                sys.clearMenuItems()
                sys.log("NG:EXIT (OS requested)")
                error(err, 0)
            end
            errorInfo = err
            sys.log("NG:ERROR " .. tostring(err):gsub("\n", " | "))
        end
    end

    pc.perf.endFrame()
    ::continue::
end

sys.clearMenuItems()
sys.log("NG:EXIT")
