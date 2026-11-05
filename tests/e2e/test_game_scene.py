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

ERRORS = {
    "update": 'scene.add("s", { update = function(dt) error("boom update") end })\n'
              'scene.switch("s")\nscene.update(0.1)\n',
    "draw": 'scene.add("s", { draw = function() error("boom draw") end })\n'
            'scene.switch("s")\nscene.draw()\n',
    "enter": 'scene.add("s", { enter = function() error("boom enter") end })\n'
             'scene.switch("s")\n',
    "exit": 'scene.add("s", { exit = function() error("boom exit") end })\n'
            'scene.add("t", {})\nscene.switch("s")\nscene.switch("t")\n',
    "push": 'scene.add("s", {})\nscene.add("t", { enter = function() error("boom push") end })\n'
            'scene.switch("s")\nscene.push("t")\n',
    "pop": 'scene.add("s", {})\nscene.add("t", { exit = function() error("boom pop") end })\n'
           'scene.switch("s")\nscene.push("t")\nscene.pop()\n',
}


@pytest.mark.sd(fixtures=[])
@pytest.mark.parametrize("where", sorted(ERRORS))
def test_a_scene_error_stops_the_app_like_any_lua_error(simulator, test_sd_card, where):
    """They went to printf only: on a device with no serial attached, a scene
    bug was a stuck screen with nothing in the error log."""
    out, lines = run(simulator, test_sd_card,
                     ERRORS[where] + 'log("STILL RUNNING")\n')
    assert out.get("result") == "error", (out, lines)
    assert f"boom {where}" in (out.get("error") or ""), out
    assert not any("STILL RUNNING" in line for line in lines), lines
    record = (test_sd_card / "system" / "error.log").read_text()
    assert f"boom {where}" in record, record


@pytest.mark.sd(fixtures=[])
def test_an_app_can_catch_a_scene_error(simulator, test_sd_card):
    """pcall around scene.update sees it, and the scene stays current."""
    lines = ok(simulator, test_sd_card, r"""
scene.add("s", { update = function(dt) error("boom") end })
scene.switch("s")
local okp, err = pcall(scene.update, 0.1)
log("CAUGHT " .. tostring(okp) .. " " .. tostring(err))
log("CURRENT " .. tostring(scene.getCurrent()))
""")
    assert value(lines, "CAUGHT").startswith("false ") and "boom" in value(lines, "CAUGHT")
    assert value(lines, "CURRENT") == "s"


@pytest.mark.sd(fixtures=[])
def test_exit_still_unwinds_a_scene(simulator, test_sd_card):
    """sys.exit() inside a scene function ends the app as an exit, not an error."""
    out, lines = run(simulator, test_sd_card, r"""
scene.add("s", { update = function(dt) picocalc.sys.exit() end })
scene.switch("s")
scene.update(0.1)
log("STILL RUNNING")
""")
    assert out.get("result") == "exit_sentinel", (out, lines)
    assert not any("STILL RUNNING" in line for line in lines), lines


# ── What the page says about leaving ─────────────────────────────────────────

@pytest.mark.sd(fixtures=[])
def test_nothing_runs_at_exit_app(simulator, test_sd_card):
    """The page tells developers to save when data changes. This is why."""
    code = r"""
local save = picocalc.game.save
scene.add("s", { exit = function() save.set("from_exit", { a = 1 }) end })
scene.switch("s")
local guard <close> = setmetatable({}, { __close = function() save.set("from_close", { a = 1 }) end })
log("RUNNING")
while true do picocalc.input.update(); picocalc.sys.sleep(5) end
"""
    stage_lua_app(test_sd_card, "scene_exit", SCENE + code)
    simulator.launch_app("scene_exit")
    simulator.wait_for_log("RUNNING", timeout=10)
    simulator.exit_app()
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "exit_sentinel", out
    saves = test_sd_card / "data" / "com.test.scene_exit" / "saves"
    assert not (saves / "from_exit.json").exists()
    assert not (saves / "from_close.json").exists()


def doc_example():
    """The last ```lua block of the page, which is the complete app."""
    text = (PROJECT_ROOT / "docs" / "API-Game.md").read_text()
    section = text.split("## Example\n", 1)[1]
    return section.split("```lua\n")[2].split("```", 1)[0]


def doc_snippet_after(heading):
    text = (PROJECT_ROOT / "docs" / "API-Game.md").read_text()
    return text.split(heading, 1)[1].split("```lua\n", 1)[1].split("```", 1)[0]


