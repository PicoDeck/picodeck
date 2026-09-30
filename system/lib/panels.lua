-- panels.lua — declarative interactive-comics framework for PicoDeck
--
-- Modelled on the Playdate "Panels" library (github.com/cadin/panels): a comic
-- is a plain Lua data table — sequences of panels of layers — and the library
-- owns scrolling, parallax, animation, audio cues, transitions, branching and
-- progress save. Load with:
--
--     local Panels = picocalc.sys.loadlib("panels")
--     Panels.start(comicData)                -- blocking, returns on exit
--
-- or embed in an existing frame loop:
--
--     local comic = Panels.new(comicData, opts)
--     while not comic:finished() do
--         picocalc.input.update()
--         comic:update()                     -- draws; does NOT full-flush
--         if comic.drewThisFrame ~= false then
--             picocalc.display.flush()       -- false = comic flushed its own
--         end                                -- strips (hardware scroll path)
--     end
--
-- PicoCalc deltas from Playdate Panels (documented in docs/Library-Panels.md):
-- no crank (D-pad scroll/advance), colour RGB565, opacity is binary (no alpha
-- blit), transitions are fade-to-colour only (no framebuffer capture yet).
--
-- Comic content must be pure data. This library never load()s strings from the
-- comic table and resolves every asset through a source object that rejects
-- path escapes — keep it that way.

local pc    = picocalc
local disp  = pc.display
local gfx   = pc.graphics
local input = pc.input
local sys   = pc.sys
local fs    = pc.fs

local Panels = {}

-- ── Capability probes ───────────────────────────────────────────────────────
-- The clip rect and partial flush are SDK additions that ship alongside this
-- library; probe so the library degrades (layers overhang their panels) rather
-- than crashes on older firmware. Same pattern as apps/nonogram.

local CAP = {
    clip      = type(disp.setClipRect) == "function"
                and type(disp.getClipRect) == "function",
    preload   = type(gfx.image.preload) == "function"
                and type(gfx.image.pollPreload) == "function",
    save      = type(pc.game) == "table" and type(pc.game.save) == "table",
    sound     = type(pc.sound) == "table",
    -- LCD hardware scroll (VSCRDEF/VSCRSADD ring) + partial flush: the
    -- fast path for rigid vertical scroll sequences.  getScrollOffset is
    -- the newest of the four — older firmware lacks it and falls back to
    -- full-frame software scrolling.
    hwscroll  = type(disp.setScrollArea) == "function"
                and type(disp.setScrollOffset) == "function"
                and type(disp.getScrollOffset) == "function"
                and type(disp.flushRows) == "function",
}

local function pushClip(x, y, w, h)
    if not CAP.clip then return nil end
    local px, py, pw, ph = disp.getClipRect()
    -- Intersect with the enclosing clip so nested panels stay bounded.
    local x1 = math.max(x, px)
    local y1 = math.max(y, py)
    local x2 = math.min(x + w, px + pw)
    local y2 = math.min(y + h, py + ph)
    disp.setClipRect(x1, y1, math.max(0, x2 - x1), math.max(0, y2 - y1))
    return { px, py, pw, ph }
end

local function popClip(saved)
    if not CAP.clip then return end
    if saved then
        disp.setClipRect(saved[1], saved[2], saved[3], saved[4])
    else
        disp.clearClipRect()
    end
end

-- ── Constants ───────────────────────────────────────────────────────────────

Panels.ScrollAxis      = { VERTICAL = 1, HORIZONTAL = 2 }
Panels.ScrollDirection = { FORWARD = 1, REVERSE = -1 }
Panels.Effect          = { SHAKE = "shake", BLINK = "blink" }

-- Conventional colour-key for transparency in layer art. transparent_color 0
-- means "disabled" in the SDK, so pure black cannot key; magenta by custom.
Panels.MAGENTA_KEY = disp.rgb(255, 0, 254)

local SCREEN = 320

Panels.Settings = {
    scrollSpeed      = 180,        -- scroll velocity in px per SECOND
    repeatDelayMs    = 180,
    repeatRateMs     = 33,
    advanceMs        = 400,        -- snap animation duration (advance mode)
    defaultFrameSize = SCREEN,     -- panel extent along the axis when omitted
    maxCachedPanels  = 3,          -- resident panel window (prev/current/next)
    minFreeBytes     = 700 * 1024, -- stop caching below this PSRAM headroom
    borderWidth      = 2,
    borderColor      = disp.rgb(40, 40, 48),
    backgroundColor  = disp.rgb(12, 12, 16),
    textColor        = disp.rgb(235, 235, 235),
    choiceColor      = disp.rgb(255, 210, 80),
    transitionMs     = 450,
    idleSkip         = true,       -- skip draw+flush on provably static frames
    useHwScroll      = true,       -- LCD-register scrolling for rigid vertical
                                   -- scroll sequences (see Comic:hwEligible)
    volume           = nil,        -- nil = leave system volume alone
}

-- Story variables, readable by renderCondition and writable by choices.
Panels.vars = {}

-- ── Small utilities ─────────────────────────────────────────────────────────

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function nowMs() return sys.getTimeMs() end

local function psramFree()
    local ok, info = pcall(sys.getMemInfo)
    if ok and type(info) == "table" and info.psram_free then
        return info.psram_free
    end
    return math.huge -- unknown platform: never refuse loads
end

local BTN = {
    UP = input.BTN_UP, DOWN = input.BTN_DOWN,
    LEFT = input.BTN_LEFT, RIGHT = input.BTN_RIGHT,
    ENTER = input.BTN_ENTER, ESC = input.BTN_ESC,
}

-- ── Validation ──────────────────────────────────────────────────────────────
-- comicData is user-authored; fail loudly at new() with a path to the mistake
-- instead of a nil-index three frames into rendering.

local function vfail(path, msg)
    error("panels: " .. path .. ": " .. msg, 0)
end

