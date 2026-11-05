#!/usr/bin/env bash
# How much the estimate moves with what the model cannot know. Run by `make sweep`.
#   sweep.sh <perf_rig> <guest.elf>
set -euo pipefail
rig="$1"
elf="$2"
one() {
  local name="$1"
  shift
  "$rig" "$elf" "$@" | awk -v name="$name" '/time at clock/ {
    printf "  %-34s %8s %8s %8s\n", name, $(NF-2), $(NF-1), $NF }'
}
printf "  %-34s %8s %8s %8s\n" "estimate (ms), device 6.30 4.10" geometry raster total
one "as shipped (LRU, cold, no dirty)"
one "replacement: FIFO" --policy fifo
one "replacement: random" --policy random
one "cache kept between frames" --warm
one "25% of lines dirty at frame start" --dirty-frac 0.25
one "50% of lines dirty at frame start" --dirty-frac 0.5
one "100% of lines dirty at frame start" --dirty-frac 1
one "taken branch +1 not +2" --branch 1
one "taken branch +3" --branch 3
one "load +0 not +1" --load 0
one "store +0 not +1" --store 0
one "instruction 0.8 cycles (dual issue)" --instr 0.8
one "XIP data hit +1" --xip-hit 1