@pytest.mark.sd(fixtures=[])
def test_doc_example_verbatim(simulator, test_sd_card):
    """The page's complete example: the camera follows, the score saves as soon
    as it is beaten, pause does not reset the game, Esc quits."""
    stage_lua_app(test_sd_card, "scene_doc", doc_example())
    simulator.launch_app("scene_doc")
    simulator.wait_frames(3)
    start = simulator.screenshot_pil().convert("RGB")

    highscore = test_sd_card / "data" / "com.test.scene_doc" / "saves" / "highscore.json"
    assert not highscore.exists()
    simulator.pad("right")
    deadline = time.time() + 20
    while time.time() < deadline and not highscore.exists():
        time.sleep(0.05)
    simulator.pad("none")
    assert highscore.exists(), "the coin at x=260 never saved"
    assert '"score"' in highscore.read_text() and "1" in highscore.read_text()

    # The camera kept the player near the screen's centre (it started at 160).
    time.sleep(0.5)
    img = simulator.screenshot_pil().convert("RGB")
    assert img != start
    red = [(x, y) for y in range(320) for x in range(320) if img.getpixel((x, y))[0] > 200
           and img.getpixel((x, y))[1] < 60 and img.getpixel((x, y))[2] < 60]
    assert red and 140 < red[0][0] < 180, red[:3]

    # Pause: pop runs play.enter again, and the score must survive it.
    before = highscore.read_text()
    seq = simulator.keypress("enter")["input_seq"]
    simulator.wait_input_consumed(seq)
    time.sleep(0.2)
    paused = simulator.screenshot_pil().convert("RGB")
    seq = simulator.keypress("enter")["input_seq"]
    simulator.wait_input_consumed(seq)
    time.sleep(0.2)
    resumed = simulator.screenshot_pil().convert("RGB")
    assert paused != resumed
    assert resumed.crop((0, 0, 320, 12)) == img.crop((0, 0, 320, 12)), "the score was reset"
    assert highscore.read_text() == before

    simulator.keypress("esc")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out


@pytest.mark.sd(fixtures=[])
def test_doc_intro_snippet_runs(simulator, test_sd_card):
    """The scene page's first snippet, in a loop of ours."""
    code = doc_snippet_after("**They are called as plain functions") + r"""
local scene = picocalc.game.scene
for i = 1, 10 do
    scene.update(0.5)
    scene.draw()
    picocalc.display.flush()
end
picocalc.sys.log("T " .. tostring(play.t))
"""
    stage_lua_app(test_sd_card, "scene_intro", code)
    simulator.launch_app("scene_intro")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out
    assert any("T 5.0" in e["text"] for e in simulator.get_log_lines(0))


@pytest.mark.sd(fixtures=[])
def test_camera_model(simulator, test_sd_card):
    """Position is the world point at the screen's centre; bounds apply in update."""
    lines = ok(simulator, test_sd_card, r"""
local c = picocalc.game.camera.new()
log("OFF0 " .. table.concat({c:getOffset()}, " "))
c:setPosition(100, 50)
log("OFF1 " .. table.concat({c:getOffset()}, " "))
log("W2S " .. table.concat({c:worldToScreen(100, 50)}, " "))
c:setZoom(100); log("Z1 " .. c:getZoom())
c:setZoom(0); log("Z2 " .. c:getZoom())
c:setZoom(1)
c:setPosition(-500, -500); c:setBounds(0, 0, 1024, 1024)
log("B0 " .. table.concat({c:getPosition()}, " "))
c:update(0.016)
log("B1 " .. table.concat({c:getPosition()}, " "))
c:setBounds(0, 0, 100, 100); c:update(0.016)
log("B2 " .. table.concat({c:getPosition()}, " "))
c:clearBounds()
c:setPosition(0, 0); c:setTarget(100, 0, 0.5); c:update(0.5)
log("LAG " .. math.floor((c:getPosition())))
""")
    assert value(lines, "OFF0") == "160 160"
    assert value(lines, "OFF1") == "60 110"
    assert value(lines, "W2S") == "160 160"
    assert value(lines, "Z1") == "10.0" and value(lines, "Z2").startswith("0.1")
    assert value(lines, "B0") == "-500.0 -500.0"
    assert value(lines, "B1") == "160.0 160.0"
    assert value(lines, "B2") == "-60.0 -60.0"
    assert value(lines, "LAG") == "63"


@pytest.mark.sd(fixtures=[])
def test_save_slot_key_shapes(simulator, test_sd_card):
    lines = ok(simulator, test_sd_card, r"""
local sv = picocalc.game.save
sv.set("arr", {1, 2, 3}); local a = sv.get("arr")
log("ARR " .. math.type(next(a)) .. " " .. #a)
sv.set("sparse", {[5] = 1}); log("SPARSE " .. type((next(sv.get("sparse")))))
sv.set("mixed", {1, a = 2}); local m = sv.get("mixed")
log("MIXED " .. tostring(m[1]) .. " " .. tostring(m["1"]))
log("NUM " .. tostring((pcall(sv.set, "n", 5))))
log("FN " .. tostring((pcall(sv.set, "f", {f = print}))))
local x, y = sv.set("../x", {}); log("BAD " .. tostring(x) .. " " .. tostring(y))
log("OK " .. tostring(sv.set("fine", {})))
""")
    assert value(lines, "ARR") == "integer 3"
    assert value(lines, "SPARSE") == "string"
    assert value(lines, "MIXED") == "nil 1"
    assert value(lines, "NUM") == "false"
    assert value(lines, "FN") == "false"
    assert value(lines, "BAD") == "false invalid save name"
    assert value(lines, "OK") == "true"
