"""picocalc.game.scene (src/os/lua_bridge_game_scene.c): the calling convention
apps rely on, the stack, and object pools."""

import time

import pytest

from helpers import PROJECT_ROOT, stage_lua_app

SCENE = "local scene, log = picocalc.game.scene, picocalc.sys.log\n"


def run(sim, sd, code, name="scene_app", timeout=20):
    stage_lua_app(sd, name, SCENE + code)
    sim.launch_app(name)
    out = sim.wait_for_exit(timeout=timeout)
    return out, [e["text"] for e in sim.get_log_lines(0)]


def ok(sim, sd, code):
    out, lines = run(sim, sd, code)
    assert out.get("result") == "returned", (out, lines)
    return lines


def value(lines, key):
    for line in lines:
        if line.startswith(key + " ") or (" " + key + " ") in line:
            return line.split(key + " ", 1)[1]
    raise AssertionError(f"{key} not logged: {lines}")


# ── The calling convention: plain functions, no self ─────────────────────────

@pytest.mark.sd(fixtures=[])
def test_lifecycle_functions_get_no_self(simulator, test_sd_card):
    """Shipped apps (guinea_pig, platformer_demo, brick15's adapter) rely on
    this: update(dt), draw(), enter(), exit()."""
    lines = ok(simulator, test_sd_card, r"""
local s = {}
function s.enter(...) log("ARGS enter " .. select("#", ...)) end
function s.exit(...) log("ARGS exit " .. select("#", ...)) end
function s.update(...) log("ARGS update " .. select("#", ...) .. " " .. tostring((...))) end
function s.draw(...) log("ARGS draw " .. select("#", ...)) end
scene.add("s", s)
scene.switch("s")
scene.update(0.5)
scene.draw()
scene.update()
scene.switch("s")
""")
    text = "\n".join(lines)
    assert "ARGS enter 0" in text and "ARGS exit 0" in text and "ARGS draw 0" in text
    assert "ARGS update 1 0.5" in text
    assert "ARGS update 1 0.016" in text   # the default dt


@pytest.mark.sd(fixtures=[])
def test_stack_order_and_what_runs(simulator, test_sd_card):
    lines = ok(simulator, test_sd_card, r"""
local ev = {}
local function make(n)
    return {
        enter = function() ev[#ev + 1] = n .. ".enter" end,
        exit = function() ev[#ev + 1] = n .. ".exit" end,
        update = function() ev[#ev + 1] = n .. ".update" end,
        draw = function() ev[#ev + 1] = n .. ".draw" end,
    }
end
log("TYPE " .. type(scene.new()))
log("NONE " .. tostring(scene.getCurrent()))
scene.update(); scene.draw()       -- no scene yet: nothing runs
scene.add("a", make("a"))
scene.add("b", make("b"))
scene.switch("a")
scene.push("b")
log("CUR " .. tostring(scene.getCurrent()) .. " " .. type(scene.getCurrent()))
scene.update(); scene.draw()
scene.pop()
log("CUR2 " .. tostring(scene.getCurrent()))
log("EV " .. table.concat(ev, " "))
local okp, err = pcall(scene.pop)
log("POPLAST " .. tostring(okp) .. " " .. tostring(err))
scene.push("b"); scene.push("b"); scene.push("b")
okp, err = pcall(scene.push, "b")
log("DEEP " .. tostring(okp) .. " " .. tostring(err))
okp, err = pcall(scene.switch, "nope")
log("NOSCENE " .. tostring(okp) .. " " .. tostring(err))
for i = 1, 14 do scene.add("x" .. i, {}) end
okp, err = pcall(scene.add, "over", {})
log("MANY " .. tostring(okp) .. " " .. tostring(err))
""")
    assert value(lines, "TYPE") == "table"
    assert value(lines, "NONE") == "nil"
    assert value(lines, "CUR") == "b string"
    assert value(lines, "CUR2") == "a"
    # Only the top scene runs, and pop runs the revealed scene's enter again.
    assert value(lines, "EV") == "a.enter b.enter b.update b.draw b.exit a.enter"
    assert "cannot pop last scene" in value(lines, "POPLAST")
    assert "scene stack overflow (max 4)" in value(lines, "DEEP")
    assert "scene 'nope' not found" in value(lines, "NOSCENE")
    assert "too many scenes (max 16)" in value(lines, "MANY")


