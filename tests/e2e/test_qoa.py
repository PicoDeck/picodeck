"""QOA streaming through the fileplayer (src/drivers/qoa.c +
third_party/qoa into src/drivers/fileplayer.c).

Mirrors test_fileplayer.py: the simulator runs the firmware fileplayer and
drains the stream ring at the real 44.1 kHz output rate, so wall-clock play
time is a faithful check of the QOA producer's flow control.  Each case runs
alone in its own simulator.
"""

import pytest

from helpers import case_params, lua_case_names, write_qoa

APP = "qoa_test"
CASES = lua_case_names(APP)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


def _setup(case):
    def setup(sd):
        app = sd / "apps" / APP
        write_qoa(app / "three_s.qoa", seconds=3.0, rate=22050)
        write_qoa(app / "one_s.qoa", seconds=1.0, rate=22050)
        write_qoa(app / "stereo_s.qoa", seconds=1.0, rate=44100, channels=2)
        (app / "only.flag").write_text(case)
    return setup


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_qoa(lua_suite, case):
    run = lua_suite(APP, setup=_setup(case), timeout=60)
    run.check_case(case)
    run.assert_clean_exit()
