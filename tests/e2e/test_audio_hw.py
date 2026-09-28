"""The audio mixer on the device: Core 1's tick cost and the output's
refill interrupt with MP3 music alone, with eight sample voices, and with
both at once (pause and resume included); the output's rate across the
video player's 300 MHz boost; the output stopping when the app exits.
The simulator renders the same mix but not its timing: these run on
hardware only.

`audiostat [reset]` (src/dev_commands.c) is the measurement. Each scenario
measures one window while the fixture plays, and sends nothing else to
the device meanwhile. AUDIO_LISTEN=1 runs test_listen, which needs a
person listening at the device. ffmpeg (on PATH) makes the music."""
import math
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
import wave
from pathlib import Path

import pytest

pytestmark = [pytest.mark.hardware, pytest.mark.timeout(900)]

FFMPEG = shutil.which("ffmpeg")
LISTEN = os.environ.get("AUDIO_LISTEN") == "1"
APP, APP_ID = "audio_mix", "com.test.audio_mix"
SFX_HZ = (523, 587, 659, 698, 784, 880, 988, 1047)

# Core 1's tick cost before the mixer took the MP3 over: the worst of
# three runs of each scenario on the firmware before this change. "both"
# could not run then; its budget is the two scenarios' sum.
BASELINE = {
    "mp3": {"tick_over": 268, "tick_missed": 9394, "tick_max_us": 43995},
    "sfx": {"tick_over": 2, "tick_missed": 6, "tick_max_us": 2963},
}
BASELINE["both"] = {k: BASELINE["mp3"][k] + BASELINE["sfx"][k]
                    for k in BASELINE["mp3"]}


def assert_tick_cost_within(stats, base):
    """The acceptance criterion "Core 1 tick overruns unchanged", with room
    for run-to-run noise."""
    assert stats["tick_missed"] <= base["tick_missed"] * 3 // 2 + 2, (stats, base)
    assert stats["tick_over"] <= base["tick_over"] * 3 // 2 + 20, (stats, base)
    assert stats["tick_max_us"] <= base["tick_max_us"] * 3 // 2 + 500, (stats, base)


def audiostat(target, arg=""):
    """One `audiostat` reply as a dict of ints."""
    lines = target.command(("audiostat " + arg).strip(), timeout=3.0)
    for line in lines:
        if "[DEV] Audio:" in line:
            return {k: int(v) for k, v in
                    re.findall(r"(\w+)=(-?\d+)", line.split("Audio:", 1)[1])}
    raise AssertionError(f"no Audio reply to 'audiostat {arg}': {lines}")


def assert_passed(results):
    bad = [c for c in results.get("cases", []) if c["status"] != "PASS"]
    assert results.get("done") and not bad, results


def music_mp3(path: Path, seconds: int = 20):
    """A 4:5:6 chord (220, 275, 330 Hz) under a slow swell: smooth, so a
    crackle stands out, with whole cycles of every partial in `seconds`."""
    tone = ("(0.6+0.4*sin(2*PI*0.5*t))*(0.22*sin(2*PI*220*t)"
            "+0.16*sin(2*PI*275*t)+0.12*sin(2*PI*330*t))")
    subprocess.run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
                    f"aevalsrc={tone}|{tone}:s=44100:d={seconds}",
                    "-c:a", "libmp3lame", "-b:a", "96k", str(path)],
                   check=True)


def sfx_wav(path: Path, hz: int, seconds: float = 0.5, rate: int = 22050):
    """A decaying sine blip, mono 16-bit. Each voice is retriggered every
    400 ms (eight voices, a blip every 50 ms): 0.5 s keeps all eight busy."""
    n = int(seconds * rate)
    pcm = b"".join(
        struct.pack("<h", int(12000 * math.exp(-6 * i / n)
                              * math.sin(2 * math.pi * hz * i / rate)))
        for i in range(n))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


