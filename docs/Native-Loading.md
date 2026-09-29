---
title: "Native App Development"
---

PicoDeck supports running native ARM Cortex-M33 (RP2350) applications in addition to Lua scripts. Native apps are Position-Independent ELF32 (PIE) binaries loaded from the SD card into PSRAM at runtime.


## Choosing Lua vs native

Both runtimes share the same `PicoCalcAPI`, but the convenience layers differ:

- **Lua** has the full game framework: sprites, spritesheets, tilemaps, camera, scene manager, animation, particles, save files. New games should start here.
- **Native (C)** gets the raw primitives: display (incl. clip rect, mode-7 `drawPlane`, raycasting columns), images, audio, input, fs. It suits ports and emulators (Doom, C-Dogs, GBC) and CPU-bound renderers. There is no native sprite/tilemap engine — replicate what you need in app code, or drive the game logic in Lua.


## Application Structure

A native application typically consists of:
- `main.elf`: The compiled binary.
- `app.json`: Metadata for the launcher (name, description, requirements).

Place these files in `/apps/<your_app_name>/` on the SD card.

### app.json

```json
{
  "id": "com.example.myapp",
  "name": "My App",
  "description": "A native PicoDeck app",
  "version": "1.0",
  "author": "Your Name",
  "requirements": ["audio", "http"]
}
```

See [Global Variables and Permissions](Global-Variables-and-Permissions.md) for available requirements (`filesystem`, `root-filesystem`, `http`, `audio`).

## Development Environment

### Prerequisites
- **GNU Arm Embedded Toolchain**: `arm-none-eabi-gcc`
- **PicoDeck SDK Headers**: `app_abi.h` and `os.h` (found in `sdk/native/`)
- **Linker Script**: `linker.ld` (found in `sdk/native/`)

## Creating a Native App

### Entry Point

Your application must define an entry point named `picodeck_main`. The OS passes a pointer to the `PicoCalcAPI` struct, which provides access to all OS services.

```c
#include "app_abi.h"
#include "os.h"

void picodeck_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    const picocalc_display_t *d = api->display;
    const picocalc_input_t   *i = api->input;
    const picocalc_sys_t     *s = api->sys;

    while (true) {
        s->poll();  // REQUIRED: polls keyboard, feeds the watchdog,
                    // handles system menu (Sym key)

        if (s->shouldExit())  // User selected "Exit App" from system menu
            return;

        if (i->getButtonsPressed() & BTN_ESC)
            return;

        d->clear(0x0000);
        d->drawText(10, 10, "Hello from C!", 0xFFFF, 0x0000);
        d->flush();
    }
}
```

**Important**: You must call `s->poll()` each frame and check `s->shouldExit()` to properly handle the system menu exit. Returning from `picodeck_main` returns control to the launcher.

Lua has no `shouldExit()`: the bridge handles the system menu's "Exit App" itself and unwinds the script, so a Lua app just loops (or returns from `main.lua` to exit). Do not carry the call over when porting a native app to Lua; `picocalc.sys.shouldExit` is nil.

### The API Surface

The `PicoCalcAPI` struct contains pointers to all OS subsystems. The full type definitions are in `sdk/native/os.h`.

#### Core subsystems (always available)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->input` | `picocalc_input_t` | Button state, edge detection, character input |
| `api->display` | `picocalc_display_t` | Drawing primitives, framebuffer flush, effects |
| `api->fs` | `picocalc_fs_t` | SD card file I/O (open, read, write, list) |
| `api->sys` | `picocalc_sys_t` | Time, battery, reboot, menu items, logging, `poll()`, `shouldExit()` |
| `api->audio` | `picocalc_audio_t` | Tone generation, PCM streaming |
| `api->wifi` | `picocalc_wifi_t` | WiFi connect/disconnect/status (Pico 2W only) |
| `api->tcp` | `picocalc_tcp_t` | Raw TCP/TLS sockets (non-blocking) |
| `api->ui` | `picocalc_ui_t` | Modal dialogs: text input, confirmation |
| `api->psram` | `picocalc_psram_t` | PIO PSRAM and QMI PSRAM allocation |
| `api->perf` | `picocalc_perf_t` | FPS counting, frame timing |
| `api->terminal` | `picocalc_terminal_t` | Terminal emulator widget |

