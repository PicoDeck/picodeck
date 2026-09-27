#!/usr/bin/env bash
# Build the browser simulator (make simulator-web) with the pinned Emscripten
# and package it as picodeck-web-sim.zip for picodeck.net/try.  Used by
# build-sim.yml (build check on every push) and release.yml (release asset).
set -euo pipefail
EMSDK_VERSION=6.0.10
EMSDK="${EMSDK:-$HOME/emsdk}"
[ -x "$EMSDK/emsdk" ] || git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK"
"$EMSDK/emsdk" install "$EMSDK_VERSION"
"$EMSDK/emsdk" activate "$EMSDK_VERSION"
make simulator-web EMSDK="$EMSDK" 2>&1 | tee web-build.log
if grep -q 'function signature mismatch' web-build.log; then
  echo "::error::wasm-ld reported a function signature mismatch; WebAssembly traps on these at run time"
  exit 1
fi
rm -rf web-sim picodeck-web-sim.zip
mkdir web-sim
cp build_web/picodeck_simulator.js build_web/picodeck_simulator.wasm build_web/picodeck_simulator.data build_web/shell.js web-sim/
cp build_web/picodeck_simulator.html web-sim/index.html
(cd web-sim && zip -qr ../picodeck-web-sim.zip .)
echo "picodeck-web-sim.zip: $(du -h picodeck-web-sim.zip | cut -f1)"
