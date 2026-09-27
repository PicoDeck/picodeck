# PicoDeck

**Website:** https://picodeck.net · **Try it in your browser:** https://picodeck.net/try/ · **Docs:** https://picodeck.net/docs/ · **App Store:** https://store.picodeck.net

An app platform for the ClockworkPi PicoCalc, built around a resident Lua runtime. Apps live on the SD card as directories containing `main.lua` and `app.json`. The OS owns all hardware; apps access everything through the `picocalc` Lua API.

> [!IMPORTANT]
> **Target hardware:** [Pimoroni Pico Plus 2 W](https://shop.pimoroni.com/products/pimoroni-pico-plus-2-w) in the [PicoCalc](https://www.clockworkpi.com/picocalc) device.
> PicoDeck relies heavily on the PSRAM being accessible via XIP, other hardware support is untested currently.

📖 **Documentation:** Full SDK reference and guides are available on the [PicoDeck Wiki](https://picodeck.net/docs/).

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
local img = pc.graphics.image.load("/apps/myapp/sprite.bmp")
img:draw(100, 100)

-- System
pc.sys.sleep(16)                 -- sleep ms (~60fps)
pc.sys.log("debug message")
local bat = pc.sys.getBattery()  -- 0-100

-- Filesystem (SD card)
local data = pc.fs.readFile("/data/mygame/save.json")
```

---

## Memory Architecture

The RP2350 has a small amount of on-chip SRAM supplemented by two independent 8MB PSRAM banks accessed over different buses (One on the PicoCalc mainboard, slower with no XIP, and the other on the Pico Plus 2 W). Understanding which memory is used for what is critical — mixing allocators or DMA sources causes hard-to-debug crashes.

```
┌─────────────────────────────────────────────────────────────────────┐
│                         RP2350 SoC                                  │
│                                                                     │
│  ┌───────────────────────────────────────┐                          │
│  │           SRAM  (~520KB)              │                          │
│  │                                       │                          │
│  │  BSS/Data ──────────── ~491KB         │  ◄── Framebuffers live   │
│  │    s_framebuffers[2][320×320]         │      here (400KB)        │
│  │    libmad synth.c (.time_critical)    │                          │
│  │    DMA ISR staging buffers            │                          │
│  │                                       │                          │
│  │  Heap (malloc/free) ── ~29KB          │  ◄── Tiny! Free promptly │
│  │    Temp buffers, FatFS workarea       │                          │
│  │                                       │                          │
│  │  Stack (MSP) ────────── 4KB           │  ◄── SCRATCH memory      │
│  │    OS + ISR context                   │      0x20081000-20082000 │
│  │                                       │                          │
│  │  Native app stack (PSP) ── 8KB        │  ◄── Static SRAM buffer  │
│  │    ELF apps run on Process SP         │      (s_native_stack)    │
│  └───────────────────────────────────────┘                          │
│           │ AHB bus (fast, single-cycle)                            │
│           ▼                                                         │
│  ┌─────────────────┐                                                │
│  │   DMA engine    │  Reads SRAM framebuffers for display flush     │
│  └─────────────────┘  (independent of PSRAM — no bus contention)    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
          │                                    │
          │ QMI bus (XIP cache)                │ PIO1 SPI
          ▼                                    ▼
┌────────────────────────────┐    ┌───────────────────────────┐
│  QMI PSRAM  (on-module)    │    │  PIO PSRAM  (mainboard)   │
│  8MB — Pico Plus 2 W       │    │  8MB — PicoCalc v2.0      │
│                            │    │                           │
│  0x11000000 (cached)       │    │  Accessed via             │
│  0x15000000 (uncached)     │    │  pio_psram_read/write()   │
│                            │    │                           │
│  ┌──────────────────────┐  │    │  ┌────────────────────┐    │
│  │ Lua heap (umm_malloc)│  │    │  │ 0x0000: MP3 PCM    │    │
│  │ Base: 0x11200000     │  │    │  │ ring buffer (32KB) │    │
│  │ ~6MB for Lua VM      │  │    │  ├────────────────────┤    │
│  │                      │  │    │  │ 0x8000: Reserved   │    │
│  │ ⚠ umm_malloc/free   │  │    │  │ (currently unused) │    │
│  │   only! Never use    │  │    │  │                    │    │
│  │   standard malloc.   │  │    │  │                    │    │
│  ├──────────────────────┤  │    │  │                    │    │
│  │ ELF app data/BSS     │  │    │  └────────────────────┘    │
│  │ (umm_malloc'd)       │  │    │                           │
│  ├──────────────────────┤  │    │  Non-fatal if absent —    │
│  │ ELF app code         │  │    │  MP3 degrades             │
│  │ (SRAM preferred,     │  │    │  gracefully               │
│  │  PSRAM fallback)     │  │    │                           │
│  └──────────────────────┘  │    └───────────────────────────┘
│                            │
│  XIP cache (16KB) serves   │
│  most instruction fetches  │
│  from PSRAM — avoids       │
│  hammering QMI on every    │
│  CPU fetch cycle           │
└────────────────────────────┘
```

### Key rules

- **Never mix allocators.** `malloc()`/`free()` → SRAM heap. `umm_malloc()`/`umm_free()` → QMI PSRAM heap. Crossing them corrupts both heaps.
- **DMA reads SRAM only.** Framebuffers are in SRAM so DMA flush is on the AHB bus, completely independent of PSRAM access. This is why `display_flush()` is non-blocking even while CPU fetches code from PSRAM.
- **ELF code uses XIP cache.** Native app code is written through the uncached alias (`0x15xxxxxx`), then executed from the cached alias (`0x11xxxxxx`). The 16KB XIP cache eliminates most QMI bus traffic.
- **PIO PSRAM is a separate bus.** The mainboard's second PSRAM bank uses PIO1 SPI on its own pins — no contention with QMI PSRAM, flash, or the display's PIO0 SPI.

---

## Hardware Pin Reference
All pins are defined in `src/hardware.h`. Verify against [clockwork_Mainboard_V2.0_Schematic.pdf](https://github.com/clockworkpi/PicoCalc/blob/master/clockwork_Mainboard_V2.0_Schematic.pdf).

| Peripheral | Interface | Pins |
|---|---|---|
| ST7365P LCD | PIO SPI (pio0) | MOSI=11, SCK=10, CS=13, DC=14, RST=15 |
| SD Card | SPI0 | MOSI=19, SCK=18, MISO=16, CS=17 |
| Keyboard (STM32) | I2C1 | SDA=6, SCL=7, addr=0x1F |
| Audio L/R | PWM | GP26 (L), GP27 (R) |

**Note:** Backlight is controlled by the STM32 keyboard MCU over I2C, not a direct GPIO. Use `picocalc.display.setBrightness()` in Lua or `kbd_set_backlight()` in C.

**Note:** The LCD uses a dedicated PIO SPI master, freeing up hardware SPI1 entirely. WiFi (CYW43 on Pico 2W) uses its own internal bus.

---

## Build Setup

**Quick Start (recommended):**

```bash
# Install ARM toolchain
sudo dnf install cmake gcc-arm-none-eabi newlib-arm-none-eabi  # Fedora
# sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi  # Debian/Ubuntu

# Get Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk

# Setup dependencies and build
make setup
make build
```

Output: `build/picodeck.uf2` — drag-and-drop to Pico in BOOTSEL mode.

**Available Make targets:**
- `make setup` — Download Lua/FatFS, verify environment
- `make build` — Compile firmware
- `make clean` — Remove build directory
- `make rebuild` — Clean and rebuild
- `make flash` — Show flashing instructions (auto-detects RPI-RP2 on Linux)
- `make test-lua` — Test all Lua apps for syntax errors before deployment
- `make help` — Show all targets

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

**Note:** `make build` automatically runs tests. To skip: `make build -o test-lua`

---

### Manual Setup

<details>
<summary>Click to expand manual build instructions</summary>

### 1. Install prerequisites

```bash
# Fedora
sudo dnf install cmake gcc-arm-none-eabi-cs gcc-arm-none-eabi-cs-c++ newlib-arm-none-eabi

# Or use the Raspberry Pi Pico toolchain installer:
# https://github.com/raspberrypi/pico-setup-windows (Windows)
# https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf (Linux)
```

### 2. Get the Pico SDK

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
echo 'export PICO_SDK_PATH=~/pico-sdk' >> ~/.bashrc
source ~/.bashrc
```

### 3. Copy `pico_sdk_import.cmake`

```bash
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
```

Lua 5.4 and FatFS (including the RP2350 SPI port) are already vendored in `third_party/` — no download needed.

### 4. Build

```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
make -j4
```

This produces `build/picodeck.uf2`. Flash it by holding BOOTSEL on your Pico while plugging in USB, then drag the UF2 to the mounted drive.

Other board values: `pico2` (no PSRAM/WiFi), `pico_w`, `pico`.

</details>

---

## Prepare your SD card

Format the SD card as FAT32. Copy the `apps/` folder to the root:

```
SD:/
├── apps/
│   ├── hello/
│   │   ├── app.json
│   │   └── main.lua
│   └── snake/
│       ├── app.json
│       └── main.lua
├── data/     (created automatically)
└── system/   (created automatically)
```

### Upgrading from v0.2.0 or earlier

Flashing the firmware does not update the SD card. Follow "Upgrading a device" in the [v0.3.0 release notes](https://github.com/PicoDeck/picodeck/releases/tag/v0.3.0): replace `SD:/apps/` with this release's `apps/`, move each first-party app's `SD:/data/<old id>` folder to its new `net.picodeck.<app>` id, and rebuild the native apps (`make -C apps/c64`, `make -C apps/gbc`, and C-Dogs from `PicoDeck/cdogs`), because their data path is compiled in.

---

## Writing Apps

Every app is a directory in `/apps/` containing at minimum:

**`app.json`**
```json
{
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

### Full Lua API reference

Moved to the [Wiki](https://picodeck.net/docs/lua-sdk-reference/).

**Example:**
```lua
while true do
    pc.perf.beginFrame()
    -- ... your rendering code ...
    pc.perf.drawFPS()  -- Easy!
    pc.display.flush()
    pc.perf.endFrame()
    pc.sys.sleep(16)
end
```

---

## Project Structure

```
picocalc-os/
├── CMakeLists.txt
├── pico_sdk_import.cmake      ← copy from $PICO_SDK_PATH/external/
├── README.md
├── SDK.md                     ← full Lua API reference
├── src/
│   ├── hardware.h             ← ALL pin definitions here
│   ├── main.c                 ← boot, init, splash, calls launcher_run()
│   ├── os/
│   │   ├── os.h               ← PicoCalcAPI struct (hardware API table)
│   │   ├── launcher.h/c       ← app discovery + scrollable menu
│   │   ├── lua_bridge.h/c     ← C functions registered as picocalc.* Lua API
│   │   ├── lua_psram_alloc.h/c ← PSRAM-backed Lua heap allocator
│   │   ├── system_menu.h/c    ← system menu overlay (Menu key)
│   │   ├── config.h/c         ← persistent JSON config
│   │   ├── clock.h/c          ← NTP time sync
│   │   ├── ui.h/c             ← shared header/footer drawing
│   │   ├── image_decoders.h/cpp ← JPEG/PNG/GIF decoding
│   │   ├── file_browser.h/c   ← file picker overlay
│   │   ├── screenshot.h/c     ← BMP screenshot capture
│   │   └── text_input.h/c     ← text entry overlay
│   └── drivers/
│       ├── display.h/c        ← ST7365P, PIO SPI, DMA double-buffered flush
│       ├── keyboard.h/c       ← STM32 I2C keyboard + battery
│       ├── sdcard.h/c         ← FatFS wrapper
│       ├── wifi.h/c           ← CYW43 WiFi driver
│       └── http.h/c           ← Mongoose HTTP client
├── apps/
│   ├── hello/                 ← example: drawing + input
│   ├── tetris/                ← example: full game
│   ├── snake/                 ← example: full game
│   ├── wikipedia/             ← example: networking
│   └── image_viewer/          ← example: graphics API
└── third_party/
    ├── lua-5.4/               ← Lua 5.4 source
    ├── fatfs/                 ← Chan FatFS + SPI port
    ├── umm_malloc/            ← Embedded memory allocator for PSRAM Lua heap
    ├── mongoose/              ← Mongoose networking library
    ├── tgx/                   ← TGX graphics library (scaling/rotation)
    ├── JPEGDEC/               ← JPEG decoder
    ├── PNGdec/                ← PNG decoder
    └── AnimatedGIF/           ← GIF decoder
```
