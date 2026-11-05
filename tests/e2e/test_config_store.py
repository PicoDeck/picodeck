"""Config store limits and per-app isolation (audit §3.1 appconfig, §3.9).

config_limits_test runs twice in one simulator: the first launch writes, the
second reads back from disk. cfg_a then cfg_b run back to back in one
simulator — the one-sim-per-test habit never ran two apps in a row, which is
how appconfig's stale app ID went unnoticed.
"""

import json
from pathlib import Path

import pytest

from helpers import (case_params, lua_case_names, require_heap_metrics_live,
                     run_lua_app, stage_lua_app)

LIMIT_CASES = lua_case_names("config_limits_test")

CAP_BUG = ("review: Core Medium — sysconfig has a fixed capacity and drops extra "
           "keys silently (a 1 KB pool); picocalc.config says so from set()")
TRUNC_BUG = ("audit §3.9 — sysconfig truncates values at 127 chars silently")

# sysconfig_eleven_keys passes: short keys no longer run out of slots.
LIMIT_KNOWN_BUGS = {
    "sysconfig_fill_pool": CAP_BUG,
    "sysconfig_200_char_value": TRUNC_BUG,
}


@pytest.fixture(scope="module")
def limits_run(sim_module):
    sim = sim_module
    first = run_lua_app(sim, "config_limits_test", timeout=20)
    second = run_lua_app(sim, "config_limits_test", timeout=20)
    for run in (first, second):
        run.assert_clean_exit()
    merged = dict(first.cases)
    merged.update(second.cases)
    first.cases = merged
    return first


@pytest.mark.parametrize("case", case_params(LIMIT_CASES, LIMIT_KNOWN_BUGS))
def test_config_limits(limits_run, case):
    limits_run.check_case(case)


def test_config_limits_saved_file_is_json(limits_run):
    """The per-app store on disk is valid JSON holding the persisted keys."""
    path = limits_run.sd / "data" / "com.test.config_limits" / "config.json"
    saved = json.loads(path.read_text())
    assert {f"p{i}": f"persist{i}" for i in range(1, 5)}.items() <= saved.items(), saved


# ── cross-app bleed ─────────────────────────────────────────────────────────


@pytest.fixture
def bleed(simulator):
    a = run_lua_app(simulator, "cfg_a", timeout=15)
    a.assert_all_passed(["a_sets_and_saves"])
    b = run_lua_app(simulator, "cfg_b", timeout=15)
    b.assert_clean_exit()
    return simulator, b


def test_second_app_does_not_see_first_apps_config(bleed):
    _, b = bleed
    b.check_case("b_does_not_see_a")


def test_second_app_saves_to_its_own_file(bleed):
    sim, b = bleed
    b.check_case("b_sets_and_saves")
    data = Path(sim.sd_card_path) / "data"
    a_cfg = json.loads((data / "com.test.cfg_a" / "config.json").read_text())
    assert a_cfg == {"k": "A"}, f"cfg_a's config was rewritten: {a_cfg}"
    b_file = data / "com.test.cfg_b" / "config.json"
    assert b_file.exists(), "cfg_b's save did not land in /data/com.test.cfg_b"
    assert json.loads(b_file.read_text()) == {"b": "B"}


# ── memory ──────────────────────────────────────────────────────────────────


def test_store_is_only_there_while_it_holds_keys(simulator):
    """It costs an app that uses no config nothing, about 9 KB once it holds a
    key, and goes back to the heap when the app exits."""
    sim = simulator
    require_heap_metrics_live(sim)
    code = """
    local pc = picocalc
    local function free() return pc.sys.getMemInfo().psram_free end
    collectgarbage(); collectgarbage()
    local before = free()
    pc.config.get("nothing_here")
    collectgarbage(); collectgarbage()
    local after_get = free()
    pc.config.set("k", "v")
    collectgarbage(); collectgarbage()
    local after_set = free()
    pc.sys.log("COST " .. (before - after_get) .. " " .. (after_get - after_set))
    """
    stage_lua_app(sim.sd_card_path, "cfg_cost", code)
    def launcher_free():
        return sim.call("get_heap_info")["lua_heap_free_kb"]
    baseline = launcher_free()
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("cfg_cost")
    sim.wait_for_exit(timeout=10)
    line = sim.wait_for_log(r"^COST ", timeout=5, since_seq=seq)
    text = line if isinstance(line, str) else line.get("text", "")
    get_cost, set_cost = (int(v) for v in text.split()[1:3])
    assert get_cost < 1024, f"reading an empty store allocated {get_cost} B"
    assert 9 * 1024 <= set_cost < 12 * 1024, f"the store cost {set_cost} B"
    assert launcher_free() == baseline, "the store outlived the app"
