"""Bridge metatables hide their metamethods (review: Lua High, "__index is
the metatable itself, so v:__gc() can be called from Lua").

Every bridge userdata type is registered by lb_register_type (lua_bridge.c):
methods live in a separate table reached through __index, the metatable
holds only metamethods and is locked (__metatable = false). So obj:__gc()
is "attempt to call a nil value", never a second finaliser run, and
getmetatable(obj) is false.

A destroyed object can still be reached from Lua through resurrection: a
finaliser that runs after the object's own __gc can use it. Those cases
build that order deliberately (Lua 5.4 runs finalisers newest-marked first)
and require a clean Lua error rather than a NULL dereference.

Each case is its own inline app on its own simulator, because a regression
takes the simulator down. video (hardware-only; the sim stubs the player) and
crypto (absent in the sim) are hardware-pending.
"""

import pytest

from helpers import run_lua_app, stage_lua_app, write_mod, write_wav

# name -> (Lua expression building one object, requirements)
TYPES = {
    "image": ("G.image.new(8, 8)", ()),
    "sprite": ("G.sprite.new(G.image.new(8, 8))", ()),
    "spritesheet": ("G.spritesheet.newGrid(G.image.new(8, 8), 1, 1, 8, 8)", ()),
    "tilemap": ("G.tilemap.new(G.image.new(8, 8), 8, 8)", ()),
    "animation_loop": ("G.animation.loop.new(100, {G.image.new(8, 8)})", ()),
    "animator": ("G.animation.animator.new(100, 0, 1)", ()),
    "blinker": ("G.animation.blinker.new()", ()),
    "font": ("G.font.new('6x8')", ()),
    "terminal": ("pc.terminal.new(10, 5)", ()),
    "modplayer": ("pc.modplayer.create()", ()),
    "camera": ("pc.game.camera.new()", ()),
    "scene_pool": ("pc.game.scene.objectPool('p', function() return {} end)", ()),
    "zip_archive": ("pc.zip.open(APP_DIR .. '/a.zip')", ()),
    "tcp": ("pc.tcp.new('127.0.0.1', 9)", ("http",)),
    "http": ("pc.network.http.new('127.0.0.1', 9)", ("http",)),
    "sample": ("pc.sound.sample(APP_DIR .. '/s.wav')", ("audio",)),
    "sampleplayer": ("pc.sound.sampleplayer(pc.sound.sample(APP_DIR .. '/s.wav'))",
                     ("audio",)),
    "fileplayer": ("pc.sound.fileplayer()", ("audio",)),
    "mp3player": ("pc.sound.mp3player()", ("audio",)),
    "fs_file": ("pc.fs.open(pc.fs.appPath('f.txt'), 'w')", ()),
    "gfx3d_mesh": ("pc.gfx3d.newMesh({0,0,0, 1,0,0, 0,1,0}, {1,2,3}, 0)", ()),
}

PRELUDE = """
local pc = picocalc
local G = pc.graphics
local T = picocalc.sys.loadlib('picotest')
"""


def _zip_bytes():
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("hello.txt", "hi")
    return buf.getvalue()


def _stage(sim, name, body, requirements=()):
    app = stage_lua_app(sim.sd_card_path, name, PRELUDE + body + "\nT.done()\n",
                        requirements=requirements,
                        files={"a.zip": _zip_bytes()})
    write_wav(app / "s.wav")
    write_mod(app / "m.mod")
    return app


HIDDEN = """
T.case('hidden', function()
    local o = T.ok(%s, 'constructor')
    T.raises(function() o:__gc() end, 'nil value')
    T.eq(o.__gc, nil, 'o.__gc')
    T.eq(o.__index, nil, 'o.__index')
    T.eq(getmetatable(o), false, 'metatable is locked')
    T.raises(function() setmetatable(o, {}) end)
    o = nil
    collectgarbage('collect'); collectgarbage('collect')
end)
"""


@pytest.mark.parametrize("name", list(TYPES))
def test_metamethods_hidden(simulator, name):
    expr, req = TYPES[name]
    _stage(simulator, f"mt_{name}", HIDDEN % expr, req)
    run = run_lua_app(simulator, f"mt_{name}", timeout=20)
    run.assert_all_passed(["hidden"])


# ── Using an object after its own finaliser ran ─────────────────────────────

# arm(make, use) creates a holder with a finaliser, then the object (so the
# object is finalised first), and has the holder's finaliser call use(obj).
# The result of that call lands in R after a full collection.
AFTER = """
local R = {}
local function arm(make, use)
    local holder = setmetatable({}, {__gc = function(h)
        R.ran = true
        R.ok, R.err = pcall(use, h.obj, h.extra)
    end})
    holder.obj, holder.extra = make()
end
local function settle()
    for _ = 1, 3 do collectgarbage('collect') end
end
"""

