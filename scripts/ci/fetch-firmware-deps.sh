#!/usr/bin/env bash
# Toolchain and third-party sources for a firmware build on Ubuntu CI
# runners (mirrors the steps in release.yml). Pico SDK goes to $HOME/pico-sdk.
set -euo pipefail
sudo apt-get update -q
sudo apt-get install -y cmake curl unzip gcc-arm-none-eabi libnewlib-arm-none-eabi \
  libstdc++-arm-none-eabi-newlib

# Lua 5.4.7, patched to honour the CMake Lua config (see cmake/picodeck_lua.cmake).
make download-lua

scripts/ci/fetch-fatfs.sh

if [ ! -d "$HOME/pico-sdk" ]; then
  git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git "$HOME/pico-sdk"
  (cd "$HOME/pico-sdk" && git submodule update --init --depth 1)
fi
