"""picocalc.gfx3d API contract (src/os/lua_bridge_gfx3d.c over gfx3d.c)."""
import pytest

from helpers import lua_case_names, stage_lua_app

CASES = lua_case_names("gfx3d_test")


@pytest.fixture(scope="module")
def gfx3d_run(lua_suite):
    return lua_suite("gfx3d_test")


@pytest.mark.parametrize("case", CASES)
def test_gfx3d_contract(gfx3d_run, case):
    gfx3d_run.check_case(case)


def test_gfx3d_suite_complete(gfx3d_run):
    gfx3d_run.assert_all_passed(CASES)


# ── I-1: a collected context cannot be reached by a later finalizer ─────────
#
# Lua 5.4 runs __gc finalisers newest-marked first, and the gfx3d context is
# created lazily (on first g3.* call). HOLD is created before the context, so
# at lua_close it is finalised AFTER the context: without the fix its __gc
# ran gfx3d_draw/endScene/beginScene through the already-freed context's
# dangling s_g (NULL), a NULL-pointer write inside gfx3d.c (HardFault on
# device, SIGSEGV in the simulator). The fix in l_ctx_gc clears s_g/s_in_scene
# and latches s_closing only when the freed context IS the live one, and
# ctx() refuses to resurrect a new one while s_closing is set.
GC_CONTEXT_APP = """
local g3 = picocalc.gfx3d
local m = g3.newMesh({-1,-1,-5, 1,-1,-5, 0,1,-5}, {1,2,3}, 0xF800)
HOLD = setmetatable({}, {__gc = function()
    pcall(g3.draw, m, 0, 0, 0, 0, 0, 0)
    pcall(g3.endScene)
    pcall(g3.beginScene, 0)
end})
g3.beginScene(0)      -- creates the context after HOLD, so it is finalised first
picocalc.sys.log("G3GC:READY")
"""


@pytest.mark.sd(fixtures=[], reserve=2)  # stages g3gc_test and g3gc_after
def test_context_gc_cannot_be_reached_by_a_later_finalizer(simulator):
    stage_lua_app(simulator.sd_card_path, "g3gc_test", GC_CONTEXT_APP)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("g3gc_test")
    simulator.wait_for_log(r"^G3GC:READY$", timeout=10, since_seq=seq)
    outcome = simulator.wait_for_exit(timeout=10)
    assert outcome.get("result") == "returned", outcome

    # The simulator must still be usable afterwards: a second app launches
    # and exits cleanly (proves the process is alive and the bridge's static
    # state was left sane, not just that the crash happened to miss once).
    stage_lua_app(simulator.sd_card_path, "g3gc_after", 'picocalc.sys.log("AFTER:READY")\n')
    simulator.launch_app("g3gc_after")
    simulator.wait_for_log(r"^AFTER:READY$", timeout=10)
    outcome2 = simulator.wait_for_exit(timeout=10)
    assert outcome2.get("result") == "returned", outcome2