DESTROYED = {
    "terminal": """
        arm(function() return pc.terminal.new(10, 5) end,
            function(t) return t:getCols() end)
    """,
    "image": """
        arm(function() return G.image.new(8, 8) end,
            function(img) img:draw(0, 0) end)
    """,
    "image_under_loop": """
        -- loop is marked before the holder, the image after it: the image
        -- is finalised first, then the holder draws through the live loop.
        local loop = G.animation.loop.new(100)
        local holder = setmetatable({}, {__gc = function(h)
            R.ran = true
            R.ok, R.err = pcall(h.loop.draw, h.loop, 0, 0)
        end})
        holder.loop = loop
        loop:setImageTable({G.image.new(8, 8)})
        loop, holder = nil, nil
    """,
    "modplayer": """
        arm(function() return pc.modplayer.create() end,
            function(m) return m:getVolume() end)
    """,
    "blinker": """
        arm(function() return G.animation.blinker.new() end,
            function(b) b:start() end)
    """,
    "sprite": """
        arm(function() return G.sprite.new() end,
            function(s) s:add() end)
    """,
    "image_under_sprite_setSourceRect": """
        -- sprite, then holder, then the image: the image is finalised
        -- first, then the holder crops through the live sprite.
        local s = G.sprite.new()
        local holder = setmetatable({}, {__gc = function(h)
            R.ran = true
            R.ok, R.err = pcall(h.s.setSourceRect, h.s, 0, 0, 8, 8)
        end})
        holder.s = s
        s:setImage(G.image.new(8, 8))
        s, holder = nil, nil
    """,
    "spritesheet": """
        arm(function() return G.spritesheet.newGrid(G.image.new(8, 8), 1, 1, 8, 8) end,
            function(ss) ss:drawFrame(0, 0, 0) end)
    """,
    "tilemap": """
        arm(function() return G.tilemap.new(G.image.new(8, 8), 8, 8) end,
            function(tm) tm:draw(0, 0) end)
    """,
    "font": """
        arm(function() return G.font.new('6x8') end,
            function(f) return f:getHeight() end)
    """,
    "animator": """
        arm(function() return G.animation.animator.new(100, 0, 1) end,
            function(a) return a:currentValue() end)
    """,
    "fileplayer": """
        arm(function() return pc.sound.fileplayer() end,
            function(fp) fp:load(APP_DIR .. '/s.wav') end)
    """,
    "mp3player": """
        arm(function() return pc.sound.mp3player() end,
            function(mp) return mp:isPlaying() end)
    """,
}


@pytest.mark.parametrize("name", list(DESTROYED))
def test_destroyed_object_errors(simulator, name):
    body = AFTER + (
        "T.case('destroyed', function()\n"
        + DESTROYED[name] +
        "    settle()\n"
        "    T.ok(R.ran, 'the holder finaliser never ran')\n"
        "    T.eq(R.ok, false, 'using a destroyed object succeeded')\n"
        "    T.ok(tostring(R.err):find('destroyed') or tostring(R.err):find('freed'),\n"
        "         'error: ' .. tostring(R.err))\n"
        "end)\n")
    _stage(simulator, f"dead_{name}", body, ("audio",))
    run = run_lua_app(simulator, f"dead_{name}", timeout=20)
    run.assert_all_passed(["destroyed"])


# ── modplayer: one player, however many times create() is called ───────────

SINGLETON = """
T.case('create_twice', function()
    local m1 = T.ok(pc.modplayer.create(), 'create 1')
    T.ok(m1:load(APP_DIR .. '/m.mod'), 'load')
    m1:setVolume(37)
    m1:play(true)
    T.ok(m1:isPlaying(), 'playing')
    local m2 = T.ok(pc.modplayer.create(), 'create 2')
    T.ok(rawequal(m1, m2), 'create() returns the one live player')
    m2 = nil
    collectgarbage('collect'); collectgarbage('collect')
    T.ok(m1:isPlaying(), 'collecting the second handle stopped the first')
    T.eq(m1:getVolume(), 37)
    m1:stop()
end)
T.case('recreate_after_collect', function()
    -- Once every handle is gone the player is released; a later create()
    -- gets a fresh one, and the old finaliser does not tear it down.
    collectgarbage('collect'); collectgarbage('collect')
    local m = T.ok(pc.modplayer.create(), 'create')
    T.eq(m:getVolume(), 100)
    T.ok(m:load(APP_DIR .. '/m.mod'), 'load')
    m:play(true)
    collectgarbage('collect'); collectgarbage('collect')
    T.ok(m:isPlaying(), 'still playing')
    m:stop()
end)
T.case('stale_finaliser', function()
    -- m1 is unreachable (so gone from create()'s weak cache) but not yet
    -- finalised when a later finaliser calls create(): m2 is a new owner,
    -- and m1's finaliser, which runs after, must leave m2's player alone.
    local R = {}
    do
        local m1 = pc.modplayer.create()
        local holder = setmetatable({}, {__gc = function()
            local m2 = pc.modplayer.create()
            R.fresh = not rawequal(m2, m1)
            m2:load(APP_DIR .. '/m.mod')
            m2:setVolume(55)
            m2:play(true)
            R.m2 = m2
        end})
        holder.m1 = m1   -- holder is marked after m1: its finaliser runs first
    end
    collectgarbage('collect'); collectgarbage('collect'); collectgarbage('collect')
    T.ok(R.m2, 'the holder finaliser never ran')
    T.ok(R.fresh, 'create() returned the collected handle')
    T.ok(R.m2:isPlaying(), "m1's finaliser stopped m2")
    T.eq(R.m2:getVolume(), 55)
    T.ok(rawequal(pc.modplayer.create(), R.m2), 'm2 is the live handle')
    R.m2:stop()
end)
"""


def test_modplayer_create_is_shared(simulator):
    _stage(simulator, "mod_single", SINGLETON, ("audio",))
    run = run_lua_app(simulator, "mod_single", timeout=20)
    run.assert_all_passed(["create_twice", "recreate_after_collect",
                           "stale_finaliser"])
