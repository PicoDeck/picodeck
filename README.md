# PicoDeck

**Website:** https://picodeck.net · **Try it in your browser:** https://picodeck.net/try/ · **Docs:** https://picodeck.net/docs/ · **App Store:** https://store.picodeck.net

An app platform for the ClockworkPi PicoCalc, built around a resident Lua runtime. Apps live on the SD card as directories containing `main.lua` and `app.json`. The OS owns all hardware; apps access everything through the `picocalc` Lua API.

> [!IMPORTANT]
> **Target hardware:** [Pimoroni Pico Plus 2 W](https://shop.pimoroni.com/products/pimoroni-pico-plus-2-w) in the [PicoCalc](https://www.clockworkpi.com/picocalc) device.
> PicoDeck relies heavily on the PSRAM being accessible via XIP, other hardware support is untested currently.

**New here?** [Getting started on a device](https://picodeck.net/docs/getting-started-on-a-device/) goes from a PicoCalc to a running app, and [Creating your first app](https://picodeck.net/docs/creating-your-first-app/) writes one. The full SDK reference is on the [docs site](https://picodeck.net/docs/).

## Features

- **Apps in Lua or native C/C++.** Lua 5.4 apps get a `picocalc.*` API; native apps are position-independent ELF binaries using the same API through a C table (see [Native Loading](https://picodeck.net/docs/native-loading/)). The first-party emulators (C64, Game Boy Color, DOS/86, TIC-80) are native.
- **Graphics.** Double-buffered 60 FPS display, images (BMP, JPEG, PNG, GIF), sprites and tilemaps, flat-shaded 3D (`gfx3d`), MJPEG video, and `picocalc.chargrid`, a character-cell grid with text, shapes, palettes and a small shader language (plus `picocalc.noise`).
- **Audio.** WAV, MP3, QOA and tracker modules (MOD, XM, S3M), tones, and raw sample streaming.
- **Input.** The built-in keyboard, a rebindable gamepad layer, and Bluetooth gamepads (Classic HID), paired from Settings.
- **Networking.** WiFi, an HTTP/HTTPS client, TCP sockets, crypto primitives, and an SSH client app.
- **System.** A launcher and system menu, an app store, signed over-the-air firmware updates, crash logging with a watchdog, and per-app config and save data.
- **Developer tools.** A PC simulator with a scripted test channel, an E2E suite that runs on the simulator or a real device, host unit tests and fuzzers, a USB serial dev console (with a PC-sampling profiler), and an MCP server (`tools/picodeck_mcp.py`) for driving a device.
- **Boot animation.** Core 1 plays a boot animation from first light while Core 0 brings up the system, so boot work costs nothing until it outlasts the 2.8 s animation.

---

## Architecture

```
Flash & Memory
├── OS kernel + drivers (Double-buffered 60 FPS tear-free display via 100MHz SPI)
├── Lua 5.4 runtime (Resident 6MB heap securely mapped into PSRAM)
└── Launcher (app menu)

SD Card (hot-swappable)
└── /apps/
    ├── hello/
    │   ├── app.json      ← name, description, version
    │   └── main.lua      ← your app
    └── snake/
        ├── app.json
        └── main.lua
└── /data/               ← app save files
└── /system/             ← OS config (WiFi credentials, brightness, etc.)
```

The `picocalc` Lua global is your entire interface to the hardware:

```lua
local pc = picocalc

-- Display
pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 20, "Hello!", pc.display.WHITE, pc.display.BLACK)
pc.display.fillRect(50, 50, 100, 100, pc.display.rgb(255, 0, 0))
pc.display.flush()

-- Input
pc.input.update()                -- poll keyboard (call once per frame)
local btns = pc.input.getButtonsPressed()
if btns & pc.input.BTN_ENTER ~= 0 then ... end
local ch = pc.input.getChar()   -- typed character or nil

-- UI (standard header/footer bars)
pc.ui.drawHeader("My App")
pc.ui.drawFooter("Press Esc to exit", "Bat: 85%")

-- Graphics (image loading)
local img = pc.graphics.image.load(APP_DIR .. "/sprite.bmp")
img:draw(100, 100)

-- System
pc.sys.sleep(500)                -- wait half a second (flush() already paces a frame)
pc.sys.log("debug message")
local bat = pc.sys.getBattery()  -- 0-100, -1 before the first reading

-- Filesystem (SD card)
local data = pc.fs.readFile(pc.fs.appPath("save.json"))  -- /data/<app id>/save.json
```

---

## Memory Architecture

The RP2350 has a small amount of on-chip SRAM supplemented by two independent 8MB PSRAM banks accessed over different buses (one on the PicoCalc mainboard, slower with no XIP, and the other on the Pico Plus 2 W). Understanding which memory is used for what is critical: mixing allocators or DMA sources causes hard-to-debug crashes.

| Memory | Holds | Limits |
|---|---|---|
| SRAM (~520 KB) | The two 200 KB framebuffers and other static data | The heap is effectively nil (about 2 KB after the SDK's share): scratch buffers go through `umm_malloc` |
| SCRATCH | The 4 KB main stack: boot, the launcher, every interrupt | |
| QMI PSRAM (8 MB, on the Pico Plus 2 W) | Lua heap, native apps' image, data and stack (`umm_malloc`, 6 MB less the 128 KB Core 1 pool) | Cached at `0x11000000`, uncached at `0x15000000` |
| PIO PSRAM (8 MB, on the mainboard) | The OS's MP3 ring below `0x48000`; apps get the rest through `pio_psram_read`/`write` | No XIP. Non-fatal if absent |

Root [`CLAUDE.md`](CLAUDE.md) (Memory Map) has the exact addresses and sizes, kept current.

### Key rules

- **Never mix allocators.** `malloc()` pairs with `free()`, `umm_malloc()` with `umm_free()`. Crossing them corrupts both heaps.
- **DMA reads SRAM only.** Framebuffers are in SRAM so DMA flush is on the AHB bus, completely independent of PSRAM access. This is why `display_flush()` is non-blocking even while CPU fetches code from PSRAM.
- **ELF code uses XIP cache.** Native app code is written through the uncached alias (`0x15xxxxxx`), then executed from the cached alias (`0x11xxxxxx`). The 16KB XIP cache eliminates most QMI bus traffic.
- **PIO PSRAM is a separate bus.** The mainboard's second PSRAM bank uses PIO1 SPI on its own pins, so there is no contention with QMI PSRAM, flash, or the display's PIO0 SPI.

---

## Hardware Pin Reference
All pins are defined in `src/hardware.h`. Verify against [clockwork_Mainboard_V2.0_Schematic.pdf](https://github.com/clockworkpi/PicoCalc/blob/master/clockwork_Mainboard_V2.0_Schematic.pdf).

| Peripheral | Interface | Pins |
|---|---|---|
| ST7365P LCD | PIO SPI (pio0) | MOSI=11, SCK=10, CS=13, DC=14, RST=15 |
| SD Card | SPI0 | MOSI=19, SCK=18, MISO=16, CS=17 |
| Keyboard (STM32) | I2C1 | SDA=6, SCL=7, addr=0x1F |
| Audio L/R | PWM | GP26 (L), GP27 (R) |

**Note:** Backlight is controlled by the STM32 keyboard MCU over I2C, not a direct GPIO (`kbd_set_backlight()` in the firmware).

**Note:** The LCD uses a dedicated PIO SPI master, freeing up hardware SPI1 entirely. WiFi (CYW43 on the Pico Plus 2 W) uses its own internal bus.

---

## Build Setup

Only needed to build the firmware or simulator yourself: released firmware and a Linux simulator are on the [download page](https://picodeck.net/download/). To write apps you need neither a toolchain nor this repository's build; see [Getting started on a device](https://picodeck.net/docs/getting-started-on-a-device/).

**Quick Start (recommended):**

```bash
# Clone with the submodules (third_party/ is empty without them;
# in an existing clone: git submodule update --init --recursive)
git clone --recurse-submodules https://github.com/PicoDeck/picodeck.git
cd picodeck

# Install the ARM toolchain
sudo dnf install cmake curl unzip arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib  # Fedora
# sudo apt install cmake curl unzip gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib  # Debian/Ubuntu

# Get the Pico SDK (2.2.0 is what release builds use)
git clone --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
git -C ~/pico-sdk submodule update --init
export PICO_SDK_PATH=~/pico-sdk

# Fetch Lua and FatFS, then build
make setup
make build
```

Output: `build/picodeck.uf2`. [Getting started on a device](https://picodeck.net/docs/getting-started-on-a-device/) shows how to flash it and prepare the card. Lua and FatFS are not in the repository: `make setup` downloads them, and `make build` does too if you skip it. A source archive from GitHub has no submodules and will not build: clone with git.

**Available Make targets** (`make help` lists them all):
- `make setup` — Verify the environment, download Lua and FatFS
- `make build` — Compile firmware
- `make clean` / `make rebuild` — Remove the build directory / clean and rebuild
- `make flash` — Show flashing instructions (auto-detects the BOOTSEL drive on Linux)
- `make flash-ota` — Build and push the firmware to a USB-connected device over the air
- `make simulator` / `make simulator-run` — Build / run the PC simulator (also `simulator-asan`, `simulator-tsan`, `simulator-net`)
- `make test-lua` — Test all Lua apps for syntax errors before deployment
- `make test-unit` — Host unit tests
- `make fuzz` — Run the libFuzzer targets (clang)

---

## Testing Lua Apps

Before copying apps to the SD Card, it's recommended to validate them first:

```bash
make test-lua
```

The test tool (`tools/test_lua_apps.lua`) checks:
- **Syntax correctness** — missing `end`, unmatched quotes, invalid operators
- **API compatibility** — validates against mock `picocalc` API
- **All apps** in `apps/` directory

Example output:
```
=== PicoDeck Lua App Syntax Checker ===

[✓] apps/hello/main.lua
[✓] apps/snake/main.lua
[✗] apps/editor/main.lua
  apps/editor/main.lua:42: ')' expected near 'end'

=== Summary ===
Passed: 2
Failed: 1
```

Catches common errors before deployment:
- Missing button constants (`BTN_CTRL`, `BTN_BACKSPACE`)
- Unclosed functions, loops, or conditionals
- String/syntax errors

---

### Simulator and test suites

The PC simulator runs Lua and native apps in an SDL2 window, with no device needed:

```bash
make simulator-run                       # needs SDL2, SDL2_image and libcurl (see simulator/README.md)
make test-unit                           # host unit tests
pip install -r tests/e2e/requirements.txt
SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto   # E2E suite against the simulator
```

The E2E suite can also run against a real device (`--target hw:<serial port>`). See [`simulator/README.md`](simulator/README.md), [`tests/e2e/README.md`](tests/e2e/README.md) and [Simulator and Testing](https://picodeck.net/docs/simulator-and-testing/).

---

### Without Make

```bash
make setup    # still the way to fetch Lua and FatFS
mkdir -p build && cd build
cmake .. -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
make -j4
```

This produces `build/picodeck.uf2`.

---

## Prepare your SD card

Format the SD card as FAT32 and copy this repository's `apps/` and `system/` folders to its root. Flashing puts nothing on the card, and a blank card shows an empty launcher, with no App Store: the store is an app in `apps/`. The libraries in `system/lib/` are what `picocalc.sys.loadlib` loads; nothing else installs them.

```
SD:/
├── apps/
│   ├── hello/
│   │   ├── app.json
│   │   └── main.lua
│   └── store/
│       ├── app.json
│       └── main.lua
├── system/
│   └── lib/    (download.lua, panels.lua, widgets.lua, fonts/)
└── data/       (created automatically)
```

[Getting started on a device](https://picodeck.net/docs/getting-started-on-a-device/) has the rest: which board, flashing, USB Disk Mode.

### Upgrading from v0.2.0 or earlier

Flashing the firmware does not update the SD card. Follow "Upgrading a device" in the [v0.3.0 release notes](https://github.com/PicoDeck/picodeck/releases/tag/v0.3.0): replace `SD:/apps/` with this release's `apps/`, move each first-party app's `SD:/data/<old id>` folder to its new `net.picodeck.<app>` id, and rebuild the native apps (`make -C apps/c64`, `make -C apps/gbc`, and C-Dogs from `PicoDeck/cdogs`), because their data path is compiled in.

---

## Writing Apps

Every app is a directory in `/apps/` containing at minimum (the [`app.json` reference](https://picodeck.net/docs/app-json-reference/) has every field):

**`app.json`**
```json
{
    "id": "com.example.myapp",
    "name": "My App",
    "description": "What it does",
    "version": "1.0"
}
```

**`main.lua`**
```lua
local pc = picocalc

-- Your app runs in a plain while loop
-- Return (or fall off the end) to go back to the launcher

while true do
    pc.perf.beginFrame()
    pc.input.update()  -- poll keyboard (call once per frame)

    local pressed = pc.input.getButtonsPressed()
    if pressed & pc.input.BTN_ESC ~= 0 then return end

    pc.display.clear(pc.display.BLACK)
    pc.ui.drawHeader("My App!")
    pc.display.drawText(10, 150, "Hello!", pc.display.WHITE, pc.display.BLACK)
    pc.ui.drawFooter("Press Esc to exit")
    pc.perf.drawFPS()
    pc.display.flush()

    pc.perf.endFrame()
end
```

The [Lua SDK Reference](https://picodeck.net/docs/lua-sdk-reference/) lists every API.

---

## Project Structure

```
picodeck/
├── CMakeLists.txt, Makefile    ← firmware build and the top-level targets
├── pico_sdk_import.cmake       ← finds the Pico SDK (tracked; PICO_SDK_PATH points at the SDK)
├── README.md, CLAUDE.md        ← CLAUDE.md is the contributor guide (nested ones sit beside the code)
├── TODO.md                     ← roadmap and open API gaps
├── docs/                       ← app-facing SDK reference, published at picodeck.net/docs
├── docs_coverage.md            ← which Lua functions the docs cover (scripts/list_lua_api)
├── src/
│   ├── hardware.h              ← ALL pin definitions here
│   ├── main.c                  ← boot, init, calls launcher_run()
│   ├── dev_commands.c, dev_ops.c ← USB serial dev console
│   ├── os/                     ← launcher, Lua bridge, app runners, config, OTA, splash, chargrid, UI
│   ├── drivers/                ← display, keyboard, SD card, WiFi/HTTP/TCP, audio, video, PSRAM, Bluetooth
│   ├── usb/                    ← USB mass-storage mode
│   └── fonts/
├── apps/                       ← first-party apps (Lua and native), each with an app.json
├── sdk/                        ← app-author kit (sdk/README.md): Lua stub for editors, native app template and ABI header, fonts
├── system/                     ← files for SD:/system/ (shared Lua libraries such as panels and widgets, fonts)
├── simulator/                  ← PC simulator (SDL2 + Unicorn); see simulator/README.md
├── tests/
│   ├── unit/                   ← host unit tests (make test-unit)
│   ├── e2e/                    ← pytest suite for the simulator and real hardware
│   └── fuzz/                   ← libFuzzer targets (make fuzz)
├── tools/                      ← build, asset and device tooling (OTA flash, profiler, MCP server)
├── scripts/                    ← CI helpers and the Lua API coverage generator
└── third_party/                ← dependencies: vendored, submodules, and Lua and most of FatFS (make setup downloads them)
    ├── lua-5.4/, fatfs/, umm_malloc/, rp2040-psram/, mongoose/, wolfssl/, miniz/
    ├── tgx/, JPEGDEC/, PNGdec/, AnimatedGIF/, stb/        (graphics, image decoding)
    └── libmad/, BackgroundAudio/, pocketmod/, qoa/        (audio)
```
