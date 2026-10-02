#!/usr/bin/env bash
# FatFS R0.15 upstream sources only; ffconf.h and port/diskio_spi.c are tracked.
# Used by the firmware build (fetch-firmware-deps.sh) and the unit tests, which
# compile ff.c for test_mv_op.
set -euo pipefail
curl -fL https://elm-chan.org/fsw/ff/arc/ff15.zip -o /tmp/ff15.zip
unzip -o -j /tmp/ff15.zip \
  'source/ff.c' 'source/ff.h' 'source/diskio.h' \
  'source/ffsystem.c' 'source/ffunicode.c' \
  -d third_party/fatfs/
