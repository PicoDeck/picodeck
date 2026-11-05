"""What the sound calls report: play() when there is nothing to play,
setFinishCallback(nil) as HTTP and TCP allow it, sound.getCurrentTime() in
milliseconds and sound.sample(seconds) really silent.
"""
import shutil
from pathlib import Path

import pytest

from helpers import app_rel_dir, case_params, lua_case_names, write_wav

APP = "sound_calls"
# Staged hidden (apps/.test/sound_calls): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / APP
CASES = lua_case_names(FIXTURE)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


@pytest.fixture(scope="module")
def calls_run(lua_suite):
    def setup(sd):
        app = Path(sd) / app_rel_dir(APP)
        shutil.copytree(FIXTURE, app)
        write_wav(app / "short.wav", seconds=0.05)
        write_wav(app / "long.wav", seconds=0.25)
    return lua_suite(APP, setup=setup, timeout=60, fixtures=[])


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_sound_calls(calls_run, case):
    calls_run.check_case(case)


def test_sound_calls_exit_cleanly(calls_run):
    calls_run.assert_clean_exit()


@pytest.mark.hardware
def test_sound_calls_on_the_device(target, tmp_path):
    """The audio clock runs on the output's refill interrupt there, and the
    results come from the same bridge."""
    app = tmp_path / APP
    shutil.copytree(FIXTURE, app)
    write_wav(app / "short.wav", seconds=0.05)
    write_wav(app / "long.wav", seconds=0.25)
    target.push_app(app, APP)
    run = target.run_lua_app(APP, timeout=120)
    run.assert_all_passed(CASES)