# run.txt holds "<mode> <seconds>": mp3 (music alone), sfx (eight voices),
# both (music + voices, a pause at 40% and a resume at 50% of the run),
# listen (the captioned sequence of test_listen).
FIXTURE = r'''
local T = picocalc.sys.loadlib("picotest")
local sys, sound, fs, input = picocalc.sys, picocalc.sound, picocalc.fs, picocalc.input
local audio, disp = picocalc.audio, picocalc.display
-- %S+ (not %a+): "mp3" has a digit in it, which %a+ (letters only) can't
-- match, so MODE/SECONDS both came back nil and every "mp3" run failed
-- immediately on the arithmetic below.
local MODE, SECONDS = fs.readFile(fs.appPath("run.txt")):match("(%S+) (%d+)")
SECONDS = tonumber(SECONDS)

local function write(name, text)
  local f = fs.open(fs.appPath(name), "w")
  fs.write(f, text)
  fs.close(f)
end

local function caption(text)
  disp.clear(0)
  disp.drawText(8, 150, text, 0xFFFF, 0)
  disp.flush()
end

local STEPS = {
  {0, "1/7 music alone"},
  {4, "2/7 music + 8 voices"},
  {8, "3/7 music paused: fades out, voices go on"},
  {10, "4/7 music resumed: fades in"},
  {13, "5/7 music volume sweep"},
  {16, "6/7 master volume sweep"},
  {19, "7/7 music stopped: fades out, voices alone"},
}

T.case("play", function()
  local metrics = {mode = MODE}
  local mp3
  if MODE ~= "sfx" then
    mp3 = sound.mp3player()
    T.ok(mp3:load(APP_DIR .. "/music.mp3"), "load music.mp3")
    mp3:setVolume(60)
    mp3:play(0)                              -- loop
  end
  local voices = {}
  if MODE ~= "mp3" then
    for i = 1, 8 do
      local s = sound.sample(APP_DIR .. "/sfx" .. i .. ".wav")
      T.ok(s, "load sfx" .. i .. ".wav")
      voices[i] = sound.sampleplayer(s)
      T.ok(voices[i], "sampleplayer " .. i)
      voices[i]:setVolume(45)
    end
  end
  write("playing", "1")
  local t0 = sys.getTimeMs()
  local t_end, next_sfx, k, step = t0 + SECONDS * 1000, t0, 0, 0
  local paused_at, resumed = nil, false
  while sys.getTimeMs() < t_end do
    local now = sys.getTimeMs()
    local t = (now - t0) / 1000
    local blips = #voices > 0
    if MODE == "listen" then
      while step < #STEPS and t >= STEPS[step + 1][1] do
        step = step + 1
        caption(STEPS[step][2])
        if step == 3 then mp3:pause()
        elseif step == 4 then mp3:resume()
        elseif step == 6 then mp3:setVolume(60)
        elseif step == 7 then audio.setVolume(100); mp3:stop() end
      end
      blips = step >= 2
      if step == 5 then
        mp3:setVolume(math.floor(60 - 55 * math.sin(math.pi * (t - 13) / 3)))
      elseif step == 6 then
        audio.setVolume(math.floor(100 - 80 * math.sin(math.pi * (t - 16) / 3)))
      end
    elseif MODE == "both" then
      if not paused_at and t >= SECONDS * 0.4 then
        local u0 = sys.getTimeUs()
        mp3:pause()
        metrics.pause_us = sys.getTimeUs() - u0
        metrics.pos_at_pause = mp3:getPosition()
        T.ok(not mp3:isPlaying(), "isPlaying() while paused")
        paused_at = now
      elseif paused_at and not resumed and t >= SECONDS * 0.5 then
        metrics.pos_before_resume = mp3:getPosition()
        mp3:resume()
        resumed = true
      end
    end
    -- A blip every 50 ms, round robin: all eight voices overlap.
    if blips and now >= next_sfx then
      k = k % 8 + 1
      voices[k]:play()
      next_sfx = now + 50
    end
    input.update()
    sys.sleep(5)
  end
  if mp3 and MODE ~= "listen" then
    T.ok(mp3:isPlaying(), "the looping MP3 still plays")
    metrics.position = mp3:getPosition()
    local u0 = sys.getTimeUs()
    mp3:stop()
    metrics.stop_us = sys.getTimeUs() - u0
  end
  for i = 1, #voices do voices[i]:stop() end
  write("metrics.json", picocalc.json.encode(metrics))
end)
T.done()
'''


def measure(target, name, app_id, measure_s, start_timeout=60):
    """Launch `name`, wait for its `playing` marker, measure one audiostat
    window of `measure_s` seconds with nothing else sent to the device,
    then wait for the fixture to finish.
    Returns (stats, metrics, outcome, results)."""
    data = f"/data/{app_id}"
    for f in ("playing", "metrics.json", "test_results.json"):
        target.delete_file(f"{data}/{f}")
    target.ensure_launcher()
    target.launch_app(name)
    deadline = time.monotonic() + start_timeout
    while True:
        try:
            target.read_file(f"{data}/playing")
            break
        except FileNotFoundError:
            assert time.monotonic() < deadline, f"{name} never started playing"
            time.sleep(0.5)
    audiostat(target, "reset")
    time.sleep(measure_s)
    stats = audiostat(target)
    outcome = target.wait_for_exit(timeout=120)
    results = target.read_json(f"{data}/test_results.json")
    try:
        metrics = target.read_json(f"{data}/metrics.json")
    except FileNotFoundError:
        metrics = {}
    return stats, metrics, outcome, results


