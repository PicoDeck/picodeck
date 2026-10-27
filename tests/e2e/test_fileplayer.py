"""WAV fileplayer flow control and per-player files (review: Audio/storage
Critical row 1 "The WAV fileplayer f_reads 4 KB ... a long WAV ends in
seconds", High row "Cross-core use-after-free ... One s_current_file is
shared by all fileplayer instances"; Task 14).

The simulator runs the firmware src/drivers/fileplayer.c and drains the
stream ring (audio_ring.h) at the real 44.1 kHz output rate, so wall-clock
play time is a faithful check of the producer's flow control: a fileplayer
that reads faster than the ring drains drops audio and finishes early.
Each case runs alone in its own simulator.

#34 (stream gaps): test_looping_stream_plays_without_gaps runs on the
simulator and on the device (@pytest.mark.both, `--target hw:<port>`) and
reads `audiostat`'s stream fields over a whole play: no underruns at the
start or at the loop points. test_audiostat_reports_a_stall (simulator
only: set_sd_busy) checks what those fields say about a stall.
"""

import io
import math
import re
import struct
import tempfile
import time
import wave
from pathlib import Path

import pytest

from helpers import case_params, lua_case_names, write_qoa, write_wav

APP = "fileplayer_test"
CASES = lua_case_names(APP)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


