"""picocalc.gfx3d API contract (src/os/lua_bridge_gfx3d.c over gfx3d.c)."""
import pytest

from helpers import lua_case_names

CASES = lua_case_names("gfx3d_test")


@pytest.fixture(scope="module")
def gfx3d_run(lua_suite):
    return lua_suite("gfx3d_test")


@pytest.mark.parametrize("case", CASES)
def test_gfx3d_contract(gfx3d_run, case):
    gfx3d_run.check_case(case)


def test_gfx3d_suite_complete(gfx3d_run):
    gfx3d_run.assert_all_passed(CASES)