_staged = set()


@pytest.fixture
def mix_app(target):
    """Stage the audio_mix fixture (music + eight blips) once per session;
    returns run(mode, seconds=30, measure_s=20)."""
    if not FFMPEG:
        pytest.skip("ffmpeg makes the music")
    if APP not in _staged:
        # push_app's on-device extraction timeout assumes ~64 KB/s; SD
        # writes on this device run closer to 12 KB/s (project memory:
        # ref_hw_v2_findings), so the ~420 KB of music + eight blips in
        # one zip regularly outran the local timeout even though the
        # on-device extraction went on to finish (confirmed with `ls`
        # after a "did not complete" error). Stage the music with the
        # app, then the blips in a second push: `stage_lua_app` writes
        # the same app.json both times (no extra reboot), and unzip
        # merges into the existing /apps/audio_mix directory rather than
        # clearing it, so each push's own payload stays comfortably
        # under the timeout on its own.
        with tempfile.TemporaryDirectory() as tmp:
            music = Path(tmp) / "music.mp3"
            music_mp3(music)
            target.stage_lua_app(APP, FIXTURE, requirements=("audio",),
                                 id=APP_ID,
                                 files={"music.mp3": music.read_bytes()})
        with tempfile.TemporaryDirectory() as tmp:
            files = {}
            for i, hz in enumerate(SFX_HZ, 1):
                p = Path(tmp) / f"sfx{i}.wav"
                sfx_wav(p, hz)
                files[p.name] = p.read_bytes()
            target.stage_lua_app(APP, FIXTURE, requirements=("audio",),
                                 id=APP_ID, files=files)
        _staged.add(APP)

    def run(mode, seconds=30, measure_s=20):
        target.write_file(f"/data/{APP_ID}/run.txt",
                          f"{mode} {seconds}".encode())
        return measure(target, APP, APP_ID, measure_s)
    return run


def test_mp3_music_alone(mix_app):
    """MP3 music alone: Core 1's tick cost and the MP3 staging underruns."""
    stats, m, outcome, results = mix_app("mp3")
    print("music alone:", stats, m)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    assert stats["window_ms"] >= 15000 and stats["ticks"] > 0, stats
    assert stats["out"] == 1, stats
    assert stats["mp3_underruns"] == 0, stats
    assert stats["stream_underruns"] == 0, stats
    assert_tick_cost_within(stats, BASELINE["mp3"])


def test_eight_sample_voices(mix_app):
    """Eight overlapping sample voices: Core 1's tick cost and the refill
    interrupt's."""
    stats, m, outcome, results = mix_app("sfx")
    print("8 voices:", stats, m)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    assert stats["window_ms"] >= 15000 and stats["ticks"] > 0, stats
    assert stats["voices"] == 8, stats
    assert stats["stream_underruns"] == 0, stats
    assert_tick_cost_within(stats, BASELINE["sfx"])


def test_music_with_eight_voices(mix_app):
    """MP3 music and eight sample voices at once (they could not share the
    output before): no MP3 underruns, eight busy voices, and the pause and
    resume in the middle fade within the bound and hold the position."""
    stats, m, outcome, results = mix_app("both")
    print("music + 8 voices:", stats, m)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    assert stats["out"] == 1 and stats["voices"] == 8, stats
    assert stats["mp3_underruns"] == 0, stats
    assert stats["stream_underruns"] == 0, stats
    # The fade waits at most 50 ms; then pause/stop can wait on s_mp3_mutex
    # behind a Core 1 decode burst (~55 ms measured).
    assert m["pause_us"] <= 120000 and m["stop_us"] <= 120000, m
    assert m["pos_before_resume"] == m["pos_at_pause"], m
    assert m["position"] > m["pos_before_resume"], m
    assert_tick_cost_within(stats, BASELINE["both"])


def test_exit_stops_the_output(mix_app, target):
    """After an app that played music and eight voices exits, the launcher
    runs with the output stopped: no refill interrupts at all."""
    stats, _, outcome, results = mix_app("both", seconds=8, measure_s=3)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    assert stats["out"] == 1, stats
    audiostat(target, "reset")
    time.sleep(1.0)
    after = audiostat(target)
    assert after["out"] == 0 and after["isr"] == 0, after
    assert after["voices"] == 0, after


