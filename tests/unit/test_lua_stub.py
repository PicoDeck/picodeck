"""sdk/lua/picocalc.lua must match the bridge. Run: python3 -m pytest tests/unit/test_lua_stub.py -v

tools/check_lua_stub.py compares every stub signature with the C function
behind the same luaL_Reg entry. The mutation tests put back the six wrong
signatures issue #37 reported and prove the checker still sees each one.
"""
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_lua_stub as cls  # noqa: E402

STUB = cls.STUB.read_text()


def test_stub_matches_the_bridge():
    assert cls.check(STUB) == []


def test_every_owner_table_exists_in_the_bridge():
    # A renamed C table would silently drop its functions from the check.
    found = {(p.name, t) for p in cls.BRIDGE_DIR.glob("lua_bridge*.c")
             for t in cls.reg_tables(cls.strip_comments(p.read_text()))}
    assert set(cls.OWNERS) <= found


def test_allowlist_names_real_stub_functions():
    assert set(cls.ALLOW) <= set(cls.parse_stub(STUB))


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
    # l_sample_playAt: `return luaL_error(...)` on failure, `return 1` on success.
    fixed = "---@return PicoDeckSamplePlayer player The player that is playing the sample (raises if none is free)\n"
    assert STUB.count(fixed) == 1
    assert any("playAt" in p for p in cls.check(STUB.replace(fixed, "")))


def test_one_line_return_list_counts_each_value():
    assert cls._return_count("---@return integer w, integer h") == 2
    assert cls._return_count("---@return string? error `\"a\"`, `\"b\"` or `\"c\"`") == 1
    assert cls._return_count("---@return {a: integer, b: integer} t") == 1


def test_c_scanning_ignores_braces_and_comment_markers_in_literals():
    src = 'static int f(lua_State *L) {\n  puts("}} // x");\n  return 1;\n}\nint g(void) { return 2; }\n'
    fns = cls.c_functions(cls.strip_comments(src))
    assert "return 1;" in fns["f"] and "return 2" not in fns["f"] and "g" in fns
