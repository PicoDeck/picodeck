"""The sandboxed, text-only `require` (src/os/lua_bridge_require.c)."""
import pytest

from helpers import lua_case_names

CASES = lua_case_names("require_test")


@pytest.fixture(scope="module")
def require_run(lua_suite):
    return lua_suite("require_test")


@pytest.mark.parametrize("case", CASES)
def test_require(require_run, case):
    require_run.check_case(case)


def test_require_suite_complete(require_run):
    require_run.assert_all_passed(CASES)
