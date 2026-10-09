"""picocalc.chargrid API contract (src/os/lua_bridge_chargrid.c over chargrid*.c)."""
import shutil
from pathlib import Path

import pytest

from helpers import app_rel_dir, lua_case_names

# Staged hidden (apps/.test/chargrid_test): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / "chargrid_test"
CASES = lua_case_names(FIXTURE)


@pytest.fixture(scope="module")
def chargrid_run(lua_suite):
    return lua_suite(
        "chargrid_test",
        setup=lambda sd: shutil.copytree(FIXTURE, Path(sd) / app_rel_dir("chargrid_test")),
        fixtures=[])


@pytest.mark.parametrize("case", CASES)
def test_chargrid_contract(chargrid_run, case):
    chargrid_run.check_case(case)


def test_chargrid_suite_complete(chargrid_run):
    chargrid_run.assert_all_passed(CASES)
