"""Coroutine library parity: PicoDeck ships a fork of lcorolib.c
(src/os/lua_corolib.c) that reports thread switches to the service hook.
Its Lua-visible behaviour must match upstream exactly."""
import pytest

from helpers import lua_case_names

CORO_CASES = lua_case_names("coro_test")


@pytest.fixture(scope="module")
def coro_run(lua_suite):
    return lua_suite("coro_test")


@pytest.mark.parametrize("case", CORO_CASES)
def test_coroutine_library(coro_run, case):
    coro_run.check_case(case)


def test_coroutine_suite_complete(coro_run):
    coro_run.assert_all_passed(CORO_CASES)
