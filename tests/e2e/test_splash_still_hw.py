"""The boot splash's still frame on the device (src/os/splash.c
splash_draw_static, reached through picocalc.ui.splash): the screenshot
must equal the simulator's golden, tests/e2e/fixtures/splash/static.png
(test_splash.py), pixel for pixel.

No tolerance: splash_render() is bit-identical on the host and the M33
(ctest test_splash_m33), the device draws it byte-swapped for the panel,
which the screenshot decodes back (big-endian, test_hw_device.py's colour
blocks), and both screenshot paths expand RGB565 to RGB888 the same way
(x * 255 / 31 or 63)."""
import io
import tempfile
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from helpers import GOLDEN_DIR, compare_golden

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(120)]

FIXTURE = Path(__file__).parent / "fixtures" / "splash_static"
GOLDEN = GOLDEN_DIR / "splash" / "static.png"


def test_still_frame_matches_simulator_golden(target):
    target.push_app(FIXTURE, "splash_static")
    mark = target.log_cursor()
    target.launch_app("splash_static")
    try:
        try:
            target.wait_for_log(r"SPLASH:READY", timeout=10.0, since=mark)
        except TimeoutError:
            # The serial capture can drop [APP] lines. The fixture draws the
            # frame before it logs, then redraws it every 20 ms: running for
            # 10 s, it has drawn it.
            assert target.status()["app"] != "launcher", "splash_static is not running"
        png = target.screenshot()
    finally:
        target.exit_app()
        target.wait_for_exit(timeout=15)
    got = np.array(Image.open(io.BytesIO(png)).convert("RGB"))
    try:
        compare_golden(got, GOLDEN)
    except AssertionError:
        saved = Path(tempfile.gettempdir(), "picodeck_hw_splash_still.png")
        saved.write_bytes(png)
        print(f"device screenshot saved to {saved}")
        raise
    print("splash still frame: equal to the simulator golden (0 pixels differ)")
