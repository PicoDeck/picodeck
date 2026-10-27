"""MP3 decoding against the app's core on the device (issue #28).

Core 1's MP3 decoder slowed a gfx3d game's frames on Core 0 ~2.1x at
44.1 kHz stereo (~1.24x at 22.05 kHz mono), every section alike: the shared
XIP cache and QMI, not CPU time. This runs the mp3_bench fixture
(tests/e2e/hw_apps/mp3_bench: ~800 gfx3d triangles a frame, paced at 30 fps
or not) for each scenario below and prints, per scenario, the frame work
(avg/p95 us, late frames) next to the window's XIP counters (`xipstat`: hit
rate, misses and downstream stall cycles per second, contested accesses),
when the decoder decoded (granules decoded because the ring was low vs
ahead in Core 0's idle windows), each granule's decode, synthesis and ring
write time, and the MP3 underruns (`audiostat`). The "off" rows decode only
when the ring runs low (`xipstat mp3idle off`), the "on" rows also decode
ahead while the paced frame waits; "idle" rows run no game (Core 1's own
traffic); "prio" sets Core 0 high bus priority (`xipstat prio core0`).

What it asserts is loose: every run produced frames, decoding ahead was used
where it can be, no run underran, and a paced game's frame work with music
decoded ahead stays within 15% of the same game with none, with no late
frame (with the decoder's hot state in SRAM and a granule at a time it was
the same: 18.7 ms at 44.1 kHz stereo and with no music, 200 MHz, Nova
Rail's music). The numbers are the point: run with -s.

    pytest tests/e2e/test_mp3_contention_hw.py -s \\
        --target hw:/dev/serial/by-id/<PicoDeck device>

MP3_BENCH_MUSIC=<file> uses real music (transcoded and cut to 8 s) instead
of the generated chord: Nova Rail's track01.mp3, for one. Needs ffmpeg. Send
nothing else to the device while it runs (a dev command stalls the app).
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from hw_target import HW_APPS_DIR

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(2400)]

FFMPEG = shutil.which("ffmpeg")
APP, APP_ID = "mp3_bench", "com.test.mp3_bench"
SECONDS = 20          # each run; the app leaves its first 2 s out
MUSIC_SECONDS = 8     # looped; short, because pushing to the SD is slow
CHORD = ("(0.6+0.4*sin(2*PI*0.5*t))*(0.22*sin(2*PI*220*t)"
         "+0.16*sin(2*PI*275*t)+0.12*sin(2*PI*330*t))")
FORMATS = {           # file: (rate, channels, bitrate)
    "s44.mp3": (44100, 2, "128k"),
    "m22.mp3": (22050, 1, "64k"),
}

# (name, music, fps, mode, decode ahead, Core 0 bus priority)
SCENARIOS = [
    ("game_none",        "",        30, "game", True,  False),
    ("game_s44_off",     "s44.mp3", 30, "game", False, False),
    ("game_s44_on",      "s44.mp3", 30, "game", True,  False),
    ("game_m22_off",     "m22.mp3", 30, "game", False, False),
    ("game_m22_on",      "m22.mp3", 30, "game", True,  False),
    ("idle_none",        "",        30, "idle", True,  False),
    ("idle_s44",         "s44.mp3", 30, "idle", True,  False),
    ("unpaced_none",     "",         0, "game", True,  False),
    ("unpaced_s44",      "s44.mp3",  0, "game", True,  False),
    ("game_s44_off_prio", "s44.mp3", 30, "game", False, True),
]


def make_music(dest: Path, name: str):
    rate, channels, kbps = FORMATS[name]
    src = os.environ.get("MP3_BENCH_MUSIC")
    inp = (["-i", src] if src else
           ["-f", "lavfi", "-i", f"aevalsrc={CHORD}|{CHORD}:s={rate}"])
    subprocess.run([FFMPEG, "-v", "error", "-y", *inp, "-t", str(MUSIC_SECONDS),
                    "-ar", str(rate), "-ac", str(channels), "-c:a", "libmp3lame",
                    "-b:a", kbps, "-write_xing", "0", "-id3v2_version", "0",
                    str(dest / name)], check=True)


def reply(target, cmd: str, tag: str) -> dict:
    """One dev-command reply line `[DEV] <tag>: k=v ...` as ints."""
    for line in target.command(cmd, timeout=3.0):
        if f"[DEV] {tag}:" in line:
            return {k: int(v) for k, v in
                    re.findall(r"(\w+)=(-?\d+)", line.split(f"{tag}:", 1)[1])}
    raise AssertionError(f"no {tag} reply to {cmd!r}")


def run(target, name, music, fps, mode, ahead, prio) -> dict:
    target.write_file(f"/data/{APP_ID}/bench.json", json.dumps(
        {"music": music, "seconds": SECONDS, "fps": fps, "mode": mode}).encode())
    target.delete_file(f"/data/{APP_ID}/bench_result.json")
    reply(target, "xipstat mp3idle " + ("on" if ahead else "off"), "XIP")
    reply(target, "xipstat prio " + ("core0" if prio else "none"), "XIP")
    reply(target, "audiostat reset", "Audio")
    target.launch_app(APP)
    # One command while the app warms up (left out of its statistics): the
    # window then ends by itself when the app exits.
    reply(target, "xipstat reset", "XIP")
    time.sleep(SECONDS + 2)           # nothing sent until it has finished
    out = target.wait_for_exit(timeout=60)
    assert out["result"] == "returned", out
    xip = reply(target, "xipstat", "XIP")
    audio = reply(target, "audiostat", "Audio")
    res = json.loads(target.read_file(f"/data/{APP_ID}/bench_result.json"))
    reply(target, "xipstat prio none", "XIP")
    reply(target, "xipstat mp3idle on", "XIP")
    return {"name": name, "xip": xip, "audio": audio, "app": res}


def row(r) -> str:
    x, a, app = r["xip"], r["audio"], r["app"]
    s = max(x["window_ms"], 1) / 1000
    hz = x["sys_khz"] * 1000
    stall = (x["stall0"] + x["stall1"]) / (s * hz) * 100
    return (f"{r['name']:18s} work avg {app.get('work_avg', 0):6d} p95 "
            f"{app.get('work_p95', 0):6d} late {app.get('late', 0):4d}/"
            f"{app.get('frames', 0):4d} | hit {x['hit_pm'] / 10:5.1f}% "
            f"miss/s {x['miss'] / s / 1e6:6.2f}M stall {stall:5.1f}% "
            f"contested/s {(x['contested0'] + x['contested1']) / s / 1e3:7.1f}k "
            f"| mp3 low {x['mp3_low_frames']:4d} ahead {x['mp3_idle_frames']:4d} "
            f"overran {x['mp3_overran']:3d} frame_us {x['mp3_frame_us']:5d} "
            f"idle_ms {x['idle_ms']:5d} underruns {a['mp3_underruns']}"
            + unit_cost(x))


def unit_cost(x) -> str:
    """Per granule (the decoder's unit): decode, synthesis, ring write (us)."""
    n = x.get("mp3_low_frames", 0) + x.get("mp3_idle_frames", 0)
    if not n or "mp3_dec_us" not in x:
        return ""
    return (f" | per granule dec {x['mp3_dec_us'] // n} syn "
            f"{x['mp3_syn_us'] // n} out {x['mp3_out_us'] // n} us")


@pytest.mark.skipif(not FFMPEG, reason="needs ffmpeg for the music")
def test_mp3_contention(target):
    with tempfile.TemporaryDirectory() as tmp:
        app = Path(tmp) / APP
        shutil.copytree(HW_APPS_DIR / APP, app)
        make_music(app, "s44.mp3")
        target.push_app(app, APP)
    with tempfile.TemporaryDirectory() as tmp:
        app = Path(tmp) / APP       # unzip merges into /apps/.test/mp3_bench
        app.mkdir()
        shutil.copy(HW_APPS_DIR / APP / "app.json", app / "app.json")
        make_music(app, "m22.mp3")
        target.push_app(app, APP)

    results = {}
    for sc in SCENARIOS:
        r = run(target, *sc)
        results[sc[0]] = r
        print(row(r), flush=True)

    for name, r in results.items():
        if r["app"]["mode"] == "game":
            assert r["app"].get("frames", 0) > 0, r
        if r["app"]["music"]:
            assert r["app"].get("mp3_playing"), r
    for fmt in ("s44", "m22"):
        on, off = results[f"game_{fmt}_on"], results[f"game_{fmt}_off"]
        assert on["xip"]["mp3_idle_frames"] > 0, on
        assert off["xip"]["mp3_idle_frames"] == 0, off
    assert results["unpaced_s44"]["xip"]["mp3_idle_frames"] == 0
    for name, r in results.items():
        assert r["audio"]["mp3_underruns"] == 0, (name, r)
    none = results["game_none"]["app"]["work_avg"]
    for name in ("game_s44_on", "game_m22_on"):
        app = results[name]["app"]
        assert app["late"] == 0, (name, app)
        assert app["work_avg"] <= none * 1.15, (name, app, none)