#### Phase 1 additions (`api->version >= 1`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->http` | `picocalc_http_t` | HTTP/HTTPS client (pool of 8 connections) |
| `api->soundplayer` | `picocalc_soundplayer_t` | Sample playback, file player, MP3 player |
| `api->appconfig` | `picocalc_appconfig_t` | Per-app key/value config (`/data/<APP_ID>/config.json`) |
| `api->crypto` | `picocalc_crypto_t` | SHA-256, SHA-1, HMAC, AES-CTR, ECDH, signature verification |

#### Phase 2 additions (`api->version >= 2`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->graphics` | `picocalc_graphics_t` | Image loading (BMP/JPEG/PNG/GIF), drawing, scaling |
| `api->video` | `picocalc_video_t` | MJPEG video playback with audio |
| `api->modplayer` | `picocalc_modplayer_t` | MOD tracker music playback |
| `api->zip` | `picocalc_zip_t` | ZIP archive extraction |

#### Version 9 additions (`api->version >= 9`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->gamepad` | `picocalc_gamepad_t` | Logical gamepad (`PAD_*` buttons aliased to keys; see [API Gamepad](API-Gamepad.md)) |

`api->gamepad` sits after `version` in `PicoCalcAPI`, so older firmware does not have the field at all: check `api->version >= 9` before reading it.

#### Version detection

The `api->version` field indicates which additions are present:

- `1` — Phase 1 additions
- `2` — Phase 2 additions
- `3` — `api->fs->browse` (modal file-browser overlay for native apps)
- `4` — display clip rect (`setClipRect`/`getClipRect`/`clearClipRect`), mode-7 `drawPlane`, and native parity for `fillHLine`/`fillTriangle`/`setScrollArea`/`setScrollOffset`
- `5` — zip read-in-place handles
- `6` — fonts (`setFont`/`getFont`/`getFontWidth`/`getFontHeight`/`textWidth`/`loadFont`/`unloadFont`/`drawTextTransparent`)
- `7` — video time seek/position, progress OSD, `hasEnded`
- `8` — TLS verification: `http->setInsecure`, `tcp->connectEx` (`PCTCP_TLS`, `PCTCP_TLS_INSECURE`)
- `9` — `api->gamepad` (`getButtons`/`getButtonsPressed`/`getButtonsReleased`/`getLabel`, `PAD_*`)

```c
if (api->version >= 2) {
    // Phase 2 APIs are available
    pcimage_t img = api->graphics->load("/apps/myapp/logo.bmp");
}
```

`sdk/native/os.h` is the source of truth for the complete type definitions, the exact struct layout, and the latest `version` values. The C API maps directly to the `picocalc.*` Lua modules documented in the [Lua SDK Reference](Lua-SDK-Reference.md).

## Compilation

Native apps MUST be compiled as **Position-Independent Executables (PIE)**. This allows the OS to load them anywhere in memory.

### Required CFLAGS
- `-fpie`: Generate position-independent code.
- `-fno-plt`: Avoid Procedure Linkage Table.
- `-mcpu=cortex-m33 -mthumb`: Target the RP2350 processor.

### Required LDFLAGS
- `-T linker.ld`: Use the provided linker script.
- `-Wl,--entry=picodeck_main`: Set the entry point.
- `-Wl,-pie`: Final link as PIE.
- `-Wl,--no-warn-rwx-segments`: Suppress linker warnings about RWX segments.
- `-nostartfiles -nodefaultlibs`: Native apps do not use standard C runtime startup (CRT0).

### Example Makefile
A complete working example can be found in `sdk/native/Makefile`.