@pytest.mark.sd(fixtures=[])
def test_removing_a_scene_takes_it_off_the_stack(simulator, test_sd_card):
    """remove left its registry ref on the stack, where the next luaL_ref (an
    object released to a pool, a new scene) made it some other table."""
    lines = ok(simulator, test_sd_card, r"""
local ran = 0
local a = { update = function() ran = ran + 1 end }
scene.add("a", {})
scene.add("b", a)
scene.switch("a")
scene.push("b")
scene.update()
scene.remove("b")
log("REMOVED " .. tostring(scene.getCurrent()) .. " " .. ran)
scene.update()
log("AFTER " .. ran)
scene.remove("a")
log("EMPTY " .. tostring(scene.getCurrent()))
local stray = { update = function() ran = ran + 100 end }
local pool = scene.objectPool("p", function() return {} end)
pool:release(stray)
scene.add("c", {})
scene.update()
log("STRAY " .. ran)
""")
    assert value(lines, "REMOVED") == "a 1"
    assert value(lines, "AFTER") == "1"
    assert value(lines, "EMPTY") == "nil"
    assert value(lines, "STRAY") == "1"


@pytest.mark.sd(fixtures=[])
def test_adding_an_existing_name_replaces_it_when_full(simulator, test_sd_card):
    lines = ok(simulator, test_sd_card, r"""
for i = 1, 16 do scene.add("x" .. i, {}) end
local ran = 0
scene.switch("x1")
scene.add("x1", { update = function() ran = ran + 1 end })
scene.update()
log("REPLACED " .. ran .. " " .. tostring(scene.getCurrent()))
local okp, err = pcall(scene.add, string.rep("n", 64), {})
log("LONG " .. tostring(okp) .. " " .. tostring(err))
okp, err = pcall(scene.add, string.rep("n", 63), {})
log("LONG63 " .. tostring(okp) .. " " .. tostring(err))
""")
    assert value(lines, "REPLACED") == "1 x1"
    assert "scene name too long (max 63 bytes)" in value(lines, "LONG")
    assert "too many scenes" in value(lines, "LONG63")   # the name was fine


# ── Object pools ─────────────────────────────────────────────────────────────

@pytest.mark.sd(fixtures=[])
def test_pool_reuses_past_64_releases(simulator, test_sd_card):
    """acquire left a hole and release always appended, so after 64 releases
    the pool looked full and dropped everything."""
    lines = ok(simulator, test_sd_card, r"""
local made = 0
local pool = scene.objectPool("p", function() made = made + 1; return { id = made } end)
local o = pool:acquire()
for i = 1, 200 do
    pool:release(o)
    o = pool:acquire()
end
log("MADE " .. made .. " " .. o.id)
""")
    assert value(lines, "MADE") == "1 1"


@pytest.mark.sd(fixtures=[])
def test_pool_holds_64_idle_objects(simulator, test_sd_card):
    lines = ok(simulator, test_sd_card, r"""
local made = 0
local pool = scene.objectPool("p", function() made = made + 1; return {} end)
local out = {}
for i = 1, 70 do out[i] = pool:acquire() end
log("A " .. made)
for i = 1, 70 do pool:release(out[i]) end       -- six are dropped
for i = 1, 70 do out[i] = pool:acquire() end
log("B " .. made)                               -- the six come from the factory
for i = 1, 70 do pool:release(out[i]) end
pool:clear()
pool:acquire()
log("C " .. made)
pool:prewarm(10)
local before = made
for i = 1, 10 do pool:acquire() end
log("D " .. (made - before) .. " " .. (before - made + 10))
pool:prewarm(1000)
log("E " .. (made - before))                    -- clamped to 64
""")
    assert value(lines, "A") == "70"
    assert value(lines, "B") == "76"
    assert value(lines, "C") == "77"
    assert value(lines, "D").split()[0] == "0"
    assert value(lines, "E") == "64"


@pytest.mark.sd(fixtures=[])
def test_pool_factory_errors_raise(simulator, test_sd_card):
    """A factory's error came back as the object: a string."""
    lines = ok(simulator, test_sd_card, r"""
local pool = scene.objectPool("p", function() error("factory failed") end)
local okp, err = pcall(pool.acquire, pool)
log("ACQ " .. tostring(okp) .. " " .. tostring(err))
okp, err = pcall(pool.prewarm, pool, 3)
log("PRE " .. tostring(okp) .. " " .. tostring(err))
""")
    assert value(lines, "ACQ").startswith("false ") and "factory failed" in value(lines, "ACQ")
    assert value(lines, "PRE").startswith("false ") and "factory failed" in value(lines, "PRE")


@pytest.mark.sd(fixtures=[])
def test_pool_field_cannot_point_anywhere(simulator, test_sd_card):
    """_pool is a plain field of a plain table: a method takes it as a pointer
    into C state only if it is one of the pools."""
    lines = ok(simulator, test_sd_card, r"""
local pool = scene.objectPool("p", function() return {} end)
pool._pool = picocalc.display.getBackBuffer()
local okp, err = pcall(pool.clear, pool)
log("BAD " .. tostring(okp) .. " " .. tostring(err))
""")
    assert value(lines, "BAD").startswith("false ") and "invalid pool" in value(lines, "BAD")


# ── A scene's error is the app's error ───────────────────────────────────────

# ── What the page says about leaving ─────────────────────────────────────────
