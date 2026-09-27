#!/usr/bin/env bash
# Add launcher.png to picodeck-web-sim.zip: the launcher as the web demo shows it,
# captured from the native simulator on the demo's SD image, for picodeck.net's
# home page. Run after build-web-sim.sh (it needs build_web/web_sd and the zip)
# with the simulator's dependencies installed (install-sim-deps.sh).
set -euo pipefail
if [ ! -d build_web/web_sd ] || [ ! -f picodeck-web-sim.zip ]; then
  echo "capture-launcher: run scripts/ci/build-web-sim.sh first" >&2
  exit 1
fi
make simulator
python3 scripts/ci/capture-launcher.py build_sim/picodeck_simulator build_web/web_sd launcher.png
zip -qj picodeck-web-sim.zip launcher.png
echo "picodeck-web-sim.zip: added launcher.png"
