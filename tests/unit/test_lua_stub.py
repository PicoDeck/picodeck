"""sdk/lua/picocalc.lua must match the bridge. Run: python3 -m pytest tests/unit/test_lua_stub.py -v

tools/check_lua_stub.py compares every stub signature with the C function
behind the same luaL_Reg entry, and every name the firmware registers with the
stub. The mutation tests put back the six wrong signatures issue #37 reported,
take entries out of the stub and add registrations to a copy of the bridge, and
prove the checker sees each one.
"""
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_lua_stub as cls  # noqa: E402

STUB = cls.STUB.read_text()
ALLOW_KEYS = tuple(cls.ALLOW)


def test_stub_matches_the_bridge():
    assert cls.check(STUB) == []


def test_every_owner_table_exists_in_the_bridge():
    # A renamed C table would silently drop its functions from the check.
    found = {(p.name, t) for p in cls.BRIDGE_DIR.glob("lua_bridge*.c")
             for t in cls.reg_tables(cls.strip_comments(p.read_text()))}
    assert set(cls.OWNERS) <= found


def test_allowlist_names_real_stub_functions():
    assert set(cls.ALLOW) <= set(cls.parse_stub(STUB))


def test_every_allow_entry_is_still_needed(monkeypatch):
    # An entry the heuristics no longer trip on only hides future drift.
    monkeypatch.setattr(cls, "ALLOW", {})
    problems = cls.check(STUB)
    for key in ALLOW_KEYS:
        assert any(f"{key}(" in p for p in problems), f"{key} no longer needs ALLOW"


def test_unstubbed_names_are_registered_and_really_have_no_entry():
    fw, stub = cls.firmware_surface(), cls.stub_surface(STUB)
    for name in cls.UNSTUBBED:
        assert name in fw.names, f"{name} is not registered: drop it from UNSTUBBED"
        assert not stub.has(name), f"{name} has a stub entry: drop it from UNSTUBBED"


def test_the_walk_reaches_everything_registered():
    names = cls.firmware_surface().names
    for name in ("picocalc.display.clear", "picocalc.display.BLACK",     # sub-table, constant
                 "picocalc.graphics.animation.loop.new",                 # nested tables
                 "picocalc.game.scene.objectPool",                       # lua_settable
                 "picocalc.chargrid.DOT1",                               # a constants loop
                 "picocalc.sys.applyUpdate", "picocalc.sysconfig.get",   # behind a requirement
                 "picocalc.appconfig.get", "picocalc.json.null",
                 "require", "draw3DWireframeEx", "APP_DIR"):             # globals
        assert name in names, name
    assert names["dofile"].kind == "nil"  # removed, so nothing to stub


def test_the_header_says_what_the_stub_is():
    head = STUB[:STUB.index("App globals")]
    assert "Generated" not in head and "Hand-written" in head


# (what the stub said before the fix, what it says now, text the report names)
ORIGINAL_BUGS = {
    "drawScaled": (
        "function PicoDeckImage:drawScaled(x, y, scale, angle) end",
        "function PicoDeckImage:drawScaled(x, y, dst_w, dst_h) end",
        "drawScaled"),
    "drawScaledNN": (
        "function PicoDeckImage:drawScaledNN(x, y, scale) end",
        "function PicoDeckImage:drawScaledNN(x, y, dst_w, dst_h) end",
        "drawScaledNN"),
    "newGrid": (
        "function picocalc.graphics.spritesheet.newGrid(image, cols, rows, frame_w, frame_h) end",
        "function picocalc.graphics.spritesheet.newGrid(image, frame_w, frame_h) end",
        "newGrid"),
    "loop.new": (
        "function picocalc.graphics.animation.loop.new(interval_ms, frames, looping) end",
        "function picocalc.graphics.animation.loop.new(spritesheet, frame_duration_ms) end",
        "loop.new"),
    "animator.new": (
        "function picocalc.graphics.animation.animator.new(duration_ms, from, to, easing, delay_ms) end",
        "function picocalc.graphics.animator.new(from, to, duration_ms, easing_fn) end",
        "animator.new"),
    "setTarget": (
        "function PicoDeckCamera:setTarget(x, y, lag) end",
        "function PicoDeckCamera:setTarget(target) end",
        "setTarget"),
}