local function validateLayer(l, path)
    if type(l) ~= "table" then vfail(path, "layer must be a table") end
    if l.image and type(l.image) ~= "string" then
        vfail(path, "image must be a filename string")
    end
    if l.images then
        if type(l.images) ~= "table" or #l.images == 0 then
            vfail(path, "images must be a non-empty array of filenames")
        end
        for i, n in ipairs(l.images) do
            if type(n) ~= "string" then
                vfail(path, "images[" .. i .. "] must be a string")
            end
        end
    end
    if l.text and type(l.text) ~= "string" then
        vfail(path, "text must be a string")
    end
    if not (l.image or l.images or l.text) then
        vfail(path, "layer needs image, images or text")
    end
    if l.parallax and (type(l.parallax) ~= "number"
                       or l.parallax < 0 or l.parallax > 1) then
        vfail(path, "parallax must be a number 0..1")
    end
    if l.animate then
        if type(l.animate) ~= "table" then vfail(path, "animate must be a table") end
        if not l.animate.duration then vfail(path, "animate.duration required") end
    end
end

local function validatePanel(p, path)
    if type(p) ~= "table" then vfail(path, "panel must be a table") end
    if p.frame and type(p.frame) ~= "table" then
        vfail(path, "frame must be a table")
    end
    if p.layers then
        for i, l in ipairs(p.layers) do
            validateLayer(l, path .. ".layers[" .. i .. "]")
        end
    end
    if p.choices then
        if type(p.choices) ~= "table" or #p.choices == 0 then
            vfail(path, "choices must be a non-empty array")
        end
        for i, c in ipairs(p.choices) do
            if type(c) ~= "table" or type(c.text) ~= "string" then
                vfail(path, "choices[" .. i .. "] needs a text field")
            end
            if not c.target then
                vfail(path, "choices[" .. i .. "] needs a target sequence index")
            end
        end
    end
    if p.renderFunction and type(p.renderFunction) ~= "function" then
        vfail(path, "renderFunction must be a function")
    end
end

function Panels.validate(comicData)
    if type(comicData) ~= "table" then error("panels: comicData must be a table", 0) end
    if type(comicData.sequences) ~= "table" or #comicData.sequences == 0 then
        error("panels: comicData.sequences must be a non-empty array", 0)
    end
    for si, seq in ipairs(comicData.sequences) do
        local spath = "sequences[" .. si .. "]"
        if type(seq) ~= "table" then vfail(spath, "sequence must be a table") end
        if type(seq.panels) ~= "table" or #seq.panels == 0 then
            vfail(spath, "panels must be a non-empty array")
        end
        local st = seq.scrollType or "scroll"
        if st ~= "scroll" and st ~= "advance" and st ~= "auto" then
            vfail(spath, "scrollType must be 'scroll', 'advance' or 'auto'")
        end
        if seq.nextSequence and (type(seq.nextSequence) ~= "number"
                                 or not comicData.sequences[seq.nextSequence]) then
            vfail(spath, "nextSequence must index an existing sequence")
        end
        for pi, p in ipairs(seq.panels) do
            validatePanel(p, spath .. ".panels[" .. pi .. "]")
            if p.choices then
                for ci, c in ipairs(p.choices) do
                    if not comicData.sequences[c.target] then
                        vfail(spath .. ".panels[" .. pi .. "].choices[" .. ci .. "]",
                              "target must index an existing sequence")
                    end
                end
            end
        end
    end
    return true
end

-- ── Asset sources ───────────────────────────────────────────────────────────
-- Every path the comic names goes through a source; the library itself never
-- concatenates comic strings into filesystem paths. fileSource is the v1
-- implementation; a zipSource over picocalc.zip.open slots in here later
-- (images via loadFromBuffer; audio still needs cache-extraction, see docs).

local function safeName(name)
    return type(name) == "string"
       and #name > 0
       and not name:find("%.%.")
       and name:sub(1, 1) ~= "/"
       and not name:find("\\")
end

function Panels.fileSource(baseDir)
    baseDir = baseDir or APP_DIR
    return {
        imagePath = function(name)
            if not safeName(name) then error("panels: bad image name: " .. tostring(name), 0) end
            return baseDir .. "/images/" .. name
        end,
        audioPath = function(name)
            if not safeName(name) then error("panels: bad audio name: " .. tostring(name), 0) end
            return baseDir .. "/audio/" .. name
        end,
    }
end

-- ── Asset cache ─────────────────────────────────────────────────────────────
-- The unit of residency is the panel: all its layer images load together and
-- evict together. A window of panels around the viewport stays resident
-- (Settings.maxCachedPanels); everything else is dropped. A single background
-- preload (Core 1) warms the next uncached panel's first image; anything still
-- missing when a panel becomes visible loads synchronously (slideshow
-- precedent: the one-frame hitch is accepted).

local Cache = {}
Cache.__index = Cache

function Cache.new(source)
    return setmetatable({
        source    = source,
        images    = {},   -- path -> image userdata
        panelSets = {},   -- panel -> { paths... }
        pending   = nil,  -- path currently preloading on Core 1
    }, Cache)
end

