# PicoDeck PC Simulator

A cross-platform SDL2-based simulator for PicoDeck that runs on desktop computers for development, debugging, and testing.

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

See [`CLAUDE.md`](CLAUDE.md) for where the simulator diverges from the device and for the control protocol, and [Simulator and Testing](../docs/Simulator-and-Testing.md) for the test commands.

## Building

### Prerequisites

- CMake 3.13+
- SDL2 development libraries
- C/C++ compiler (GCC, Clang, MSVC)

**Fedora/Linux:**
```bash
sudo dnf install cmake SDL2-devel
```

**macOS:**
```bash
brew install cmake sdl2
```

**Windows (MSYS2):**
```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2
```

### Build Commands

From the repository root:

```bash
make simulator          # Release build -> build_sim/picodeck_simulator
make simulator-run      # Build and run
make simulator-asan     # ASan + UBSan (build_sim_asan/); also simulator-tsan, simulator-net
```

or by hand:

```bash
mkdir build_sim && cd build_sim
cmake ../simulator -DCMAKE_BUILD_TYPE=Release
make -j4
./picodeck_simulator --sd-card ../apps
```

## Usage

```bash
# Run with default SD card path (.)
./picodeck_simulator

# Run with custom SD card path
./picodeck_simulator --sd-card /path/to/apps

# Play the boot splash animation, then its status steps (for screencasting)
./picodeck_simulator --show-splash

# Launch an app at startup
./picodeck_simulator --launch hello

# Deterministic runs for tests (fixed clock and random seed, error screens return at once)
./picodeck_simulator --test-mode --virtual-time

# Enable debug logging
./picodeck_simulator --debug

# Show help
./picodeck_simulator --help
```

## Keyboard Mapping

| PC Key | PicoDeck Button |
|--------|-------------|
| Arrow Keys | BTN_UP/DOWN/LEFT/RIGHT |
| Enter | BTN_ENTER |
| Escape | BTN_ESC |
| F1-F4 | BTN_F1-F4 |
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
    └── sd_card/            # Simulated SD card
        ├── apps/           # Lua apps
        ├── data/           # App data
        └── system/         # System config
```

## Differences from Hardware

1. **No actual GPIO**: Hardware drivers are stubbed
2. **WiFi, HTTP and TCP use the host's network** (or a scripted radio under test); Bluetooth is a scripted radio
3. **No `picocalc.crypto`**: mbedTLS is firmware-only
4. **Faster execution**: PC is faster than RP2350, and native apps run under an emulator, not on a real core
5. **More memory**: Uses host RAM instead of limited PSRAM (`--real-umm` uses the firmware's heap allocator for realistic fragmentation)
6. **Different timing**: SDL2 timing vs hardware timers (`--virtual-time` makes it deterministic)

The full list is in [`CLAUDE.md`](CLAUDE.md).

## Browser Build

The demo at [picodeck.net/try](https://picodeck.net/try/) is built from these sources by [PicoDeck/web-sim](https://github.com/PicoDeck/web-sim), not by this repository. It runs Lua apps and native apps compiled into the page (DOOM is one), but not Unicorn-emulated ELF apps (no JIT in WASM), and has no networking. `hal/web_platform.h` is the contract between the two projects.

## Automated Testing

CI builds the simulator and drives it with the pytest suite in [`tests/e2e`](../tests/e2e/README.md) (see `.github/workflows/e2e.yml` and `build-sim.yml`):

```bash
make simulator
SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
```

Host unit tests (`make test-unit`) and the nightly fuzzers (`make fuzz`) do not need the simulator.

## Future Enhancements

- [ ] GDB remote debugging support
- [ ] Video capture for regression testing
- [ ] Network passthrough to host for every network API

## License

Same as PicoDeck (see main project LICENSE)
