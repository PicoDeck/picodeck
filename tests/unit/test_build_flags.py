"""The Lua VM runs hook-free (src/os/lua_bridge.c's Service hook), which
matters only because the VM itself is fast: -O2, not -Os (P0,
specs/2026-09-27-p0-lua-perf-findings.md). tests/e2e/test_lua_hook_hw.py's
EMPTY_LOOP_NS_MAX assumes this; pin it here so a regression fails fast in the
unit suite instead of only showing up as a wider hardware timing budget.
Run: python3 -m pytest tests/unit/test_build_flags.py -v"""
from pathlib import Path

CMAKE_LISTS = Path(__file__).resolve().parents[2] / "CMakeLists.txt"


def test_lua_builds_at_o2():
    text = CMAKE_LISTS.read_text()
    assert "target_compile_options(lua PRIVATE -O2)" in text
