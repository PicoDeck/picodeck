"""Fire-and-forget sound effects: sample:play() and sample:playAt() with the
result dropped must play to the end, whatever the collector does, and give
their voice back when they finish, referenced or not. Before the fix the ninth
call raised "failed to create player" in an app making little garbage, and the
collector cut the sound off in one making more.

sound_sfx runs each case alone in its own simulator, on the counting allocator
and on the device's own heap (--real-umm), and all of them in one run on the
device.
"""
import shutil
from pathlib import Path

import pytest

from helpers import app_rel_dir, case_params, lua_case_names, write_wav

APP = "sound_sfx"
# Staged hidden (apps/.test/sound_sfx): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / APP
CASES = lua_case_names(FIXTURE)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}

_WAVS = {"short.wav": 0.05, "long.wav": 0.25}


def _stage(app: Path):
    shutil.copytree(FIXTURE, app, dirs_exist_ok=True)
    for name, seconds in _WAVS.items():
        write_wav(app / name, seconds=seconds)


def _setup(case):
    def setup(sd):
        app = Path(sd) / app_rel_dir(APP)
        _stage(app)
        (app / "only.flag").write_text(case)
    return setup


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_sound_sfx(lua_suite, case):
    run = lua_suite(APP, setup=_setup(case), timeout=60, fixtures=[])
    run.check_case(case)
    run.assert_clean_exit()


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_sound_sfx_real_umm(lua_suite, case):
    run = lua_suite(APP, setup=_setup(case), timeout=60, fixtures=[],
                    extra_args=["--real-umm"])
    run.check_case(case)
    run.assert_clean_exit()


@pytest.mark.hardware
def test_sound_sfx_on_the_device(target, tmp_path):
    """The whole fixture in one run on a PicoCalc: there the mixer is Core 1's
    refill interrupt and the Lua service pass runs from the 1 ms timer."""
    app = tmp_path / APP
    _stage(app)
    target.push_app(app, APP)
    run = target.run_lua_app(APP, timeout=180)
    run.assert_all_passed(CASES)
