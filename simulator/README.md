# PicoDeck PC Simulator

A cross-platform SDL2-based simulator for PicoDeck that runs on desktop computers for development, debugging, and testing.

App authors: start with [Simulator and Testing](../docs/Simulator-and-Testing.md), which has the download, the card layout, the keys, the control port and the full list of differences from the device. This file is the contributor's view. [`CLAUDE.md`](CLAUDE.md) has the implementation notes behind those differences, and the test control channel.

## Features

- **3x Scale Display**: 960×960 window simulating the 320×320 LCD
- **Dual-Core Simulation**: Core 0 (Lua VM) and Core 1 (audio/network) run in separate threads
- **Lua and native apps**: Lua apps run on the same bridge as the firmware; native ELF apps run under the Unicorn CPU emulator, and a few are compiled straight into the simulator
- **Keyboard and game controllers**: full keyboard mapping, plus host SDL game controllers feeding `picocalc.gamepad` (and a scripted Bluetooth radio for the pairing UI)
- **Host Filesystem**: SD card simulated as local directory
- **Audio Support**: SDL2 audio output
- **Test control channel**: a JSON-RPC socket for launching apps, injecting input, reading logs and stepping a virtual clock; the E2E suite (`tests/e2e`) drives it
- **Sanitizer builds**: ASan/UBSan and TSan variants, and a build on the firmware's network stack
- **Browser build**: the same sources build to WASM for [picodeck.net/try](https://picodeck.net/try/) (see below)
- **Debuggable**: Use GDB/LLDB, printf debugging, IDE breakpoints

## Building

### Prerequisites

- A clone with its submodules: `git clone --recurse-submodules https://github.com/PicoDeck/picodeck.git` (in an existing clone, `git submodule update --init --recursive`). `make simulator` stops and says so if they are missing.
- CMake 3.13+
- C and C++ compilers (GCC or Clang)
- SDL2, SDL2_image and libcurl development libraries
- Network access at the first configure: CMake fetches mbedTLS and Unicorn, and `make simulator` fetches Lua. The Pico SDK is not needed.

CI builds and tests on Ubuntu; other systems are not tested.

**Fedora:**
```bash
sudo dnf install cmake gcc gcc-c++ SDL2-devel SDL2_image-devel libcurl-devel
```

**Debian/Ubuntu:**
```bash
sudo apt install cmake build-essential libsdl2-dev libsdl2-image-dev libcurl4-openssl-dev
```

**macOS:**
```bash
brew install cmake sdl2 sdl2_image curl
```

### Build Commands

From the repository root:

```bash
make simulator          # Release build -> build_sim/picodeck_simulator
make simulator-run      # Build and run, with this checkout as the SD card
make simulator-asan     # ASan + UBSan (build_sim_asan/); also simulator-tsan, simulator-net
```

or by hand (`make download-lua` first: it fetches and patches Lua):

```bash
make download-lua
mkdir build_sim && cd build_sim
cmake ../simulator -DCMAKE_BUILD_TYPE=Release
make -j4
./picodeck_simulator
```

## Usage

`--sd-card` names the card's root, not the apps folder: the launcher scans `<root>/apps/<name>/`. The simulator stops with a message if the path is not a folder, or if it holds app folders but no `apps/`.

```bash
# The source tree it was built from is the card (its apps/ and system/lib/)
./picodeck_simulator

# A card of your own: a folder with each app in mycard/apps/<name>/
./picodeck_simulator --sd-card mycard

# Play the boot splash animation, then its status steps (for screencasting)
./picodeck_simulator --show-splash

# Launch an app at startup (by id, display name or folder name)
./picodeck_simulator --launch hello

# Deterministic runs for tests (fixed clock and random seed, error screens return at once)
./picodeck_simulator --test-mode --virtual-time

# Enable debug logging
./picodeck_simulator --debug

# Show help
./picodeck_simulator --help
```

A binary moved off the computer it was built on (the published `picodeck-simulator-linux`, say) has no source tree to fall back on, so it needs `--sd-card`.

## Keyboard Mapping

| PC Key | PicoDeck Button |
|--------|-------------|
| Arrow Keys | BTN_UP/DOWN/LEFT/RIGHT |
| Enter | BTN_ENTER |
| Escape | BTN_ESC |
| F1-F5 | BTN_F1-F5 |
| F10 | BTN_MENU, the system menu key (Shift+F5, reported as F10, on the device) |
| F12 | Save a screenshot to `/screenshots` on the card (Shift+Esc on the device) |
| Tab | BTN_TAB |
| Backspace | BTN_BACKSPACE |
| Delete | BTN_DEL |
| Home | BTN_FN |
| Ctrl | BTN_CTRL |
| Shift | BTN_SHIFT |

## Project Structure

```
simulator/
├── CMakeLists.txt          # Simulator build configuration (also the Emscripten build)
├── main.c                  # SDL2 main loop, dual-core simulation
├── hal/                    # Hardware Abstraction Layer
│   ├── hal_display.c/h     # SDL2 320×320 display
│   ├── hal_input.c/h       # Keyboard → button mapping
│   ├── hal_pad.c/h         # Host game controllers
│   ├── hal_sdcard.c/h      # Host filesystem
│   ├── hal_psram.c/h       # Host memory allocation
│   ├── hal_timing.c/h      # SDL2 timing
│   ├── hal_audio.c/h       # SDL2 audio
│   ├── hal_threading.c/h   # POSIX threads
│   └── web_platform.h      # Contract with the browser build (PicoDeck/web-sim)
├── unicorn_runner.c/h      # Runs native ELF apps under Unicorn
├── unicorn_trampolines.c   # The PicoCalcAPI table, as seen by those apps
├── sim_native_api.c/h      # The same API for apps compiled into the simulator
├── sim_socket*.c/h         # JSON-RPC control channel
├── sim_test_control.c/h    # Test hooks (virtual time, input injection)
├── sim_wifi.c, sim_http.c, sim_tcp.c, sim_bt.c, sim_audio.c  # Scripted radios and host-backed I/O
├── stubs/                  # Pico SDK, driver and keyboard stubs
└── assets/
    └── sd_card/            # A small card the E2E harness defaults to
```

## Differences from Hardware

The list app authors should read is in [Simulator and Testing](../docs/Simulator-and-Testing.md#simulator-vs-device). The implementation notes behind it are in [`CLAUDE.md`](CLAUDE.md).

## Browser Build

The demo at [picodeck.net/try](https://picodeck.net/try/) is built from these sources by [PicoDeck/web-sim](https://github.com/PicoDeck/web-sim), not by this repository. It runs Lua apps and native apps compiled into the page (DOOM is one), but not Unicorn-emulated ELF apps (no JIT in WASM), and has no networking. `hal/web_platform.h` is the contract between the two projects.

## Automated Testing

CI builds the simulator and drives it with the pytest suite in [`tests/e2e`](../tests/e2e/README.md) (see `.github/workflows/e2e.yml` and `build-sim.yml`):

```bash
make simulator
pip install -r tests/e2e/requirements.txt
SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
```

Host unit tests (`make test-unit`) and the nightly fuzzers (`make fuzz`) do not need the simulator.

## Future Enhancements

- [ ] GDB remote debugging support
- [ ] Video capture for regression testing
