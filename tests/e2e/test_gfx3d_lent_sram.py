"""A gfx3d scene's records sit in SRAM lent by the MP3 decoder (src/os/gfx3d.h,
gfx3d_sram_tier). An MP3 session started between two draw calls takes the block
back: the fixture checks the scene survives, pixel for pixel. On the device Core 1
is decoding into the block by the time the scene is rasterised, which the simulator
cannot show (`tests/unit/test_gfx3d.c` runs every block size under ASan).
"""
import shutil
from pathlib import Path

import pytest

from helpers import app_rel_dir, case_params, lua_case_names, write_mp3

APP = "gfx3d_lent_sram"
# Staged hidden (apps/.test/...): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / APP
CASES = lua_case_names(FIXTURE)
FRAMES = 400  # 417 B each: ~10 s, longer than the fixture, and it loops anyway

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


@pytest.fixture(scope="module")
def lent_run(lua_suite):
    def setup(sd):
        app = Path(sd) / app_rel_dir(APP)
        shutil.copytree(FIXTURE, app)
        write_mp3(app / "music.mp3", frames=FRAMES)
    return lua_suite(APP, setup=setup, timeout=60, fixtures=[])


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_lent_sram(lent_run, case):
    lent_run.check_case(case)


def test_lent_sram_exits_cleanly(lent_run):
    lent_run.assert_clean_exit()


@pytest.mark.hardware
def test_lent_sram_on_the_device(target, tmp_path):
    app = tmp_path / APP
    shutil.copytree(FIXTURE, app)
    write_mp3(app / "music.mp3", frames=FRAMES)
    target.push_app(app, APP)
    run = target.run_lua_app(APP, timeout=120)
    run.assert_all_passed(CASES)
