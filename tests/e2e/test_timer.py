"""picocalc.sys.getTimeUs and perf.setTargetFPS deadline pacing."""
import pytest

from helpers import lua_case_names

CASES = lua_case_names("timer_test")


@pytest.fixture(scope="module")
def timer_run(lua_suite):
    return lua_suite("timer_test")


@pytest.mark.parametrize("case", CASES)
def test_timer(timer_run, case):
    timer_run.check_case(case)


def test_timer_suite_complete(timer_run):
    timer_run.assert_all_passed(CASES)