@pytest.mark.parametrize("name", ORIGINAL_BUGS)
def test_original_bug_is_caught(name):
    fixed, broken, needle = ORIGINAL_BUGS[name]
    assert STUB.count(fixed) == 1, "the fixed entry moved; update this test"
    problems = cls.check(STUB.replace(fixed, broken))
    assert any(needle in p for p in problems), problems


def test_dropped_optional_param_is_caught():
    fixed = "function PicoDeckSprite:draw(x, y) end"
    assert cls.check(STUB.replace(fixed, "function PicoDeckSprite:draw() end"))


def test_wrong_return_count_is_caught():
    fixed = "---@return integer[] point `{cx, cy}`: one table, not two values"
    two = "---@return integer cx\n---@return integer cy"
    assert any("getCenterPoint" in p for p in cls.check(STUB.replace(fixed, two)))


def test_optional_marked_required_is_caught():
    fixed = "---@param factory fun(): any Factory function for new objects"
    problems = cls.check(STUB.replace(fixed, "---@param factory? fun(): any Factory"))
    assert any("objectPool" in p for p in problems)


def test_raising_returns_do_not_hide_the_real_return_count():
    # l_crypto_randomBytes: `return luaL_error(...)` on failure, `return 1` on success.
    fixed = "---@param n integer\n---@return string bytes\nfunction picocalc.crypto.randomBytes"
    assert STUB.count(fixed) == 1
    broken = fixed.replace("---@return string bytes\n", "")
    assert any("randomBytes" in p for p in cls.check(STUB.replace(fixed, broken)))


def test_one_line_return_list_counts_each_value():
    assert cls._return_count("---@return integer w, integer h") == 2
    assert cls._return_count("---@return string? error `\"a\"`, `\"b\"` or `\"c\"`") == 1
    assert cls._return_count("---@return {a: integer, b: integer} t") == 1


def test_c_scanning_ignores_braces_and_comment_markers_in_literals():
    src = 'static int f(lua_State *L) {\n  puts("}} // x");\n  return 1;\n}\nint g(void) { return 2; }\n'
    fns = cls.c_functions(cls.strip_comments(src))
    assert "return 1;" in fns["f"] and "return 2" not in fns["f"] and "g" in fns


def test_checkoption_with_a_default_is_optional():
    body = '  int w = luaL_checkoption(L, 3, "set", opts);\n  luaL_checkinteger(L, 2);\n'
    f = cls.analyse_c(body, method=False)
    assert f["total"] == 3 and f["required"] == 2 and f["nonopt"] == 2
    f = cls.analyse_c(body.replace('"set"', "NULL"), method=False)
    assert f["required"] == 3


def test_return_through_a_helper_with_more_arguments_is_counted():
    funcs = {"push_no_voice": "  return 2;\n", "temp_play": "  if (x) return push_no_voice(L);\n  return 1;\n"}
    assert cls.own_returns("  return temp_play(L, s, 1, 100, rate);\n", funcs) == {1, 2}
    assert cls.own_returns("  return ok ? 1 : 2;\n", {}) == {1, 2}
    assert cls.own_returns("  return lua_gettop(L);\n", {}) == {None}


def test_short_return_list_is_caught():
    # fs.glob returns {}, "permission denied": a stub with only the list loses the error.
    line = "---@return string? error `\"permission denied\"` when the sandbox refused it (the list is then empty)\n"
    assert STUB.count(line) == 1
    assert any("picocalc.fs.glob" in p for p in cls.check(STUB.replace(line, "")))


# ── bridge to stub ───────────────────────────────────────────────────────────

