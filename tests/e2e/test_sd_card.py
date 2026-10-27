"""The SD card manifest and the launcher's app cap (MAX_APPS).

The launcher keeps at most MAX_APPS apps in directory order and drops the rest
with a warning, and which apps survive depends on the filesystem. These tests
keep the harness from ever building a card that trips it, and check that a
card that does is reported loudly.
"""
import time

import pytest

import helpers
from helpers import (build_sd_card, fixture_app_names, launcher_app_cap,
                     new_simulator, stage_lua_app)


def _apps(sd):
    return sorted(p.name for p in (sd / "apps").iterdir() if p.is_dir())


def test_default_card_holds_every_fixture_and_fits_the_cap(tmp_path):
    """A fixture added past the cap fails here, on every filesystem, instead
    of silently dropping apps from every test's card. One slot must stay
    free: many tests stage a single app without a marker."""
    sd = build_sd_card(tmp_path / "sd", reserve=1)
    assert set(fixture_app_names()) <= set(_apps(sd))


def test_fixtures_option_limits_the_card_to_the_named_apps(tmp_path):
    sd = build_sd_card(tmp_path / "sd", fixtures=["fs_test"])
    assert set(_apps(sd)) == {"fs_test", "hello"}
    assert _apps(build_sd_card(tmp_path / "sd2", fixtures=[])) == ["hello"]


def test_unknown_fixture_name_is_an_error(tmp_path):
    with pytest.raises(ValueError, match="no_such_fixture"):
        build_sd_card(tmp_path / "sd", fixtures=["no_such_fixture"])


def test_card_over_the_cap_is_refused_with_the_remedy(tmp_path, monkeypatch):
    fake = tmp_path / "fixtures"
    for i in range(launcher_app_cap()):
        (fake / f"dummy_{i}").mkdir(parents=True)
    monkeypatch.setattr(helpers, "FIXTURE_APPS", fake)
    with pytest.raises(RuntimeError, match=r"MAX_APPS.*fixtures=") as e:
        build_sd_card(tmp_path / "sd")  # hello + the dummies
    assert "src/os/launcher.c" in str(e.value)


def test_reserve_counts_the_apps_a_test_will_stage(tmp_path):
    room = launcher_app_cap() - len(fixture_app_names()) - 1  # hello
    build_sd_card(tmp_path / "ok", reserve=max(room, 0))
    with pytest.raises(RuntimeError, match="stages"):
        build_sd_card(tmp_path / "over", reserve=max(room, 0) + 1)
    # A test that stages many apps asks for a card without the fixtures.
    build_sd_card(tmp_path / "many", fixtures=[], reserve=launcher_app_cap() - 1)


@pytest.mark.sd(fixtures=[])
def test_a_simulator_that_drops_apps_at_the_cap_is_unhealthy(
        request, simulator_binary, test_sd_card, tmp_path):
    """The backstop for a test that stages more than it declared: the
    launcher's drop warning turns into a health problem naming the fix."""
    for i in range(launcher_app_cap() + 1):
        stage_lua_app(test_sd_card, f"cap_{i:03d}", "return\n", hidden=False)
    sim = new_simulator(request.config, simulator_binary, test_sd_card,
                        tmp_path / "crash.log")
    try:
        deadline = time.time() + 10
        problems = sim.health_problems()
        while not problems and time.time() < deadline:  # the boot scan logs it
            time.sleep(0.05)
            problems = sim.health_problems()
    finally:
        sim.stop()
    # The launcher's own message carries its compiled-in cap, which must
    # equal the value the harness parsed from the source.
    want = f"app cap ({launcher_app_cap()})"
    assert any("MAX_APPS" in p and want in p for p in problems), problems


def test_hidden_roots_do_not_count_against_the_cap(tmp_path):
    """/apps/.test and /apps/.dev are dot names the launcher skips: a card
    holding only hidden apps beyond the cap still builds."""
    sd = build_sd_card(tmp_path / "hid", fixtures=[],
                       reserve=launcher_app_cap() - 1)
    for i in range(launcher_app_cap() + 5):
        stage_lua_app(sd, f"t_{i:03d}", "return\n")
    (sd / "apps" / ".dev" / "mine").mkdir(parents=True)
    build_sd_card(sd, fixtures=[], reserve=launcher_app_cap() - 1)
