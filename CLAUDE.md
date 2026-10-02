# CLAUDE.md

## Project Overview

PicoDeck is a bare-metal embedded OS for the [ClockworkPi PicoCalc v2.0](https://www.clockworkpi.com/) handheld device. It runs on a Pimoroni Pico Plus 2 W (RP2350, 8MB QMI PSRAM + 8MB PIO PSRAM, WiFi). A resident C firmware lives in flash; apps are Lua scripts or native ELF binaries on an SD card; all hardware is exposed via a `picocalc.*` Lua API and a C `PicoCalcAPI` struct.

**Where detail lives.** Subsystem notes are in nested `CLAUDE.md` files (loaded when you read files in that directory). Read the one for an area before changing it:
- `src/drivers/CLAUDE.md` — display, WiFi/HTTP/TCP, TLS and randomness, audio, video, PIO PSRAM, keyboard
- `src/os/CLAUDE.md` — app runners and exit teardown, Lua bridge internals (object types, number rules, count hook, sticky exit), sandbox, file handles, config stores, save slots, OTA policy, crash records, Lua heap
- `simulator/CLAUDE.md` — simulator divergences, the test control channel, `--test-mode` and virtual time, allocators, sanitizer and firmware-net builds. Read it before writing an E2E test or trusting a simulator result.
- `docs/` — the app-facing API reference (Markdown in this repo), published at https://picodeck.net/docs/ from each release's `picodeck-docs.zip`. Change it in the same commit as the behaviour it documents; where it disagrees with the nested files, the nested files are right and the doc is a bug. `docs/_sidebar.json` is the site's sidebar.
- The app store is `PicoDeck/store` (https://store.picodeck.net); the website is `PicoDeck/website` (https://picodeck.net), rebuilt from the latest release by a Pages deploy hook. The browser demo (picodeck.net/try) is [PicoDeck/web-sim](https://github.com/PicoDeck/web-sim), built from this repo's simulator (`simulator/CLAUDE.md`, "Web build seam") and released on its own, following PicoDeck's releases.

This file keeps only what applies across the tree. Record subsystem behaviour in the nested file (or header comment) nearest the code.

## Build Commands

### Prerequisites

**Fedora/Linux:**
```bash
sudo dnf install cmake gcc-arm-none-eabi-cs gcc-arm-none-eabi-cs-c++ newlib-arm-none-eabi
```

**macOS (one-shot):**
```bash
chmod +x setup.sh && ./setup.sh
```

**Pico SDK (required for all platforms):**
```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk
```

### Build
```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
make -j4
```

Other board values: `pico2` (no PSRAM/WiFi), `pico_w`, `pico`. Note: the board name in `CMakeLists.txt` defaults to `pimoroni_pico_plus2_w_rp2350` (full SDK name — not `pimoroni_pico_plus2_w`).

Output: `build/picodeck.uf2` — drag-and-drop to Pico in BOOTSEL mode.

### Debug
USB serial at 115200 baud. App log calls appear as `[APP] message`. Lua errors display on-screen with stack trace (dismiss with **Esc**).

Stage multi-file apps (hardware or simulator) with the `push_app` MCP tool (`tools/picodeck_mcp.py`): it ships the directory as one ZIP and extracts it on-device with the `unzip <zip> <dest>` dev command (`rm <path>` cleans up), far faster than per-file transfers. Dev commands run on an app stack, so `unzip` is safe at the launcher.

Dev commands while an app runs (`src/dev_commands.c`, `lua_bridge.c`, native `sys_poll`): plain `reboot` and `reboot-flash` are honoured at once and kill the app without teardown (a native app that never calls `sys->poll()` latches them until the launcher); `reboot-ota` is dropped (`reboot-ota ignored`), so `make flash-ota` (`tools/ota_flash.py`) exits the app first; `usb` waits for the launcher. `exit` with no app running replies `Error: exit: no app running` and still closes a modal open at the launcher (system menu, text input).

### Tests
- **E2E (simulator)**: `SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto` (see `tests/e2e/README.md`; config and markers in the repo-level `pytest.ini`). Runs against `build_sim/picodeck_simulator`; `PICODECK_SIM_BINARY` (or `--simulator-path`) selects another build: `make simulator-asan` (`build_sim_asan/`, enables `asan_only` tests), `make simulator-tsan`, `make simulator-net` / `-net-asan` / `-net-tsan` (`build_sim_net/`…, the firmware network stack; enables `firmware_net` tests). Virtual time and `--real-umm` are described in `simulator/CLAUDE.md`. Lua fixture apps report through `tests/e2e/lib/picotest.lua` (staged as `/system/lib/picotest.lua`).
- **E2E (hardware)**: `--target hw:/dev/serial/by-id/<PicoDeck device>` runs the `hardware`/`both` tests on a real PicoCalc through `tests/e2e/hw_target.py` (they skip, allow-listed, on the simulator).
- **Unit**: `make test-unit` (host C tests: `tests/unit/CMakeLists.txt` → `build_unit/`, ctest, ASan+UBSan; covers the pure modules `elf_plan`, `fs_path`, `app_manifest`, `config`/`appconfig` over an in-memory SD fake, `wav`, `qoa`, `audio_ring`, `zip_name`, `text_wrap`, `lua_numfmt` vs glibc, fonts, the keyboard decode, `pad_source`, and `lua_hook_arm` against the real Lua core, which `make test-unit` fetches with `download-lua`), `python3 -m pytest tests/unit`, `make test-lua`.
- **Fuzz**: `make fuzz` runs the libFuzzer targets in `tests/fuzz/` (clang; `FUZZ_TARGETS=`, `FUZZ_SECONDS=`). CI runs them nightly (`.github/workflows/fuzz.yml`) and the unit job on every push (`unit.yml`). There is no linter.

## Architecture

### Boot Sequence (`src/main.c`)
```
main()
  set_sys_clock_khz(200000)     // 200 MHz overclock
  Wire g_api struct (function pointers)
  display_init()                // PIO0 SPI, double-buffered SRAM
  pio_psram_init()              // Mainboard 8MB PIO PSRAM (non-fatal if absent)
  sound_init() → audio_init()
  kbd_init()
  sdcard_init()
  lua_psram_alloc_init()        // umm_malloc heap on QMI PSRAM
  config_load()                 // /system/config.json
  boot_crypto_init()            // on the OS stack: Core 0 CTR_DRBG seed + CA bundle parse
  wifi_init() → http_init() → tcp_init()
  multicore_launch_core1()      // Core 1: wifi_poll, http, mp3, fileplayer
  system_menu_init()
  launcher_run()                // Never returns
```

### Dual-Core Model
- **Core 0**: runs the launcher, Lua VM and native apps. Owns the display, keyboard and SD card.
- **Core 1**: runs the network stack (Mongoose/CYW43) and audio decode (MP3, fileplayer, MOD) on a 1 ms tick. Core 0 talks to it through an IPC ring (`wifi_req_push()`). **Core 0 must never call `mg_*` functions directly.** Core 1 only try-locks shared player/SD state and skips the tick when busy.

### Central API (`src/os/os.h`)
`PicoCalcAPI g_api` is a function pointer table wired in `main.c`. Sub-tables, native name → Lua name:
- `display`, `input`, `fs`, `sys`, `wifi`, `audio`, `tcp`, `ui` → `picocalc.<same>`. Lua input also has `pollEvent()` (ordered `{type="down"|"up"|"char", key, char, mods, button, repeat}` events, nil when empty) and `isKeyDown(k)`; Lua TCP sockets are `picocalc.tcp.new(host, port, tls)` objects.
- `http` → `picocalc.network.http` (OO connections); `soundplayer` → `picocalc.sound`; `graphics` → `picocalc.graphics.image`; `video` → `picocalc.video`; `modplayer` → `picocalc.modplayer`; `zip` → `picocalc.zip`; `crypto` → `picocalc.crypto` (SHA-256/SHA-1/HMAC/AES-CTR/ECDH).
- `appconfig` → `picocalc.config` **and** `picocalc.appconfig` (same store, two names).
- `gamepad` → `picocalc.gamepad`: `PAD_*` buttons aliased to keys (primary + alternate slot; the keys still report through `input`), resolved in the shared keyboard decode (`src/drivers/CLAUDE.md`), ORed with the pad sources (`src/drivers/pad_source.h`: the `pad` dev command, the simulator's game controller, later a BT/USB pad); key names and the bindings files (`/system/gamepad.json`, per app `/data/<id>/gamepad.json`, loaded at every launch) in `src/os/gamepad_map.c` (`src/os/CLAUDE.md`).
- `picocalc.sysconfig` is Lua-only and needs the `"sysconfig"` requirement; there is no `g_api.config`.
- `g_api.version`: 1 = Phase 1, 2 = Phase 2, 3 = `fs->browse`, 4 = clip rect + mode-7 plane, 5 = zip read-in-place handles, 6 = fonts (setFont/getFont/getFontWidth/getFontHeight/textWidth/loadFont/unloadFont/drawTextTransparent), 7 = video time seek/position, progress OSD, `hasEnded`, 8 = TLS verification: `http->setInsecure`, `tcp->connectEx` (`PCTCP_TLS`/`PCTCP_TLS_INSECURE`), 9 = `gamepad` (the first table after `version`: native apps check `version >= 9` before reading it).

> ⚠️ **Config naming**: in Lua, `picocalc.config` (alias `picocalc.appconfig`) is the **per-app** store (`/data/<APP_ID>/config.json`); `picocalc.sysconfig` is the **system-wide** store (`/system/config.json`).

### App Lifecycle (`src/os/launcher.c`)
1. Scans `/apps/` for dirs containing `main.lua` or `main.elf` (native wins if both), reads each `app.json`, shows a scrollable menu with a battery % header.
2. Installs the app's identity (`src/os/app_identity.c`: id, dirs, requirements). Every enforcement check reads it, never the `APP_*` Lua globals.
3. Runs the app through the `AppRunner` vtable (`src/os/app_runner.h`) on its own `PSPLIM`-guarded PSP stack (`app_stack_run()`; IRQs and the launcher stay on the MSP): **Lua** (`lua_runner.c`, 64 KB VM stack) or **native** (`native_loader.c`, ELF32 PIE relocated to PSRAM, validated by the pure `elf_plan.c`).
4. Tears down whatever the app left open: files, handles, players, connections, fonts, menu items, zip handles (full lists in `src/os/CLAUDE.md`).

### Lua Bridge (`src/os/lua_bridge*.c`)
- One file per `picocalc.*` module, coordinated by `lua_bridge.c`. Functions are `static int l_<module>_<fn>(lua_State *L)` wrappers in `luaL_Reg` tables passed to `register_subtable()`; integer constants (button codes, colour names) are pushed with `lua_pushinteger` / `lua_setfield`.
- Every userdata type registers with `lb_register_type()` (methods in a separate `__index` table, locked metatable), and each `__gc` leaves its object dead so a resurrected object is rejected. Check userdata with `luaL_checkudata`/`luaL_testudata`, never `lua_touserdata`.
- Lua 5.4.7 with a restricted stdlib: `base`, `table`, `string`, `math`, `coroutine`, `utf8`. Blocked: `io`, `os`, `package`, `debug`; `dofile`/`loadfile` removed. `load` is text-only (bytecode rejected), as are app `main.lua` and `sys.loadlib`. Apps load their own modules with the sandboxed global `require` (`lua_bridge_require.c`).
- Numbers: `lua_Integer` is 32-bit and `lua_Number` is `float` (integers exact only to 2^24). Bridge quantity arguments (coordinates, sizes, durations, volumes) round floats to nearest; identifiers (handles, enums, colours, masks, byte counts, ports) require exact integers. Compile-time config lives in `cmake/picodeck_lua.cmake`.

### Drivers: rules that apply everywhere
- **Framebuffer pixels are byte-swapped RGB565** (panel order): every primitive swaps on write, every reader swaps back, and native apps writing via `getBackBuffer()` write swapped pixels.
- Every display primitive clips to the clip rect **once**, before any pixel loop. The simulator has its own display implementation (`simulator/stubs/driver_stubs.c`): a primitive change lands in both.
- Every HTTPS / `tls://` connection verifies the server certificate and waits for SNTP to set the clock; `setInsecure(true)` (Lua) or `PCTCP_TLS_INSECURE` (native) opts one connection out, for self-signed dev servers only.
- Volumes are 0-100 everywhere; larger values clamp.
- The default alarm pool: SDK 2.2.0 (which release builds pin) loses a slot whenever its two earliest alarms are both cancelled before its interrupt handler runs, and a full pool breaks `stdio_usb`, the keyboard bus, the CYW43 driver and every other user (issue #58). The build compiles a fixed copy of pico_time's `time.c` (`cmake/picodeck_pico_time.cmake`: the configure fails on an SDK before 2.3.1 it cannot patch; drop it once the SDK is past 2.3.0). A driver that needs a timeout per step still arms its own hardware alarm rather than cancelling and re-adding pool alarms (`kbd_i2c.c`). TIMER0's alarms: 0 the keyboard engine, 2 Core 1's audio pool, 3 the default pool, so only alarm 1 is left (`hardware_alarm_claim_unused(true)` panics once it is taken); TIMER1's four alarms are unused.

### System Menu (`src/os/system_menu.c`)
- Triggered by the Sym key; detected via `kbd_consume_menu_press()` in the Lua count hook.
- Overlays the current framebuffer (darkened with `display_darken()`) and gives the app both framebuffers back when it closes; app item callbacks run after that (`src/os/CLAUDE.md`).
- Apps and OS register items with `system_menu_add_item()` / `picocalc.sys.addMenuItem()`.

### Memory Map
- **SRAM heap**: ~2.6 KB (`__end__`=0x2007f580 to `__HeapLimit`=0x20080000; the 400 KB double framebuffer is BSS), so effectively none: scratch buffers go through `umm_malloc`.
- **QMI PSRAM (8 MB)**: Lua heap via `umm_malloc` at 0x11200000 (cached alias), 6 MB minus the 128 KB Core 1 pool; ELF app data/BSS. Every umm allocation costs at least one 200-byte block (small Lua objects share slabs; see `src/os/CLAUDE.md`).
- **PIO PSRAM (8 MB)**: the OS owns everything below `0x48000` (MP3 PCM ring); apps get `0x48000`+, range-checked for Lua and native. Accessed via `pio_psram_read`/`pio_psram_write` and `g_api.psram`.
- **Main stack (MSP)**: 4 KB in SCRATCH (`__StackBottom`=0x20081000, `__StackTop`=0x20082000), `MSPLIM`-guarded. Boot, the launcher (menus, USB MSC) and every IRQ run here; dev commands and screenshot save move to a short-lived 32 KB PSRAM stack. Measured peak ~2.2-2.5 KB; the `stack` dev command prints it and the running app's stack peak.
- **Lua VM stack**: 64 KB `umm_malloc` per launch, on the PSP. PSRAM because no SRAM region that size exists; costs ~10-50% on C-call-heavy Lua code (pure Lua loops unchanged).
- **Native app stack**: 16 KB SRAM `malloc` if available (in practice never), else 64 KB `umm_malloc`; on the PSP.

## Coding Conventions

- Functions: `snake_case`
- Types: `_t` suffix (e.g., `picocalc_display_t`)
- Static module vars: `s_` prefix (e.g., `s_framebuffers`)
- Globals: `g_` prefix (e.g., `g_api`)
- Macros: `UPPER_CASE`
- `umm_malloc` pairs with `umm_free`, `malloc` with `free`; the two heaps never mix. PSRAM allocations go through `umm_malloc` (Lua heap) or the PIO PSRAM APIs (audio buffers).
- Pins are defined only in `src/hardware.h`.
- BTN_* constants live in `os.h` (not keyboard.h)

## Extending the OS

### Adding a new hardware driver
1. Create `src/drivers/<name>.h` and `.c`
2. Add pin defines to `hardware.h`
3. Add a `picocalc_<name>_t` struct to `os.h`
4. Wire function pointers in `main.c`
5. Add sources and link libraries in `CMakeLists.txt`

### Changing the native API (`PicoCalcAPI`)
The struct exists three times: `src/os/os.h`, `sdk/native/os.h` and the simulator's trampoline tables (`simulator/unicorn_trampolines.c`). Never insert a field before `version` (it would move); add new fields after it, in the same order in both headers and the trampolines, bump `g_api.version` in `src/main.c` and the trampolines' version write together, then run `python3 tools/check_native_abi.py --cc arm-none-eabi-gcc` (CI's `native-sdk` job) and extend and rebuild the E2E probe (`tests/e2e/native/api_probe.c`, `make -C tests/e2e/native`); `tests/e2e/test_native.py` checks the running layout.

### Exposing a new Lua API
1. Create `src/os/lua_bridge_<module>.c`
2. Write `static int l_<module>_<fn>(lua_State *L)` functions
3. Add to a `luaL_Reg` table
4. Call `register_subtable(L, "name", table)` in `lua_bridge_register()` (in `lua_bridge.c`)
5. Add the new `.c` file to `CMakeLists.txt`

## App Model

Apps are directories at `/apps/<name>/` on a FAT32 SD card containing:
- `app.json` — `{"id":"com.example.app", "name":"...", "description":"...", "version":"...", "author":"...", "requirements":[...]}`
- `main.lua` — Lua app (loop until `return` to exit to launcher)
- `main.elf` — native C/TinyGo app (ELF32 PIE; native takes priority if both present)

The `id` becomes `/data/<id>`: 1-79 characters of `[A-Za-z0-9._-]`, folded to lower case; the launcher refuses an invalid one (rules in `src/os/CLAUDE.md`). Optional `"min_psram_kb": N` makes the launcher refuse to start the app (on-screen reason + error.log entry) unless the PSRAM heap has a single free block of at least N KB — the largest block is the test, not total free bytes.

Pre-set Lua globals: `APP_DIR` (e.g. `"/apps/hello"`), `APP_NAME`, `APP_ID`, `APP_REQUIREMENTS` (booleans, e.g. `APP_REQUIREMENTS.root_filesystem`). They are convenience copies: the sandbox and the per-app stores read the C-side identity, so an app that rewrites them gains nothing.

Native apps receive `PicoCalcAPI *api` as their entry point argument. Call `api->sys->poll()` each frame and check `api->sys->shouldExit()` to handle the system menu exit. Anything the app does not free (files, images, players, terminals, crypto contexts, `qmiAlloc` blocks, HTTP/TCP connections, loaded fonts, menu items, zip handles) is freed when `picodeck_main` returns. Freeing a handle twice is ignored only until its address is reused by a newer handle of the same kind (then the second free releases the newer one), so free each handle once.

### App Requirements
`requirements` in `app.json`:
- `"filesystem"` (default): read `/apps/<dir>` and `/system/lib/`, read + write `/data/<app_id>`; relative paths and `..` are refused. The sandbox (`src/os/fs_path.c`) guards `picocalc.fs` and every loader that takes a path.
- `"root-filesystem"`: full SD card read/write.
- `"http"`: network access; WiFi stays connected after the boot time sync.
- `"audio"`: audio output.
- `"clipboard"`: reserved.
- `"sysconfig"`: registers `picocalc.sysconfig` (`wifi_pass` is write-only).
- `"system-update"`: `sys.applyUpdate`, for the OS updater/store only. Updates are flashed only when signed: dev builds embed the TEST key in `tests/keys/` (`make flash-ota` signs with it); release builds need the `UPDATE_SIGNING_KEY` secret. The full OTA policy is in `src/os/CLAUDE.md`.

### Crash records
- `/system/error.log`: app-level failures (Lua errors and panics, native loader errors, launch refusals), each with a `Heap:` line.
- `/system/crashlog.txt`: HardFault dumps and `UNCLEAN EXIT` entries, written at the next boot.
- `/system/running.txt`: dirty-exit marker; if it survives a boot, the named app never returned to the launcher.
- Boot-loop protection: after `BOOT_MAX_RETRIES` (3) failed boots the boot watchdog stays off and a fault at the limit halts with "Boot failed N times - halted"; power-cycling with the batteries out clears it (a USB replug keeps the scratch registers).

## Lua API Notes

- API reference: `docs/API-*.md` (published at https://picodeck.net/docs/).
- System libraries live in `/system/lib/` and load via `picocalc.sys.loadlib(name)`: `download.lua` provides `download.toFile(url, dest, opts)` (blocking HTTP(S)-to-SD streaming download). `picocalc.crypto.sha256File(path)` returns a file's SHA-256 as lowercase hex.
- Names that differ from other APIs: HTTP connections have no `conn:query`; the TCP socket's error getter is `tcp:error()` (native keeps `g_api.tcp->getError`); `picocalc.video` players have no `destroy()` (the collector frees them). There is no `graphics.cache` table; sprite `setImageDrawMode`, `setIgnoresDrawOffset`, stencils, sprite clip rects, `addDirtyRect` and `markDirty` are accepted but not applied.

## Simulator

`make simulator` builds `build_sim/picodeck_simulator` (SDL2 + Unicorn Engine for native ELF apps). What it does not model, in brief (full list in `simulator/CLAUDE.md`):
- `picocalc.crypto` is absent; `picocalc.video` is stubbed; TLS verification, the SNTP clock gate and the TRNG are firmware-only.
- The display is a separate simulator implementation (`stubs/driver_stubs.c`): keep it in step with the firmware, and confirm colour and timing on hardware. Audio runs the firmware mixer; `sim_audio.c` is only its output.
- Networking is libcurl unless you build `make simulator-net` (the firmware stack on Mongoose, no TLS).
- `umm_*` is a counting allocator: largest-block and fragmentation figures need `--real-umm`.

## Not Yet Implemented

- `picocalc.display.drawBitmap` / raw bitmap blitting (use `picocalc.graphics.image` instead for image loading)
- Native-app HTTP callbacks (`http_fire_c_pending` is a no-op; native apps must poll `http->isComplete()`)
