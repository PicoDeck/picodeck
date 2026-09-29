"""WAV fileplayer flow control and per-player files (review: Audio/storage
Critical row 1 "The WAV fileplayer f_reads 4 KB ... a long WAV ends in
seconds", High row "Cross-core use-after-free ... One s_current_file is
shared by all fileplayer instances"; Task 14).

The simulator runs the firmware src/drivers/fileplayer.c and drains the
stream ring (audio_ring.h) at the real 44.1 kHz output rate, so wall-clock
play time is a faithful check of the producer's flow control: a fileplayer
that reads faster than the ring drains drops audio and finishes early.
Each case runs alone in its own simulator.
"""

import wave

import pytest

from helpers import case_params, lua_case_names, write_wav

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