local function panelImagePaths(cache, panel)
    local set = cache.panelSets[panel]
    if set then return set end
    set = {}
    for _, l in ipairs(panel.layers or {}) do
        if l.image then set[#set + 1] = cache.source.imagePath(l.image) end
        if l.images then
            for _, n in ipairs(l.images) do
                set[#set + 1] = cache.source.imagePath(n)
            end
        end
    end
    cache.panelSets[panel] = set
    return set
end

function Cache:get(path)
    local img = self.images[path]
    if img then return img end
    -- If Core 1 happens to be preloading exactly this image, collect it now
    -- rather than decoding twice.
    self:pollPreload()
    img = self.images[path]
    if img then return img end
    local ok, loaded = pcall(gfx.image.load, path)
    if not ok then
        sys.log("PANELS:ERR image load failed: " .. path)
        return nil
    end
    self.images[path] = loaded
    return loaded
end

function Cache:ensurePanel(panel)
    for _, path in ipairs(panelImagePaths(self, panel)) do
        if not self.images[path] then self:get(path) end
    end
end

function Cache:pollPreload()
    if not self.pending or not CAP.preload then return end
    local ok, img, ready = pcall(gfx.image.pollPreload)
    if ok and ready and img then
        self.images[self.pending] = img
        self.pending = nil
    elseif not ok then
        self.pending = nil
    end
end

-- Warm the first missing image of `panel` in the background, if the slot is
-- free and memory headroom allows.
function Cache:prefetchPanel(panel)
    if not CAP.preload or self.pending then return end
    if psramFree() < Panels.Settings.minFreeBytes then return end
    for _, path in ipairs(panelImagePaths(self, panel)) do
        if not self.images[path] then
            local ok, started = pcall(gfx.image.preload, path)
            if ok and started then self.pending = path end
            return
        end
    end
end

-- Drop every image not referenced by a panel in `keep` (set of panels).
function Cache:evictOutside(keep)
    local wanted = {}
    for panel in pairs(keep) do
        for _, path in ipairs(panelImagePaths(self, panel)) do
            wanted[path] = true
        end
    end
    local dropped = false
    for path in pairs(self.images) do
        if not wanted[path] then
            self.images[path] = nil
            dropped = true
        end
    end
    if dropped then collectgarbage("step") end
end

function Cache:dropAll()
    self.images = {}
    if CAP.preload and self.pending then
        pcall(gfx.image.cancelPreload)
        self.pending = nil
    end
    collectgarbage("collect")
end

-- ── Audio ───────────────────────────────────────────────────────────────────

local Audio = {}
Audio.__index = Audio

function Audio.new(source)
    return setmetatable({ source = source, bgm = nil, players = {} }, Audio)
end

function Audio:startBgm(spec)
    self:stopBgm()
    if not (CAP.sound and spec and spec.file) then return end
    local ok, p = pcall(function()
        local fp = pc.sound.fileplayer()
        fp:load(self.source.audioPath(spec.file))
        return fp
    end)
    if not ok or not p then
        sys.log("PANELS:ERR bgm load failed: " .. tostring(spec.file))
        return
    end
    if spec.volume then pcall(p.setVolume, p, spec.volume) end
    if spec.loop then
        -- Loop by replay-on-finish; the callback fires from the opcode hook,
        -- so it only sets a flag that the next update() acts on.
        local self_ = self
        pcall(p.setFinishCallback, p, function() self_.bgmFinished = true end)
    end
    pcall(p.play, p)
    self.bgm = p
    self.bgmLoop = spec.loop and true or false
    self.bgmFinished = false
end

function Audio:stopBgm()
    if self.bgm then
        pcall(self.bgm.stop, self.bgm)
        self.bgm = nil
    end
    self.bgmFinished = false
end

function Audio:playSfx(spec)
    if not (CAP.sound and spec and spec.file) then return end
    local path = self.source.audioPath(spec.file)
    local p = self.players[path]
    if not p then
        local ok, made = pcall(pc.sound.sampleplayer, path)
        if not ok or not made then
            sys.log("PANELS:ERR sfx load failed: " .. tostring(spec.file))
            self.players[path] = false -- don't retry every trigger
            return
        end
        p = made
        self.players[path] = p
    end
    if p == false then return end
    if spec.volume then pcall(p.setVolume, p, spec.volume) end
    pcall(p.play, p)
end

function Audio:update()
    if self.bgm and self.bgmLoop and self.bgmFinished then
        self.bgmFinished = false
        pcall(self.bgm.play, self.bgm)
    end
end

function Audio:teardown()
    self:stopBgm()
    for _, p in pairs(self.players) do
        if p then pcall(p.stop, p) end
    end
    self.players = {}
end

-- ── Sequence layout ─────────────────────────────────────────────────────────
-- Precomputed once per sequence: each panel's start offset and extent along
-- the scroll axis, plus its reachable scroll-percentage range so triggers at
-- 0 and 1 stay reachable for the first/last panels (whose leading/trailing
-- edges never traverse the full viewport).

-- A panel is "rigid" when its pixels depend only on the panel's position —
-- not on the scroll percentage, time, or randomness.  A sequence of rigid
-- panels is one static tall strip, which the hardware-scroll fast path can
-- move in silicon with no redraws.  Anything pct- or time-varying (parallax,
-- animators, image arrays, scroll triggers, shake/blink, custom render)
-- disqualifies.  Audio scroll triggers do NOT: they change no pixels and the
-- fast path evaluates them against the true scroll position every frame.
local function panelIsRigid(panel)
    if panel.effect or panel.renderFunction or panel.updateFunction then
        return false
    end
    for _, l in ipairs(panel.layers or {}) do
        if (l.parallax and l.parallax > 0) or l.animate or l.images
           or l.effect or l.scrollTrigger then
            return false
        end
    end
    return true
end

local function buildLayout(seq)
    local S = Panels.Settings
    local layout = { panels = {}, axis = seq.axis or Panels.ScrollAxis.VERTICAL }
    layout.rigid = true
    local pos = 0
    for i, panel in ipairs(seq.panels) do
        if not panelIsRigid(panel) then layout.rigid = false end
        local f = panel.frame or {}
        local size
        if layout.axis == Panels.ScrollAxis.HORIZONTAL then
            size = f.width or S.defaultFrameSize
        else
            size = f.height or S.defaultFrameSize
        end
        pos = pos + (f.marginBefore or 0)
        layout.panels[i] = { panel = panel, start = pos, size = size }
        pos = pos + size + (f.marginAfter or 0)
    end
    layout.total = pos
    layout.maxScroll = math.max(0, pos - SCREEN)

    for _, lp in ipairs(layout.panels) do
        local D = lp.size + SCREEN
        local rawMin = clamp((0 + SCREEN - lp.start) / D, 0, 1)
        local rawMax = clamp((layout.maxScroll + SCREEN - lp.start) / D, 0, 1)
        lp.pctMin, lp.pctMax = rawMin, math.max(rawMax, rawMin + 1e-6)
    end
    return layout
end

local function panelPct(lp, scrollPos)
    local raw = clamp((scrollPos + SCREEN - lp.start) / (lp.size + SCREEN), 0, 1)
    return clamp((raw - lp.pctMin) / (lp.pctMax - lp.pctMin), 0, 1)
end

-- ── Layer rendering ─────────────────────────────────────────────────────────

local function newAnimator(duration, from, to, ease, delay)
    local ok, a = pcall(gfx.animation.animator.new, duration, from, to,
                        ease or "cubicOut", delay or 0)
    if ok then return a end
    -- an unrecognised easing name raises; a comic's typo falls back to linear
    ok, a = pcall(gfx.animation.animator.new, duration, from, to, "linear", delay or 0)
    if ok then return a end
    return nil
end

local function layerRuntime(comic, layer)
    local rt = comic.layerState[layer]
    if not rt then
        rt = { armed = false, ax = nil, ay = nil, sfxFired = false }
        comic.layerState[layer] = rt
    end
    return rt
end

local function shakeOffset(effect)
    local s = (effect and effect.strength) or 3
    return math.random(-s, s), math.random(-s, s)
end

local function blinkOn(effect)
    local on  = (effect and effect.onMs) or 400
    local off = (effect and effect.offMs) or 300
    return (nowMs() % (on + off)) < on
end

local function condPasses(cond)
    if not cond then return true end
    return Panels.vars[cond.var] == cond.equals
end

-- Draw one layer inside a panel whose frame origin is at (ox, oy) on screen.
local function drawLayer(comic, lp, layer, ox, oy, pct, axisVertical)
    if layer.visible == false then return end
    if not condPasses(layer.renderCondition) then return end
    if layer.opacity and layer.opacity < 0.5 then return end -- binary opacity
    if layer.scrollTrigger and pct < layer.scrollTrigger and not layer.images then
        return
    end
    if layer.effect and layer.effect.type == "blink" and not blinkOn(layer.effect) then
        return
    end

    local rt = layerRuntime(comic, layer)

    -- Parallax offset along the scroll axis.
    local D = lp.size + SCREEN
    local par = layer.parallax or 0
    local shift = (D * pct - D / 2) * par
    local lx = (layer.x or 0)
    local ly = (layer.y or 0)
    if axisVertical then ly = ly + shift else lx = lx + shift end

    -- Keyframed animation: arm at the scroll trigger, add animator offsets.
    if layer.animate then
        local trig = layer.animate.scrollTrigger or 0
        if not rt.armed and pct >= trig then
            rt.armed = true
            local a = layer.animate
            if a.x then rt.ax = newAnimator(a.duration, layer.x or 0, a.x, a.ease, a.delay) end
            if a.y then rt.ay = newAnimator(a.duration, layer.y or 0, a.y, a.ease, a.delay) end
        end
        if rt.armed then
            if rt.ax then lx = rt.ax:currentValue() + (axisVertical and 0 or shift) end
            if rt.ay then ly = rt.ay:currentValue() + (axisVertical and shift or 0) end
        end
    end

    if layer.effect and layer.effect.type == "shake" then
        local dx, dy = shakeOffset(layer.effect)
        lx, ly = lx + dx, ly + dy
    end

    local sx = math.floor(ox + lx)
    local sy = math.floor(oy + ly)

    if layer.text then
        local font  = layer.font or comic.data.font
        local color = layer.color or Panels.Settings.textColor
        if layer.w and layer.h then
            gfx.drawTextInRect(layer.text, sx, sy, layer.w, layer.h,
                               layer.align or 0, font)
        else
            -- bg=false → transparent glyph background (the binding's contract;
            -- omitting the argument would paint black boxes behind the text).
            disp.drawText(sx, sy, layer.text, color, layer.bg or false)
        end
        return
    end

    local path
    if layer.images then
        local n = #layer.images
        local idx = clamp(math.ceil(pct * n), 1, n)
        path = comic.cache.source.imagePath(layer.images[idx])
    else
        path = comic.cache.source.imagePath(layer.image)
    end
    local img = comic.cache:get(path)
    if img then
        -- transparentColor = true selects the conventional magenta key.
        -- Reapplied every draw: it is a plain field set, and the image may
        -- have been evicted and reloaded since the key was last applied.
        local key = layer.transparentColor
        if key then
            if key == true then key = Panels.MAGENTA_KEY end
            pcall(img.setTransparentColor, img, key)
        end
        img:draw(sx, sy)
    end
end

-- ── Panel rendering ─────────────────────────────────────────────────────────

local function drawPanel(comic, lp, scrollPos)
    local S = Panels.Settings
    local axisVertical = comic.layout.axis == Panels.ScrollAxis.VERTICAL
    local panel = lp.panel

    local ox, oy, pw, ph
    if axisVertical then
        ox, oy = 0, lp.start - scrollPos
        pw, ph = SCREEN, lp.size
    else
        ox, oy = lp.start - scrollPos, 0
        pw, ph = lp.size, SCREEN
    end

    local pct = panelPct(lp, scrollPos)

    if panel.effect and panel.effect.type == "shake" then
        local dx, dy = shakeOffset(panel.effect)
        ox, oy = ox + dx, oy + dy
    end

    local saved = pushClip(ox, oy, pw, ph)

    disp.fillRect(ox, oy, pw, ph,
                  panel.backgroundColor or comic.seq.backgroundColor
                  or S.backgroundColor)

    for _, layer in ipairs(panel.layers or {}) do
        drawLayer(comic, lp, layer, ox, oy, pct, axisVertical)
    end

    if panel.updateFunction then
        local ok, err = pcall(panel.updateFunction, panel, pct)
        if not ok then error("panels: updateFunction: " .. tostring(err), 0) end
    end
    if panel.renderFunction then
        local ok, err = pcall(panel.renderFunction, panel, ox, oy, pct)
        if not ok then error("panels: renderFunction: " .. tostring(err), 0) end
    end

    popClip(saved)

    if not panel.borderless and S.borderWidth > 0 then
        for i = 0, S.borderWidth - 1 do
            disp.drawRect(ox + i, oy + i, pw - 2 * i, ph - 2 * i, S.borderColor)
        end
    end

    -- Audio trigger: fires once per viewport entry.  Suppressed during
    -- ring-strip draws, whose scroll origin (and therefore pct) is fake —
    -- the fast path evaluates triggers separately with the true position.
    if panel.audio and not comic.suppressAudio then
        local rt = layerRuntime(comic, panel)
        local trig = panel.audio.scrollTrigger or 0
        if not rt.sfxFired and pct >= trig and pct < 1 then
            rt.sfxFired = true
            comic.audio:playSfx(panel.audio)
        end
    end
end

-- ── Choice UI ───────────────────────────────────────────────────────────────

local function drawChoices(comic)
    local S = Panels.Settings
    local panel = comic.choicePanel.panel
    local n = #panel.choices
    local rowH = 22
    local boxH = n * rowH + 30
    local boxW = 240
    local bx = (SCREEN - boxW) // 2
    local by = SCREEN - boxH - 20

    disp.fillRect(bx, by, boxW, boxH, S.backgroundColor)
    disp.drawRect(bx, by, boxW, boxH, S.choiceColor)
    if panel.choices.prompt or panel.prompt then
        disp.drawText(bx + 10, by + 8,
                      panel.choices.prompt or panel.prompt, S.textColor, false)
    end
    for i, c in ipairs(panel.choices) do
        local y = by + 24 + (i - 1) * rowH
        local selected = (i == comic.choiceIndex)
        if selected and blinkOn({ onMs = 500, offMs = 180 }) then
            disp.drawText(bx + 10, y, ">", S.choiceColor, false)
        end
        disp.drawText(bx + 26, y, c.text,
                      selected and S.choiceColor or S.textColor, false)
    end
end

-- ── The comic object ────────────────────────────────────────────────────────

local Comic = {}
Comic.__index = Comic

function Panels.new(comicData, opts)
    Panels.validate(comicData)
    opts = opts or {}

    local source = opts.source or Panels.fileSource(opts.baseDir)

    local comic = setmetatable({
        data        = comicData,
        opts        = opts,
        source      = source,
        cache       = Cache.new(source),
        audio       = Audio.new(source),
        layerState  = {},          -- layer/panel -> runtime state
        seqIndex    = 0,
        seq         = nil,
        layout      = nil,
        scrollPos   = 0,
        scrollAnim  = nil,         -- animator for advance/auto snapping
        state       = "run",       -- run | choice | fade_out | fade_in | done
        fadeAnim    = nil,
        fadeTarget  = nil,         -- pending sequence index during fade
        choicePanel = nil,
        choiceIndex = 1,
        result      = nil,
        lastAutoMs  = nowMs(),
        dirty       = true,
        saveName    = opts.name or comicData.name or "comic",
    }, Comic)

    if type(input.setRepeat) == "function" then
        input.setRepeat(Panels.Settings.repeatDelayMs, Panels.Settings.repeatRateMs)
    end

    local startSeq = opts.startSequence or 1
    if opts.resume then
        local saved = comic:loadProgress()
        if saved then
            startSeq = saved.seq or startSeq
            if type(saved.vars) == "table" then Panels.vars = saved.vars end
        end
    end
    comic:enterSequence(startSeq, true)
    return comic
end

-- Save key is namespaced by app id because game.save used to write to a
-- single global /saves directory. Saves are per-app now, but the key is kept
-- so progress saved before that change is still found (and migrated).
function Comic:saveKey()
    local id = (APP_ID or "app"):gsub("[^%w_%-]", "_")
    return "panels_" .. id .. "_" .. self.saveName:gsub("[^%w_%-]", "_")
end

function Comic:saveProgress()
    if not CAP.save then return end
    pcall(pc.game.save.set, self:saveKey(),
          { seq = self.seqIndex, vars = Panels.vars })
end

function Comic:loadProgress()
    if not CAP.save then return nil end
    local ok, saved = pcall(pc.game.save.get, self:saveKey())
    if ok and type(saved) == "table" then return saved end
    return nil
end

function Comic:clearProgress()
    if not CAP.save then return end
    pcall(pc.game.save.delete, self:saveKey())
end

function Comic:enterSequence(idx, skipSave)
    local seq = self.data.sequences[idx]
    if not seq then
        self.state = "done"
        self.result = "finished"
        return
    end

    -- Drop the outgoing sequence's assets wholesale (slideshow precedent).
    self.cache:dropAll()
    self.layerState = {}
    self.audio:stopBgm()

    self.seqIndex = idx
    self.seq = seq
    self.layout = buildLayout(seq)
    self.scrollAnim = nil
    self.choicePanel = nil
    self.choiceIndex = 1
    self.currentPanel = 1
    self.lastAutoMs = nowMs()
    self.dirty = true

    local reverse = (seq.direction or Panels.ScrollDirection.FORWARD)
                    == Panels.ScrollDirection.REVERSE
    self.reverse = reverse
    self.scrollPos = 0

    -- Load what is immediately visible before first draw.
    for _, lp in ipairs(self:visiblePanels()) do
        self.cache:ensurePanel(lp.panel)
    end

    self.audio:startBgm(seq.audio)
    if not skipSave then self:saveProgress() end
    sys.log("PANELS:SEQ " .. idx)
end

function Comic:visiblePanels(margin)
    margin = margin or 0
    local out = {}
    local lo = self.scrollPos - margin
    local hi = self.scrollPos + SCREEN + margin
    for _, lp in ipairs(self.layout.panels) do
        if lp.start + lp.size > lo and lp.start < hi then
            out[#out + 1] = lp
        end
    end
    return out
end

-- The panel nearest the viewport centre — target of choices and snapping.
function Comic:centrePanel()
    local mid = self.scrollPos + SCREEN / 2
    local best, bestDist
    for i, lp in ipairs(self.layout.panels) do
        local c = lp.start + lp.size / 2
        local d = math.abs(c - mid)
        if not bestDist or d < bestDist then best, bestDist = i, d end
    end
    return best
end

function Comic:snapTarget(i)
    local lp = self.layout.panels[i]
    if not lp then return nil end
    return clamp(lp.start + lp.size / 2 - SCREEN / 2, 0, self.layout.maxScroll)
end

function Comic:advanceTo(i)
    local target = self:snapTarget(i)
    if not target then return false end
    self.currentPanel = i
    self.scrollAnim = newAnimator(Panels.Settings.advanceMs,
                                  self.scrollPos, target, "cubicOut")
    if not self.scrollAnim then self.scrollPos = target end
    self.dirty = true
    return true
end

function Comic:nextSequenceIndex()
    return self.seq.nextSequence or (self.seqIndex + 1)
end

function Comic:startTransition(targetSeq)
    self:hwExit()  -- fades and cuts draw full frames; release the ring first
    local kind = self.seq.transition or "fadeToBlack"
    if kind == "cut" then
        self:enterSequence(targetSeq)
        return
    end
    self.state = "fade_out"
    self.fadeTarget = targetSeq
    self.fadeWhite = (kind == "fadeToWhite")
    self.fadeAnim = newAnimator(Panels.Settings.transitionMs, 0, 255, "sineInOut")
    if not self.fadeAnim then -- animator unavailable: hard cut
        self.state = "run"
        self:enterSequence(targetSeq)
    end
end

function Comic:goToSequence(idx)
    self:startTransition(idx)
end

function Comic:finished() return self.state == "done" end
function Comic:currentSequence() return self.seqIndex end

-- ── Hardware-scroll fast path ───────────────────────────────────────────────
-- Rigid vertical "scroll" sequences render into the LCD's frame memory as a
-- mod-320 ring — virtual row v lives at GRAM row v % 320 — and scroll by
-- writing the panel's VSCRSADD register.  Per frame only the newly revealed
-- strip (a few rows) is drawn and partially flushed, instead of full-screen
-- blits plus a 205KB DMA: the scroll itself happens in silicon.
--
-- OS contract: the system menu and launcher reset the scroll offset to 0
-- when they take the screen.  hwSync() detects any foreign register write
-- via getScrollOffset()'s write counter and falls back to a full repaint.
--
-- Caveat: on-device screenshots read the draw framebuffer, which holds the
-- ring layout while the fast path is active — capture tools see a rotated
-- image.  The simulator composes screenshots through its GRAM emulation, so
-- sim captures show the true screen.

function Comic:hwEligible()
    local S = Panels.Settings
    return CAP.hwscroll and CAP.clip and S.useHwScroll ~= false
       and self.layout.rigid
       and self.layout.axis == Panels.ScrollAxis.VERTICAL
       and (self.seq.scrollType or "scroll") == "scroll"
       and self.state == "run"
       and not self.scrollAnim
end

-- Draw virtual rows [v0, v0+len) into the framebuffer at their GRAM ring
-- rows (v % 320).  len must not cross a mod-320 boundary (callers split).
function Comic:drawRingStrip(v0, len)
    local S = Panels.Settings
    local fbRow = v0 % SCREEN
    local fake = v0 - fbRow  -- scroll origin that lands v0 on fbRow
    local saved = pushClip(0, fbRow, SCREEN, len)
    disp.fillRect(0, fbRow, SCREEN, len,
                  self.seq.backgroundColor or S.backgroundColor)
    self.suppressAudio = true
    for _, lp in ipairs(self.layout.panels) do
        if lp.start + lp.size > v0 and lp.start < v0 + len then
            drawPanel(self, lp, fake)
        end
    end
    self.suppressAudio = false
    popClip(saved)
end

function Comic:hwRememberWrite()
    local off, gen = disp.getScrollOffset()
    self.hws.offset = off
    self.hws.gen = gen or 0
end

function Comic:hwEnter()
    local sp = math.floor(self.scrollPos)
    local g0 = sp % SCREEN
    -- Ring-paint the visible window (two mod-320 spans), then flush without
    -- a buffer swap so the draw buffer keeps accumulating the ring image.
    self:drawRingStrip(sp, SCREEN - g0)
    if g0 > 0 then self:drawRingStrip(sp + SCREEN - g0, g0) end
    disp.flushRows(0, SCREEN - 1)
    -- Frame memory is 480 lines; (0, 320, 160) rings the visible panel.
    disp.setScrollArea(0, SCREEN, 160)
    disp.setScrollOffset(g0)
    self.hws = { pos = sp }
    self:hwRememberWrite()
    self.dirty = false
    sys.log("PANELS:HW on")
end

-- Leave the fast path, repainting in normal screen layout first so the
-- register reset reveals a correct frame (the setter waits out the DMA).
function Comic:hwExit()
    if not self.hws then return end
    self.hws = nil
    self:drawFrame()
    disp.flushRows(0, SCREEN - 1)
    disp.setScrollOffset(0)
    self.dirty = true
    sys.log("PANELS:HW off")
end

-- Detect a foreign scroll-register write (system menu, launcher): the OS
-- resets the offset to 0 and leaves GRAM holding whatever it drew, so the
-- ring is gone.  Drop the fast path; eligibility re-enters it next frame
-- with a full ring repaint.
function Comic:hwSync()
    if not self.hws then return end
    local off, gen = disp.getScrollOffset()
    if off ~= self.hws.offset or (gen or 0) ~= self.hws.gen then
        self.hws = nil
        self.dirty = true
    end
end

-- Move the ring to scroll position np: draw and flush only the newly
-- revealed strip, then bump the register.
function Comic:hwScrollTo(np)
    local hws = self.hws
    self.scrollPos = np
    local sp = math.floor(np)
    local delta = sp - hws.pos
    if delta == 0 then return end
    if delta >= SCREEN or delta <= -SCREEN then
        -- Jumped a whole screen or more: nothing on-screen survives,
        -- cheaper to repaint the ring outright.
        self.hws = nil
        self:hwEnter()
        return
    end
    local v0, len
    if delta > 0 then v0, len = hws.pos + SCREEN, delta
    else              v0, len = sp, -delta end
    while len > 0 do
        local g0 = v0 % SCREEN
        local span = math.min(len, SCREEN - g0)
        self:drawRingStrip(v0, span)
        disp.flushRows(g0, g0 + span - 1)
        v0, len = v0 + span, len - span
    end
    disp.setScrollOffset(sp % SCREEN)
    hws.pos = sp
    self:hwRememberWrite()
end

-- drawPanel fires audio triggers only when it draws; in the fast path
-- visible panels are not redrawn, so evaluate triggers here every frame
-- against the true scroll position.
function Comic:hwAudioTriggers()
    local sp = math.floor(self.scrollPos)
    for _, lp in ipairs(self:visiblePanels()) do
        local panel = lp.panel
        if panel.audio then
            local rt = layerRuntime(self, panel)
            local trig = panel.audio.scrollTrigger or 0
            local pct = panelPct(lp, sp)
            if not rt.sfxFired and pct >= trig and pct < 1 then
                rt.sfxFired = true
                self.audio:playSfx(panel.audio)
            end
        end
    end
end

-- ── Input handling per scroll mode ──────────────────────────────────────────

function Comic:forwardBack()
    local axisVertical = self.layout.axis == Panels.ScrollAxis.VERTICAL
    local fwd, back
    if axisVertical then fwd, back = BTN.DOWN, BTN.UP
    else fwd, back = BTN.RIGHT, BTN.LEFT end
    if self.reverse then fwd, back = back, fwd end
    return fwd, back
end

function Comic:handleScrollInput(pressed, repeated, dt)
    local S = Panels.Settings
    local fwd, back = self:forwardBack()
    local st = self.seq.scrollType or "scroll"

    if st == "scroll" then
        -- Frame-paced scrolling: movement is scrollSpeed px/second scaled by
        -- the real frame time, not a fixed step per key-repeat tick. Repeat
        -- ticks (33ms) beat against slower hardware frames, so tick-stepping
        -- moved 1-3 steps per frame in an uneven rhythm — judder. Velocity
        -- scaling keeps every drawn frame's movement proportional instead.
        local held = input.getButtons()
        local dir = 0
        if held & fwd ~= 0 then dir = 1 end
        if held & back ~= 0 then dir = -1 end
        if dir ~= 0 then
            local delta = dir * S.scrollSpeed * dt / 1000
            local np = clamp(self.scrollPos + delta, 0, self.layout.maxScroll)
            if np ~= self.scrollPos then
                if self.hws then
                    self:hwScrollTo(np)  -- strip draw + register bump
                else
                    self.scrollPos = np
                    self.dirty = true
                end
            elseif dir > 0 and pressed & fwd ~= 0
                   and self.scrollPos >= self.layout.maxScroll then
                -- Pushed forward at the end of the sequence: advance.
                self:startTransition(self:nextSequenceIndex())
            end
        end

    elseif st == "advance" then
        local key = self.seq.advanceControl
                    or (self.layout.panels[self.currentPanel]
                        and self.layout.panels[self.currentPanel].panel.advanceControl)
                    or fwd
        if pressed & key ~= 0 or (key ~= fwd and pressed & fwd ~= 0) then
            if self.currentPanel < #self.layout.panels then
                self:advanceTo(self.currentPanel + 1)
            else
                self:startTransition(self:nextSequenceIndex())
            end
        elseif pressed & back ~= 0 and self.currentPanel > 1 then
            self:advanceTo(self.currentPanel - 1)
        end

    else -- auto
        local lp = self.layout.panels[self.currentPanel]
        local waitMs = (lp and lp.panel.autoAdvanceMs)
                       or self.seq.autoAdvanceMs or 2500
        if nowMs() - self.lastAutoMs >= waitMs then
            self.lastAutoMs = nowMs()
            if self.currentPanel < #self.layout.panels then
                self:advanceTo(self.currentPanel + 1)
            else
                self:startTransition(self:nextSequenceIndex())
            end
        end
    end
end

function Comic:handleChoiceInput(pressed)
    local panel = self.choicePanel.panel
    local n = #panel.choices
    if pressed & BTN.UP ~= 0 then
        self.choiceIndex = ((self.choiceIndex - 2) % n) + 1
        self.dirty = true
    elseif pressed & BTN.DOWN ~= 0 then
        self.choiceIndex = (self.choiceIndex % n) + 1
        self.dirty = true
    elseif pressed & BTN.ENTER ~= 0 then
        local c = panel.choices[self.choiceIndex]
        if c.setVars then
            for k, v in pairs(c.setVars) do Panels.vars[k] = v end
        end
        sys.log("PANELS:CHOICE " .. self.choiceIndex .. " -> seq " .. c.target)
        self.state = "run"
        self.choicePanel = nil
        self:startTransition(c.target)
    end
end

-- ── Frame update ────────────────────────────────────────────────────────────

function Comic:update()
    local S = Panels.Settings
    self.cache:pollPreload()
    self.audio:update()

    if self.state == "done" then return end

    -- If the OS wrote the scroll register since our last write (system
    -- menu), the GRAM ring is gone — drop the fast path and repaint.
    self:hwSync()

    -- Frame delta for velocity-based scrolling; capped so a stall (modal,
    -- sequence load, system menu) doesn't turn into one giant jump.
    local now = nowMs()
    local dt = now - (self.lastFrameMs or now)
    if dt > 100 then dt = 100 end
    self.lastFrameMs = now

    local pressed = input.getButtonsPressed()
    local repeated = input.getButtonsRepeated and input.getButtonsRepeated() or 0

    -- Fades run to completion regardless of input.
    if self.state == "fade_out" or self.state == "fade_in" then
        self:drawFrame()
        local v = math.floor(self.fadeAnim:currentValue())
        if self.state == "fade_in" then v = 255 - v end
        if self.fadeWhite then
            disp.applyEffect("fade", 255, 255, 255, v)
        else
            disp.applyEffect("darken", 255 - v)
        end
        self.dirty = true
        self.drewThisFrame = true
        if self.fadeAnim:ended() then
            if self.state == "fade_out" then
                self:enterSequence(self.fadeTarget)
                if self.state ~= "done" then
                    self.state = "fade_in"
                    self.fadeAnim = newAnimator(S.transitionMs, 0, 255, "sineInOut")
                    if not self.fadeAnim then self.state = "run" end
                end
            else
                self.state = "run"
            end
        end
        return
    end

    if self.state == "choice" then
        self:handleChoiceInput(pressed)
        if self.state == "choice" then -- confirm may have started a transition
            self:drawFrame()
            drawChoices(self)
            self.drewThisFrame = true
        end
        return
    end

    -- Scroll snap animation in flight?
    if self.scrollAnim then
        self.scrollPos = self.scrollAnim:currentValue()
        self.dirty = true
        if self.scrollAnim:ended() then
            self.scrollPos = self.scrollAnim:currentValue()
            self.scrollAnim = nil
        end
    else
        self:handleScrollInput(pressed, repeated, dt)
    end

    -- Track the centre panel; reset off-screen layer state so animations and
    -- audio triggers re-arm when their panel scrolls back in.
    local centre = self:centrePanel()
    if centre ~= self.currentPanel and (self.seq.scrollType or "scroll") == "scroll" then
        self.currentPanel = centre
    end
    local visible = {}
    for _, lp in ipairs(self:visiblePanels()) do visible[lp.panel] = true end
    for _, lp in ipairs(self.layout.panels) do
        if not visible[lp.panel] then
            if self.layerState[lp.panel] then self.layerState[lp.panel] = nil end
            for _, layer in ipairs(lp.panel.layers or {}) do
                if self.layerState[layer] then self.layerState[layer] = nil end
            end
        end
    end

    -- Choice activation: centre panel with choices, once settled.
    local clp = self.layout.panels[self.currentPanel]
    if clp and clp.panel.choices and not self.scrollAnim then
        self.state = "choice"
        self.choicePanel = clp
        self.choiceIndex = 1
        self.dirty = true
    end

    -- Cache maintenance. Visible panels load synchronously (they must draw
    -- this frame); the wider window is warmed asynchronously only, so a fast
    -- scroll never stalls on a panel that is still off-screen. Eviction runs
    -- when headroom drops toward the emergency-GC floor.
    local keep = {}
    local windowMargin = SCREEN * (S.maxCachedPanels - 1) / 2
    for _, lp in ipairs(self:visiblePanels(windowMargin)) do
        keep[lp.panel] = true
    end
    for _, lp in ipairs(self:visiblePanels()) do
        self.cache:ensurePanel(lp.panel)
    end
    if psramFree() < S.minFreeBytes then
        self.cache:evictOutside(keep)
    end
    local nextIdx = self.currentPanel + 1
    if self.layout.panels[nextIdx] then
        self.cache:prefetchPanel(self.layout.panels[nextIdx])
    end

    -- Hardware-scroll fast path: enter or leave as eligibility changes.
    -- While active, skip the normal draw entirely — hwScrollTo already drew
    -- and flushed the revealed strips, and the runner must not full-flush
    -- (the framebuffer holds the ring layout, not the screen layout).
    if self:hwEligible() then
        if not self.hws then self:hwEnter() end
    else
        self:hwExit()
    end
    if self.hws then
        self:hwAudioTriggers()
        self.dirty = false
        self.drewThisFrame = false
        return
    end

    -- Animators, blinkers and shakes render continuously; only provably
    -- static frames can skip.
    if self:hasLiveMotion() then self.dirty = true end

    if self.dirty or not S.idleSkip then
        self:drawFrame()
        self.dirty = false
        self.drewThisFrame = true
    else
        self.drewThisFrame = false
    end
end

function Comic:hasLiveMotion()
    if self.scrollAnim then return true end
    for _, lp in ipairs(self:visiblePanels()) do
        local p = lp.panel
        if p.effect then return true end
        if p.audio and not (self.layerState[p] and self.layerState[p].sfxFired) then
            return true
        end
        for _, layer in ipairs(p.layers or {}) do
            if layer.effect then return true end
            local rt = self.layerState[layer]
            if rt and rt.armed then
                if (rt.ax and not rt.ax:ended()) or (rt.ay and not rt.ay:ended()) then
                    return true
                end
            elseif layer.animate then
                return true -- not yet armed; pct changes may arm it
            end
        end
        if (self.seq.scrollType or "scroll") == "auto" then return true end
    end
    return false
end

function Comic:drawFrame()
    disp.clear(self.seq.backgroundColor or Panels.Settings.backgroundColor)
    for _, lp in ipairs(self:visiblePanels()) do
        drawPanel(self, lp, math.floor(self.scrollPos))
    end
end

function Comic:teardown()
    self.audio:teardown()
    self.cache:dropAll()
    if self.hws then
        disp.setScrollOffset(0)  -- launcher takes the screen next
        self.hws = nil
    end
    if CAP.clip then disp.clearClipRect() end
end

-- ── Blocking runner ─────────────────────────────────────────────────────────

local function drawErrorScreen(err)
    disp.clear(disp.rgb(20, 0, 0))
    disp.fillRect(0, 0, SCREEN, 14, disp.rgb(180, 30, 30))
    disp.drawText(4, 3, "PANELS FAULT", disp.rgb(255, 255, 255), false)
    local y = 20
    for line in tostring(err):gmatch("[^\n]+") do
        if y > 290 then break end
        for i = 1, #line, 52 do
            if y > 290 then break end
            disp.drawText(3, y, line:sub(i, i + 51), disp.rgb(230, 230, 230),
                          false)
            y = y + 9
        end
    end
    disp.drawText(4, 302, "ESC exit", disp.rgb(160, 160, 160), false)
    disp.flush()
end

-- Blocking entry point. Returns "finished" | "quit" | "error"[, err].
function Panels.start(comicData, opts)
    opts = opts or {}
    local ok, comic = pcall(Panels.new, comicData, opts)
    if not ok then
        -- Validation errors surface immediately on screen, not as a crash.
        local err = comic
        sys.log("PANELS:ERR " .. tostring(err))
        while true do
            input.update()
            if input.getButtonsPressed() & BTN.ESC ~= 0 then
                return "error", err
            end
            drawErrorScreen(err)
        end
    end

    local errorInfo = nil
    local result = nil

    while true do
        input.update()
        local pressed = input.getButtonsPressed()

        if errorInfo then
            if pressed & BTN.ESC ~= 0 then
                result = "error"
                break
            end
            drawErrorScreen(errorInfo)
        else
            if pressed & BTN.ESC ~= 0 then
                comic:saveProgress()
                result = "quit"
                break
            end
            local fok, ferr = xpcall(function() comic:update() end,
                                     function(e) return tostring(e) end)
            if not fok then
                errorInfo = ferr
                sys.log("PANELS:ERR " .. tostring(ferr))
            elseif comic:finished() then
                comic:clearProgress() -- a finished comic restarts fresh
                result = comic.result or "finished"
                break
            elseif comic.drewThisFrame ~= false then
                disp.flush()
            end
        end
    end

    comic:teardown()
    sys.log("PANELS:EXIT " .. tostring(result))
    if result == "error" then return "error", errorInfo end
    return result
end

return Panels
