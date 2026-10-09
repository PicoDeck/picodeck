"""The boot splash in the simulator (src/os/splash.c): its still frame
(picocalc.ui.splash) against a golden and against the settled frame the user
picked in the crt-splash prototype, and --show-splash playing the animation
through."""
import re
import shutil
from pathlib import Path

import numpy as np
from PIL import Image

from helpers import GOLDEN_DIR, app_rel_dir, compare_golden

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = GOLDEN_DIR / "splash" / "static.png"
PROTOTYPE = GOLDEN_DIR / "splash" / "prototype_settled.png"
# Staged hidden (apps/.test/splash_static): the default card has no app slot to spare.
FIXTURE = Path(__file__).parent / "fixtures" / "splash_static"


def still_frame(simulator):
    shutil.copytree(FIXTURE, Path(simulator.sd_card_path) / app_rel_dir("splash_static"))
    simulator.clear_log()
    simulator.launch_app("splash_static")
    simulator.wait_for_log(r"SPLASH:READY", timeout=30)
    simulator.wait_frames(3)
    img = np.array(simulator.screenshot_pil().convert("RGB"))
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
    return img


def test_splash_still_frame_golden(simulator, update_baselines):
    compare_golden(still_frame(simulator), GOLDEN, update_baselines)


def _palette():
    """The splash palette (src/os/splash_logo_cells.c) as RGB888."""
    src = (ROOT / "src" / "os" / "splash_logo_cells.c").read_text()
    body = re.search(r"splash_palette\[\w*\] = \{(.*?)\};", src, re.S).group(1)
    out = []
    for v in (int(h, 16) for h in re.findall(r"0x[0-9a-f]+", body)):
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out.append(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return np.array(out, np.int32)


def _entries(img, pal):
    """Each pixel's nearest palette entry."""
    out = np.empty(img.shape[:2], np.int32)
    for y in range(img.shape[0]):
        d = ((img[y, :, None, :].astype(np.int32) - pal[None, :, :]) ** 2).sum(-1)
        out[y] = d.argmin(-1)
    return out


def test_splash_still_frame_matches_prototype(simulator):
    """The picked look, ported: above the status text every pixel is within
    one ramp step of the prototype's settled frame."""
    h = (ROOT / "src" / "os" / "splash_logo_cells.h").read_text()
    row0 = int(re.search(r"SPLASH_LOGO_ROW0 (\d+)", h).group(1))
    rows = int(re.search(r"SPLASH_LOGO_ROWS (\d+)", h).group(1))
    y1 = 2 * (row0 + rows)  # the status text starts below this
    got = still_frame(simulator)[:y1]
    want = np.array(Image.open(PROTOTYPE).convert("RGB"))[:y1]
    pal = _palette()
    diff = np.abs(_entries(got, pal) - _entries(want, pal))
    assert diff.max() <= 1, f"{int((diff > 1).sum())} pixels differ by more than one ramp step"


def test_show_splash_plays_through(sim_factory, test_sd_card):
    """--show-splash (screencasts): the simulator's stdout has
    "[Core0] Showing boot splash screen...", then "[Core0] Splash sequence
    complete", both before "[Socket] TCP port" (so before the launcher, which
    starts after the socket). simulator/main.c prints the second line only
    once its animation loop, which presents a frame per step, has reached
    SPLASH_T_END and the status steps are done. The simulator then answers
    a ping."""
    sim = sim_factory(test_sd_card, extra_args=["--show-splash"], timeout=30.0)
    out = sim.get_output()["stdout"].splitlines()

    def at(text):
        hits = [i for i, line in enumerate(out) if text in line]
        assert hits, f"no {text!r} on the simulator's stdout:\n" + "\n".join(out[-40:])
        return hits[0]

    start, done, port = (at(t) for t in ("[Core0] Showing boot splash screen...",
                                         "[Core0] Splash sequence complete",
                                         "[Socket] TCP port"))
    assert start < done < port, (start, done, port)
    assert sim.ping()