@pytest.mark.skipif(not LISTEN, reason="AUDIO_LISTEN=1: a person listens at the device")
def test_listen(mix_app):
    """The captioned sequence (see STEPS in FIXTURE): music alone, + eight
    voices, pause and resume, a music volume sweep, a master volume sweep,
    stop. The person listening reports any crackle, click, pop or dropout
    against the on-screen step; the run itself only has to finish cleanly
    with no MP3 underruns."""
    stats, _, outcome, results = mix_app("listen", seconds=23, measure_s=20)
    print("listen:", stats)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    assert stats["mp3_underruns"] == 0, stats


VIDEO_APP, VIDEO_ID = "audio_video", "com.test.audio_video"

VIDEO_FIXTURE = r'''
-- A looping sample starts the output at 200 MHz, then a looping clip with
-- MP3 audio plays (the player boosts to 300 MHz) for SECONDS.
local T = picocalc.sys.loadlib("picotest")
local sys, sound, fs, input = picocalc.sys, picocalc.sound, picocalc.fs, picocalc.input
local SECONDS = 14
T.case("video", function()
  local s = sound.sample(APP_DIR .. "/sfx1.wav")
  T.ok(s, "load sfx1.wav")
  local voice = sound.sampleplayer(s)
  voice:setVolume(30)
  voice:play(0)                          -- loop: the output runs from here
  local v = picocalc.video.player()
  T.ok(v:load(APP_DIR .. "/clip.avi"), "load clip.avi")
  v:setLoop(true)
  v:play()
  local f = fs.open(fs.appPath("playing"), "w")
  fs.write(f, "1")
  fs.close(f)
  local t_end = sys.getTimeMs() + SECONDS * 1000
  while sys.getTimeMs() < t_end do
    v:update()
    input.update()
  end
  T.ok(v:isPlaying(), "the clip still plays")
  v:stop()
  voice:stop()
end)
T.done()
'''


def test_output_rate_survives_the_video_boost(target):
    """The output, started at 200 MHz by a sample, keeps 44.1 kHz after the
    video player boosts clk_sys to 300 MHz (audio_apply_clock re-derives the
    PWM divider), with the clip's MP3 audio mixed in: under 10% of the
    window's frames find no MP3 (each loop restart leaves a short gap,
    ~2.5% measured). After the app, the output is off and the clock back
    at 200 MHz."""
    if not FFMPEG:
        pytest.skip("ffmpeg makes the clip")
    with tempfile.TemporaryDirectory() as tmp:
        clip = Path(tmp) / "clip.avi"
        # 3 s at 20 fps (~218 KB; it loops). With sfx1.wav the push stays
        # under ~250 KB, inside push_app's extraction timeout (see mix_app).
        # The frame rate keeps the MP3 in the video player's audio index,
        # which holds frames * 1.5 + 64 chunks: ffmpeg writes one 26 ms MP3
        # frame per chunk, so at 10 fps the index ran out ~1.4 s before the
        # end of a 5 s clip and the MP3 went silent for the rest of it.
        subprocess.run([FFMPEG, "-v", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc=size=160x120:rate=20",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
                        "-t", "3", "-c:v", "mjpeg", "-pix_fmt", "yuvj420p",
                        "-q:v", "12", "-c:a", "libmp3lame", "-b:a", "96k",
                        "-ac", "2", str(clip)], check=True)
        sfx = Path(tmp) / "sfx1.wav"
        sfx_wav(sfx, SFX_HZ[0])
        target.stage_lua_app(VIDEO_APP, VIDEO_FIXTURE, requirements=("audio",),
                             id=VIDEO_ID,
                             files={"clip.avi": clip.read_bytes(),
                                    "sfx1.wav": sfx.read_bytes()})
    stats, _, outcome, results = measure(target, VIDEO_APP, VIDEO_ID, measure_s=6)
    print("video boost:", stats)
    assert outcome["result"] == "returned", outcome
    assert_passed(results)
    rate = stats["isr"] * 1000 / stats["window_ms"]
    assert stats["sys_khz"] == 300000, stats
    assert 334 <= rate <= 355, f"{rate:.1f} refills/s at 300 MHz, want ~344.5: {stats}"
    frames = 44.1 * stats["window_ms"]
    assert stats["mp3_underruns"] * 10 < frames, stats
    after = audiostat(target)
    assert after["out"] == 0 and after["sys_khz"] == 200000, after