@pytest.mark.parametrize("name, entry", [
    ("picocalc.ui.toast", "function picocalc.ui.toast(text, style) end"),
    ("picocalc.audio.ringFree", "function picocalc.audio.ringFree() end"),
    ("PicoDeckSprite.setGroups", "function PicoDeckSprite:setGroups(groups) end"),
    ("PicoDeckCharGrid.draw", "function PicoDeckCharGrid:draw(px, py, sx, sy, w, h) end"),
    ("PicoDeckObjectPool.acquire", "function PicoDeckObjectPool:acquire() end"),
    ("PicoDeckFramebuffer.width", "function PicoDeckFramebuffer:width() end"),
    ("require", "function require(name) end"),
    ("picocalc.tcp.CB_READ", "---@field CB_READ    integer `getEvents()` bit: data arrived (2)"),
    ("picocalc.chargrid.DOT1", "---@field DOT1 integer Dots of growing size: `·`"),
    ("picocalc.ui.TOAST_ERROR", "picocalc.ui.TOAST_ERROR = 3"),
    ("PicoDeckSprite.scale_nn", "---@field scale_nn integer Integer nearest-neighbour scale, as `setScaleNN`"),
    ("PicoDeckAnimator.reverses", "---@field reverses boolean Play back to `from` after reaching `to` (default false)"),
    ("picocalc.appconfig.get", "picocalc.appconfig = picocalc.config"),
])
def test_entry_taken_out_of_the_stub_is_caught(name, entry):
    assert STUB.count(entry) == 1, "the entry moved; update this test"
    problems = cls.check(STUB.replace(entry, ""))
    assert any(f"registers {name}:" in p for p in problems), problems


@pytest.mark.parametrize("name, after, added", [
    ("picocalc.tcp.NOPE", "---@class picocalc.tcp\n", "---@field NOPE integer\n"),
    ("PicoDeckSprite.health", "---@field tag integer\n", "---@field health integer\n"),
    ("picocalc.ui.NOPE", "picocalc.ui.TOAST_INFO = 0\n", "picocalc.ui.NOPE = 9\n"),
    ("NOPE_GLOBAL", "APP_DIR = \"\"\n", "NOPE_GLOBAL = \"\"\n"),
])
def test_entry_with_nothing_behind_it_is_caught(name, after, added):
    assert STUB.count(after) >= 1
    problems = cls.check(STUB.replace(after, after + added, 1))
    assert any(name in p for p in problems), problems


def bridge_copy(tmp_path, file, old, new):
    """A copy of the bridge sources with one edit, as a new registration would make."""
    for p in list(cls.BRIDGE_DIR.glob("lua_bridge*.c")) + [cls.BRIDGE_DIR / "lua_runner.c"]:
        shutil.copy(p, tmp_path / p.name)
    path = tmp_path / file
    text = path.read_text()
    assert text.count(old) >= 1, f"{old!r} moved in {file}; update this test"
    path.write_text(text.replace(old, new, 1))
    return tmp_path


def test_unedited_copy_of_the_bridge_passes(tmp_path):
    assert cls.check(STUB, bridge_copy(tmp_path, "lua_bridge_audio.c", "", "")) == []


@pytest.mark.parametrize("file, old, new, needle", [
    ("lua_bridge_audio.c", '{"ringFree", l_audio_ringFree},',
     '{"ringFree", l_audio_ringFree}, {"newThing", l_audio_ringFree},', "picocalc.audio.newThing"),
    ("lua_bridge_sound.c", '{"setRate", l_sound_fileplayer_setRate},',
     '{"setRate", l_sound_fileplayer_setRate}, {"newThing", l_sound_fileplayer_getRate},',
     "PicoDeckFilePlayer.newThing"),
    ("lua_bridge_tcp.c", 'lua_setfield(L, -2, "CB_CONNECT");',
     'lua_setfield(L, -2, "CB_CONNECT"); lua_pushinteger(L, 32); lua_setfield(L, -2, "CB_NEW");',
     "picocalc.tcp.CB_NEW"),
    ("lua_bridge_chargrid.c", '{"BOX_ASCII", CG_STYLE_ASCII},',
     '{"BOX_ASCII", CG_STYLE_ASCII}, {"BOX_NEW", 3},', "picocalc.chargrid.BOX_NEW"),
    ("lua_bridge_graphics.c", '!strcmp(key, "tag")) {\n    lua_pushinteger(L, s->tag);',
     '!strcmp(key, "health")) {\n    lua_pushinteger(L, 1);\n  } else if (!strcmp(key, "tag")) {\n'
     '    lua_pushinteger(L, s->tag);', "PicoDeckSprite.health"),
    ("lua_bridge_3d.c", 'lua_setglobal(L, "draw3DWireframeEx");',
     'lua_setglobal(L, "draw3DWireframeEx");\n    lua_pushcfunction(L, l_graphics_draw3DWireframeEx);\n'
     '    lua_setglobal(L, "newGlobal");', "newGlobal"),
    ("lua_runner.c", 'lua_setglobal(L, "APP_DIR");',
     'lua_setglobal(L, "APP_DIR");\n  lua_pushstring(L, app->path);\n  lua_setglobal(L, "APP_NEW");', "APP_NEW"),
])
def test_new_name_in_the_bridge_is_caught(tmp_path, file, old, new, needle):
    problems = cls.check(STUB, bridge_copy(tmp_path, file, old, new))
    assert any(f"registers {needle}:" in p for p in problems), problems


