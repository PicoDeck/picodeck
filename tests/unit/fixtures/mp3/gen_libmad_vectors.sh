#!/bin/bash
# The libmad vectors of tests/unit/test_mp3_granule.c beyond the chords
# (issue #28): LSF and MPEG-2.5 joint stereo, CRCs, intensity stereo.
# Run in tests/unit/fixtures/mp3 (needs ffmpeg, lame 3.100, python3);
# the golden hashes in the test come from libmad before the granule split,
# so regenerated files need new ones.
set -e
cd "$(dirname "$0")"
tmp=.gen_tmp; mkdir -p "$tmp"
SRC="0.35*sin(2*PI*220*t)+0.25*sin(2*PI*330*t)+lt(mod(t\,0.25)\,0.012)*0.8*sin(2*PI*3100*t)|0.3*sin(2*PI*137*t)+0.15*sin(2*PI*1760*t)+lt(mod(t+0.11\,0.25)\,0.012)*0.8*sin(2*PI*2300*t)"
for r in 44100 22050 11025; do
  ffmpeg -v error -y -f lavfi -i "aevalsrc=$SRC:s=$r:d=1" -fflags +bitexact -flags:a +bitexact -c:a pcm_s16le "$tmp/src$r.wav"
done
lame --quiet -t -m j -b 64 "$tmp/src22050.wav" lsf_j22.mp3        # MPEG-2, joint stereo
lame --quiet -t -m j -b 32 "$tmp/src11025.wav" m25_j11.mp3        # MPEG-2.5
lame --quiet -t -p -m j -b 128 "$tmp/src44100.wav" crc_j128.mp3   # MPEG-1 with CRCs
lame --quiet -t -m j -b 128 "$tmp/src44100.wav" "$tmp/j128.mp3"
python3 mkis.py "$tmp/j128.mp3" is_j128.mp3                        # intensity stereo, MPEG-1
python3 mkis.py lsf_j22.mp3 is_lsf_j22.mp3                         # intensity stereo, MPEG-2
rm -r "$tmp"