def _setup(case):
    def setup(sd):
        app = sd / "apps" / APP
        write_wav(app / "three_s.wav", seconds=3.0)
        write_wav(app / "one_s.wav", seconds=1.0)
        write_wav(app / "empty.wav", seconds=0.0)  # a data chunk with no frames
        # Files load() must refuse: an MP3 (ID3 tag), an 8-bit WAV, junk.
        (app / "fake.mp3").write_bytes(b"ID3\x03\x00\x00\x00\x00\x00\x00" + bytes(64))
        with wave.open(str(app / "eight_bit.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(1)
            w.setframerate(22050)
            w.writeframes(b"\x80" * 2205)
        (app / "junk.bin").write_bytes(b"not audio at all, just text" * 4)
        (app / "only.flag").write_text(case)
    return setup


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_fileplayer(lua_suite, case):
    run = lua_suite(APP, setup=_setup(case), timeout=60)
    run.check_case(case)
    run.assert_clean_exit()


# ── #34: stream gaps at the start, the loop point and a stall (audiostat) ────
# `audiostat` (src/dev_ops.c, the simulator's dev_command RPC runs it too)
# says when the stream ran dry: stream_start_underruns (a stream's first
# second), stream_loop_underruns (the second after a loop point),
# stream_gaps, stream_first_ms / stream_last_ms (ms into the stream) and
# stream_low_ms (the ring's low-water mark).

GAPS_APP, GAPS_ID = "stream_gaps", "com.test.stream_gaps"
GAPS_FIXTURE = r'''
-- run.txt: "<file> <seconds>". Plays APP_DIR/<file> looping (play(0)) and,
-- right after play(), loads a 44 KB sample: Core 0 holds the SD card for
-- the load, as Nova Rail's race start does. Then it plays for <seconds>,
-- or until /data/<id>/stop appears, and stops. "playing" marks the start.
local T = picocalc.sys.loadlib("picotest")
local sys, sound, fs = picocalc.sys, picocalc.sound, picocalc.fs
local FILE, SECONDS = fs.readFile(fs.appPath("run.txt")):match("(%S+) (%d+)")
SECONDS = tonumber(SECONDS)

T.case("play", function()
  local fp = sound.fileplayer()
  T.ok(fp:load(APP_DIR .. "/" .. FILE), "load " .. FILE)
  fp:setVolume(40)
  fp:play(0)
  local s = sound.sample(APP_DIR .. "/load.wav")
  T.ok(s, "load the sample")
  local f = fs.open(fs.appPath("playing"), "w")
  fs.write(f, "1")
  fs.close(f)
  local t_end = sys.getTimeMs() + SECONDS * 1000
  while sys.getTimeMs() < t_end and not fs.exists(fs.appPath("stop")) do
    sys.sleep(20)
  end
  T.ok(fp:isPlaying(), "the looping file still plays")
  fp:stop()
end)
T.done()
'''


def _sine_wav(seconds: float, rate: int, channels: int, hz: int = 440) -> bytes:
    """A 16-bit PCM WAV of a sine (whole cycles: it loops without a click)."""
    period = [struct.pack("<h", int(9000 * math.sin(2 * math.pi * i / (rate / hz))))
              * channels for i in range(rate // hz)]
    cycle = b"".join(period)
    frames = int(seconds * rate) // (rate // hz) * (rate // hz)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(cycle * (frames // (rate // hz)))
    return buf.getvalue()


def _qoa(seconds: float, rate: int, channels: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "m.qoa"
        write_qoa(p, seconds=seconds, rate=rate, channels=channels, hz=441)
        return p.read_bytes()


def _audiostat(target, arg=""):
    """One `audiostat` reply as a dict of ints (as test_audio_hw.py reads it)."""
    for line in target.command(("audiostat " + arg).strip(), timeout=3.0):
        if "[DEV] Audio:" in line:
            return {k: int(v) for k, v in
                    re.findall(r"(\w+)=(-?\d+)", line.split("Audio:", 1)[1])}
    raise AssertionError(f"no Audio reply to 'audiostat {arg}'")


def _stage_gaps(target, music_name, music, seconds):
    target.stage_lua_app(GAPS_APP, GAPS_FIXTURE, requirements=("audio",),
                         id=GAPS_ID,
                         files={music_name: music,
                                "load.wav": _sine_wav(1.0, 22050, 1, 300)})
    for f in ("playing", "stop", "test_results.json"):
        target.delete_file(f"/data/{GAPS_ID}/{f}")
    target.write_file(f"/data/{GAPS_ID}/run.txt",
                      f"{music_name} {seconds}".encode())


LOOP_S = 0.5     # each file: a loop point every half second
PLAY_S = 4
MUSIC = {
    "wav44s": ("m.wav", lambda: _sine_wav(LOOP_S, 44100, 2)),
    "wav22m": ("m.wav", lambda: _sine_wav(LOOP_S, 22050, 1)),
    "qoa44s": ("m.qoa", lambda: _qoa(LOOP_S, 44100, 2)),
    "qoa22m": ("m.qoa", lambda: _qoa(LOOP_S, 22050, 1)),
}


@pytest.mark.both
@pytest.mark.timeout(600)
@pytest.mark.sd(fixtures=[], reserve=1)
@pytest.mark.parametrize("fmt", sorted(MUSIC))
def test_looping_stream_plays_without_gaps(target, fmt):
    """A looping WAV or QOA plays from play() to stop() without the stream
    running dry: not at the start, where Core 0 holds the SD card for a
    sample load right after play() (play() starts the stream held, and the
    player releases it with the ring full), and not at the loop points.
    The simulator's SD loads take no time, so there Core 1 finds the card
    busy for 300 ms instead (set_sd_busy): after its first read for a WAV
    (a stream playing that first 4 KB would run dry for ~200-280 ms), from
    its first read for a QOA (whose first frame fills the ring without
    another read)."""
    name, make = MUSIC[fmt]
    _stage_gaps(target, name, make(), PLAY_S)
    if target.kind == "hw":
        target.ensure_launcher()  # the window starts at the launcher
    _audiostat(target, "reset")
    if target.kind == "sim":
        after = 1 if name.endswith(".wav") else 0
        target.sim.call("set_sd_busy", {"ms": 300, "after_reads": after})
    target.launch_app(GAPS_APP)
    outcome = target.wait_for_exit(timeout=60)
    stats = _audiostat(target)
    print(fmt, stats)
    assert outcome["result"] == "returned", outcome
    results = target.read_json(f"/data/{GAPS_ID}/test_results.json")
    assert results.get("done") and all(
        c["status"] == "PASS" for c in results["cases"]), results
    assert stats["stream_starts"] == 1, stats
    assert stats["stream_loops"] >= PLAY_S / LOOP_S - 2, stats
    assert stats["stream_underruns"] == 0, stats
    assert stats["stream_gaps"] == 0, stats
    assert stats["stream_first_ms"] == -1, stats
    assert stats["stream_low_ms"] >= 0, stats   # it played


@pytest.mark.sd(fixtures=[], reserve=1)
@pytest.mark.timeout(120)
def test_audiostat_reports_a_stall(simulator):
    """Mid-stream, Core 1 cannot read the card for a while (set_sd_busy):
    a stall shorter than the ring (93 ms of 44.1 kHz stereo) only lowers its
    low-water mark; a longer one is one gap, reported with where it began
    and neither as the start nor as a loop point."""
    from hw_target import SimTarget
    target = SimTarget(simulator)
    _stage_gaps(target, "m.wav", _sine_wav(10.0, 44100, 2), 30)
    target.launch_app(GAPS_APP)
    deadline = time.monotonic() + 15
    while True:
        try:
            target.read_file(f"/data/{GAPS_ID}/playing")
            break
        except FileNotFoundError:
            assert time.monotonic() < deadline, "the fixture never started playing"
            time.sleep(0.05)
    time.sleep(1.5)                      # past the stream's first second

    _audiostat(target, "reset")
    simulator.call("set_sd_busy", {"ms": 40})
    time.sleep(0.5)
    short = _audiostat(target)
    print("40 ms stall:", short)
    assert short["stream_underruns"] == 0, short
    assert short["stream_gaps"] == 0, short
    assert 0 <= short["stream_low_ms"] < 80, short  # the ring went down, not dry

    _audiostat(target, "reset")
    simulator.call("set_sd_busy", {"ms": 400})
    time.sleep(1.0)
    long_ = _audiostat(target)
    print("400 ms stall:", long_)
    # ~300 ms dry (the stall less the ring), give or take the sim's 5 ms tick.
    assert 5000 < long_["stream_underruns"] < 20000, long_
    assert long_["stream_gaps"] == 1, long_
    assert long_["stream_start_underruns"] == 0, long_
    assert long_["stream_loop_underruns"] == 0, long_
    assert long_["stream_first_ms"] == long_["stream_last_ms"], long_
    assert 1500 <= long_["stream_first_ms"] < 10000, long_
    assert long_["stream_low_ms"] == 0, long_

    target.write_file(f"/data/{GAPS_ID}/stop", b"1")
    outcome = simulator.wait_for_exit(timeout=30)
    assert outcome["result"] == "returned", outcome
