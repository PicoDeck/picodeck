#!/usr/bin/env bash
# FatFS R0.15 upstream sources only; ffconf.h and port/diskio_spi.c are tracked.
# Used by the firmware build (fetch-firmware-deps.sh) and the unit tests, which
# compile ff.c for test_mv_op. elm-chan.org does not always answer CI runners,
# so the zip is kept in FATFS_ZIP_CACHE (default ~/.cache/picodeck, which the
# workflows persist with actions/cache) and checked against its SHA-256.
set -euo pipefail
FF_ZIP_SHA256=e0d76654d877e6c74be5ea3c395808794d495169514e98cbf6046168b8f4f070
cache="${FATFS_ZIP_CACHE:-$HOME/.cache/picodeck}"
zip="$cache/ff15.zip"
mkdir -p "$cache"
if ! echo "$FF_ZIP_SHA256  $zip" | sha256sum -c --quiet 2>/dev/null; then
  curl -fL --retry 5 --retry-delay 10 --retry-connrefused --connect-timeout 30 \
    --speed-limit 1024 --speed-time 60 --max-time 600 \
    https://elm-chan.org/fsw/ff/arc/ff15.zip -o "$zip.part"
  echo "$FF_ZIP_SHA256  $zip.part" | sha256sum -c --quiet
  mv "$zip.part" "$zip"
fi
unzip -o -j "$zip" \
  'source/ff.c' 'source/ff.h' 'source/diskio.h' \
  'source/ffsystem.c' 'source/ffunicode.c' \
  -d third_party/fatfs/
