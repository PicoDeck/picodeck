"""Image pixel access from Lua (#39): img:getPixel/setPixel/getPixels/
setPixels and display.getPixel, and generated images used as textures by
drawPlane and drawTexturedColumn.

The contract cases run in tests/e2e/apps/pixels_test (picotest). The screen
test runs the same app in draw mode and probes the presented frame with
get_pixel, so the generated pixels are checked all the way to the screen.

The simulator's framebuffer is host order, so display_get_pixel's unswap and
the firmware blitters' swap of generated images run only on the device:
test_pixels_contract_on_device runs the same cases there (--target hw).
"""
from pathlib import Path

import pytest

from helpers import FIXTURE_APPS, lua_case_names

APP = "pixels_test"
CASES = lua_case_names(APP)


@pytest.fixture(scope="module")
def pixels_run(lua_suite):
    return lua_suite(APP)


@pytest.mark.parametrize("case", CASES)
def test_pixels_contract(pixels_run, case):
    pixels_run.check_case(case)


def test_pixels_suite_complete(pixels_run):
    pixels_run.assert_all_passed(CASES)


def _px(sim, x, y):
    return sim.call("get_pixel", {"x": x, "y": y})["rgb565"]


def test_generated_images_on_screen(simulator):
    (Path(simulator.sd_card_path) / "apps" / APP / "draw.flag").write_text("1")
    simulator.clear_log()
    simulator.launch_app(APP)
    simulator.wait_for_log("PX:READY", timeout=30)
    simulator.wait_frames(2)
    # 2x2 built with setPixel, drawn at 10x
    assert _px(simulator, 5, 5) == 0x1234
    assert _px(simulator, 15, 5) == 0xF800
    assert _px(simulator, 5, 15) == 0x07E0
    assert _px(simulator, 15, 15) == 0x001F
    # 4x1 built with setPixels (little-endian bytes), drawn at 5x from x = 40
    assert [_px(simulator, x, 2) for x in (42, 47, 52, 57)] == \
        [0xABCD, 0x5678, 0x0F0F, 0xF0F0]
    # textured column from a generated 1x2 texture
    assert _px(simulator, 100, 2) == 0x2345
    assert _px(simulator, 100, 17) == 0x6789
    # mode-7 plane from a generated solid texture, below the horizon only
    assert _px(simulator, 160, 300) == 0x4A69
    assert _px(simulator, 0, 201) == 0x4A69
    assert _px(simulator, 160, 150) == 0x0000
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)


@pytest.mark.hardware
@pytest.mark.timeout(300)
def test_pixels_contract_on_device(target):
    """The same picotest cases on a PicoCalc, where the framebuffer is
    byte-swapped: display.getPixel must undo the swap and a drawn generated
    image must come back as the colour it was built with."""
    target.push_app(FIXTURE_APPS / APP, APP)
    # Never pushed; a stale one from a manual run would switch to draw mode.
    target.delete_file(f"/apps/.test/{APP}/draw.flag")
    run = target.run_lua_app(APP, timeout=60)
    run.assert_all_passed(CASES)