def test_new_module_table_is_caught(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_audio.c",
                         'void lua_bridge_audio_init(lua_State *L) { register_subtable(L, "audio", l_audio_lib); }',
                         'static const luaL_Reg l_new_lib[] = {{"f", l_audio_ringFree}, {NULL, NULL}};\n'
                         'void lua_bridge_audio_init(lua_State *L) { register_subtable(L, "audio", l_audio_lib);\n'
                         '  register_subtable(L, "newmod", l_new_lib); }')
    problems = cls.check(STUB, bridge)
    assert any("registers picocalc.newmod.f:" in p for p in problems), problems
    assert any("l_new_lib is registered as picocalc.newmod and not in OWNERS" in p for p in problems), problems


def test_new_object_type_is_caught(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_audio.c",
                         'void lua_bridge_audio_init(lua_State *L) { register_subtable(L, "audio", l_audio_lib); }',
                         'static const luaL_Reg l_new_methods[] = {{"f", l_audio_ringFree}, {NULL, NULL}};\n'
                         'void lua_bridge_audio_init(lua_State *L) { lb_register_type(L, "new.mt", l_new_methods, NULL);\n'
                         '  register_subtable(L, "audio", l_audio_lib); }')
    problems = cls.check(STUB, bridge)
    assert any("l_new_methods is the method table of metatable" in p for p in problems), problems


def test_table_registered_under_another_name_is_caught(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_perf.c", 'register_subtable(L, "perf", l_perf_lib);',
                         'register_subtable(L, "perf2", l_perf_lib);')
    problems = cls.check(STUB, bridge)
    assert any("OWNERS says lua_bridge_perf.c l_perf_lib is picocalc.perf" in p for p in problems), problems


def test_table_no_longer_registered_is_caught(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_perf.c", 'register_subtable(L, "perf", l_perf_lib);', ";")
    problems = cls.check(STUB, bridge)
    assert any("OWNERS lists lua_bridge_perf.c l_perf_lib" in p for p in problems), problems


def test_registration_the_walk_cannot_read_is_reported_not_skipped(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_tcp.c", 'lua_setfield(L, -2, "CB_CONNECT");',
                         'lua_setfield(L, -2, some_name);')
    problems = cls.check(STUB, bridge)
    assert any("cannot read lua_bridge_register()" in p for p in problems), problems


def test_unbalanced_registration_is_reported_not_skipped(tmp_path):
    bridge = bridge_copy(tmp_path, "lua_bridge_tcp.c", 'lua_setfield(L, -2, "CB_CONNECT");', "")
    problems = cls.check(STUB, bridge)
    assert any("cannot read lua_bridge_register()" in p for p in problems), problems


# ── annotations ──────────────────────────────────────────────────────────────

def test_parameter_without_annotation_is_caught():
    block = "---@param px number\n---@param py number\n---@param sx? number default 0\n"
    assert STUB.count(block) == 1
    problems = cls.check(STUB.replace(block, "---@param px number\n---@param py number\n"))
    assert any("PicoDeckCharGrid.draw: parameter sx has no ---@param" in p for p in problems), problems


def test_annotation_without_parameter_is_caught():
    line = "---@param verts number[] Flat `{x1, y1, z1, x2, y2, z2, ...}`; at most 4096 vertices\n"
    assert STUB.count(line) == 1
    problems = cls.check(STUB.replace(line, line + "---@param nope integer\n"))
    assert any("picocalc.gfx3d.newMesh: ---@param nope names no parameter" in p for p in problems), problems


def test_function_without_description_is_caught():
    line = "---Reset the group mask to 0.\n"
    assert STUB.count(line) == 1
    problems = cls.check(STUB.replace(line, ""))
    assert any("PicoDeckSprite.resetGroupMask: no description line" in p for p in problems), problems
