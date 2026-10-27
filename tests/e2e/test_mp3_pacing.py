"""MP3 decoding ahead while a paced app waits for its frame (issue #28).

MP3 decoding on Core 1 contends with Core 0 for the XIP cache and the QMI,
so the decoder now also decodes while a paced app (perf.setTargetFPS) waits
at the end of its frame, and otherwise only when the PCM ring runs low, as
before (src/drivers/mp3_sched.h). The simulator runs the firmware's
mp3_player.c and perf.c, with Core 1 on its own thread, so this checks the
schedule end to end: music plays with no underrun, paced (most frames are
decoded in Core 0's idle windows) and unpaced (none are), at 44.1 kHz stereo
and 22.05 kHz mono. The timing itself (what it saves the app) is hardware
only: tests/e2e/test_mp3_contention_hw.py.

The app is the hardware test's (tests/e2e/hw_apps/mp3_bench), staged at
runtime with silent MP3s; `get_mp3_stats` is the simulator's RPC.
"""
import json
import shutil
from pathlib import Path

import pytest

from hw_target import HW_APPS_DIR

APP, APP_ID = "mp3_bench", "com.test.mp3_bench"
SECONDS = 6

# One silent frame of each format (all-zero side info decodes to silence):
# MPEG-1 Layer III 128 kbps 44.1 kHz joint stereo (417 bytes, 1152 samples)
# and MPEG-2 Layer III 64 kbps 22.05 kHz mono (208 bytes, 576 samples).
FRAMES = {
    "stereo44": (b"\xff\xfb\x90\x64", 417, 44100, 77),
    "mono22": (b"\xff\xf3\x80\xc0", 208, 22050, 77),
}


def stage(sd, fmt, fps):
    app = sd / "apps" / APP
    shutil.copytree(HW_APPS_DIR / APP, app, dirs_exist_ok=True)
    header, size, _, count = FRAMES[fmt]
    (app / "music.mp3").write_bytes((header + bytes(size - 4)) * count)  # ~2 s
    data = sd / "data" / APP_ID
    data.mkdir(parents=True, exist_ok=True)
    (data / "bench.json").write_text(json.dumps(
        {"music": "music.mp3", "seconds": SECONDS, "fps": fps, "warm": 1}))
    result = data / "bench_result.json"
    if result.exists():
        result.unlink()
    return result


@pytest.mark.sd(fixtures=[], reserve=1)
@pytest.mark.parametrize("fmt", ["stereo44", "mono22"])
@pytest.mark.parametrize("paced", [True, False], ids=["paced", "unpaced"])
def test_mp3_plays_without_underruns(simulator, fmt, paced):
    result_file = stage(Path(simulator.sd_card_path), fmt, 30 if paced else 0)
    before = simulator.call("get_mp3_stats")
    simulator.launch_app(APP)
    outcome = simulator.wait_for_exit(timeout=SECONDS + 60)
    assert outcome.get("result") == "returned", outcome
    after = simulator.call("get_mp3_stats")
    result = json.loads(result_file.read_text())
    delta = {k: after[k] - before[k] for k in
             ("underruns", "low_frames", "idle_frames", "idle_windows")}
    print(f"{fmt} {'paced' if paced else 'unpaced'}: {delta} {result}")

    rate = FRAMES[fmt][2]
    assert result["mp3_playing"], result
    assert result["mp3_rate"] == rate
    # It kept up with the music the whole time (a loose bound: the
    # position counts only what the mixer played).
    assert result["mp3_position"] >= rate * (SECONDS - 2), result
    assert delta["underruns"] == 0, delta
    if paced:
        assert delta["idle_windows"] > 0, delta
        assert delta["idle_frames"] > delta["low_frames"], delta
    else:
        assert delta["idle_windows"] == 0, delta
        assert delta["idle_frames"] == 0, delta
        assert delta["low_frames"] > 0, delta