```makefile
CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -fpie -fno-plt -Os -I.
LDFLAGS = -T linker.ld -Wl,--entry=picodeck_main -Wl,-pie \
          -Wl,--no-warn-rwx-segments -nostartfiles -nodefaultlibs

all: main.elf

main.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -o $@
```

## Binary Loading Process

When the PicoDeck launcher starts a native app:
1. Core 1 is paused to prevent PSRAM heap contention during loading.
2. The `main.elf` file is read and the ELF header and program headers are validated. A malformed ELF file is refused with a reason (on screen, in `/system/error.log`).
3. The virtual address range of all `PT_LOAD` segments is computed.
4. **Split-mode loading**: If the code segment (`PF_X`) fits in SRAM, it is placed there for faster execution. Data/BSS segments go into PSRAM via `umm_malloc`. If SRAM is insufficient, everything goes into PSRAM.
5. Code written to PSRAM uses the uncached alias (`0x15xxxxxx`) to bypass write-back cache, then XIP cache is invalidated. Execution uses the cached alias (`0x11xxxxxx`) so the 16KB XIP cache serves most instruction fetches.
6. `R_ARM_RELATIVE` relocations are applied with dual bias (code bias for SRAM, data bias for PSRAM).
7. The app runs on the **Process Stack Pointer (PSP)** on its own 64 KB stack in PSRAM (16 KB SRAM if it were ever available), guarded by `PSPLIM`: an overflow faults (crash record `PSP (native app)`) instead of corrupting memory. Interrupts keep using the main stack (MSP).
8. Core 1 is resumed after the app exits and resources are freed.

The application runs in the same privilege level as the OS but is expected to return control to the OS by returning from `picodeck_main`.

## Memory

- **Stack**: 64 KB in PSRAM (PSPLIM-guarded, runs on PSP).
- **SRAM heap**: effectively none (~2.6 KB). Use `api->psram->qmiAlloc()`.
- **PSRAM heap**: 8MB available via `api->psram->qmiAlloc()`/`api->psram->qmiFree()`. Use for large allocations.
- **PIO PSRAM**: 8MB secondary PSRAM available via `api->psram->pioRead()`/`api->psram->pioWrite()` for bulk data. Only addresses `0x48000` and up are yours; `pioRead/pioWrite/pioBulkRead/pioBulkWrite` refuse anything overlapping the OS region (the call does nothing and logs `[NATIVE] psram->… refused`).

**Important**: Do not mix `malloc`/`free` (SRAM) with PSRAM allocation functions. They use separate heaps.

### Resources are freed at exit

Anything the app got through the API and did not free — files, images, samples and players, video/MOD players, terminals, AES/ECDH contexts, `qmiAlloc` blocks, HTTP/TCP connections, fonts, menu items, zip handles — is released when `picodeck_main` returns. Do not rely on double frees being harmless: a stale pointer whose address was reused by a newer handle frees that newer object.

### Per-app config, randomness

`appconfig`: nothing loads it for you. Call `api->appconfig->load(your_app_id)` first; another app's id is refused. `crypto->randomBytes` returns `bool` (false = buffer zeroed, no healthy generator). There is no system-config table in the native API.

## Debugging

- Native apps can log via `api->sys->log("message: %d", value)`. Output appears on USB serial at 115200 baud as `[APP] message`.
- If the app crashes (HardFault), the fault handler displays register state, CFSR flags, and stack pointer info on the LCD. It detects whether the crash was in the app (PSP) or OS (MSP).
- A stack overflow raises a STKOF UsageFault (CFSR bit 20, 0x00100000) recorded in /system/crashlog.txt with the stack owner (PSP (native app) / PSP (Lua VM) / PSP (OS command) / MSP).

## See also

- [Lua SDK Reference](Lua-SDK-Reference.md) — Lua API documentation (C API mirrors these modules)
- [Global Variables and Permissions](Global-Variables-and-Permissions.md) — App requirements
- [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md) — Fault recovery and reboot behavior
