---
title: "Native App Development"
---

PicoDeck supports running native ARM Cortex-M33 (RP2350) applications in addition to Lua scripts. Native apps are Position-Independent ELF32 (PIE) binaries loaded from the SD card into PSRAM at runtime.


## Choosing Lua vs native

Lua and native apps reach the same OS, through two different surfaces: the `picocalc.*` modules and the `PicoCalcAPI` table.

- **Lua** has the full game framework: sprites, spritesheets, tilemaps, camera, scene manager, animation, particles, save files. New games should start here.
- **Native (C)** gets the raw primitives: display (incl. clip rect, mode-7 `drawPlane`, raycasting columns), images, audio, input, fs. It suits ports and emulators (Doom, C-Dogs, GBC) and CPU-bound renderers. There is no native sprite/tilemap engine — replicate what you need in app code, or drive the game logic in Lua.

### What only Lua has

Most modules have the same names on both sides, but the native table is not a copy of the Lua API. The game framework and the libraries are built on the Lua VM, so they have no C form; the other rows are calls the native table has not been given.

| In Lua | In a native app |
|--------|-----------------|
| Sprites, spritesheets, tilemaps, animation and particles (`picocalc.graphics`); scenes, camera and save slots (`picocalc.game`) | Nothing: write your own |
| `picocalc.chargrid`, `picocalc.noise`, `picocalc.gfx3d`, `picocalc.json`, `picocalc.repl`, and the `/system/lib` libraries | Nothing |
| `picocalc.sysconfig` | No system-config table. The file `/system/config.json` is readable (see [below](#native-apps-have-full-access)) |
| `sys.sleep` | Loop on `sys->getTimeMs()`, calling `sys->poll()`, or pace frames with `perf->setTargetFPS` and `perf->endFrame` |
| `sys.getClock` (wall clock and time zone) | Nothing: `sys->getTimeMs()` and `getTimeUs()` count from boot |
| `input.pollEvent`, `isKeyDown`, `getRawKey`, `getButtonsRepeated` | The button masks and `input->getChar()`: no ordered events, and no key-up for keys without a `BTN_*` bit |
| `conn:getResponseHeaders` | Nothing: a native app sees the status and body, so it cannot follow a redirect's `Location` |
| `sys.addMenuOption` | `sys->addMenuItem` only |
| `sys.getVersion` | `api->version` is the API version, not the firmware's |
| `sys.getMemInfo` | Nothing: `psram->qmiAlloc` returning `NULL` is what you learn |
| `fileplayer:load`, `mp3player:load` return an error | `filePlayerLoad` and `mp3PlayerLoad` return nothing: test `filePlayerIsPlaying` or `mp3PlayerIsPlaying` after playing |

The same name can also differ in detail: `graphics->drawScaled` takes a destination size where Lua's `img:drawScaled` takes a multiplier. [`sdk/native/os.h`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/os.h) is the reference for the native side.


## Application Structure

A native application typically consists of:
- `main.elf`: The compiled binary.
- `app.json`: Metadata for the launcher (name, description, requirements).

Place these files in `/apps/<dir>/` on the SD card.

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

The [`app.json` reference](App-Json-Reference.md) has every field, including `min_psram_kb` and `system_clock_khz`, which big native apps usually want. See [Global Variables and Permissions](Global-Variables-and-Permissions.md) for available requirements (`filesystem`, `root-filesystem`, `http`, `audio`). The filesystem ones do not bind a native app: `api->fs` has no sandbox, so a native app can read and write the whole card, `/system/config.json` included.

### Native apps have full access

A native app runs at the same privilege as the OS. The sandbox and the requirements that restrict an app are enforced by the Lua bridge, and nothing enforces them here: `api->fs`, and every native call that loads an image, sound, font, zip or video, takes any path. So a native app with no requirements at all can read and write the whole SD card, `/system/config.json` and its WiFi password included, along with any memory. Declare what your app does anyway, since the launcher and the store read it (`http` keeps WiFi connected), but tell your users that installing a native app means trusting it with the device.

## Development Environment

### Prerequisites
- **GNU Arm Embedded Toolchain**: `arm-none-eabi-gcc` with newlib, which on Fedora and Ubuntu is a separate package ([`sdk/README.md`](https://github.com/PicoDeck/picodeck/blob/master/sdk/README.md#native-apps) has the install commands)
- **PicoDeck SDK Headers**: `app_abi.h`, `os.h` and `terminal.h` (found in [`sdk/native/`](https://github.com/PicoDeck/picodeck/tree/master/sdk/native))
- **Linker Script**: `linker.ld` (found in the same folder)
- **`stubs.c`** (also there), if your app uses the [C library](#c-library)

The folder also holds a sample program to start from: `main.c`, its `Makefile` and an `app.json`. [`sdk/README.md`](https://github.com/PicoDeck/picodeck/blob/master/sdk/README.md) walks through building it.

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
                    // handles the system menu key (Shift+F5, reported as F10)

        if (s->shouldExit())  // User selected "Exit App" from system menu
            return;

        if (i->getButtonsPressed() & BTN_ESC)
            return;

        d->clear(PC_COLOR_BLACK);
        d->drawText(10, 10, "Hello from C!", PC_COLOR_WHITE, PC_COLOR_BLACK);
        d->flush();
    }
}
```

**Important**: You must call `s->poll()` each frame and check `s->shouldExit()` to properly handle the system menu exit. Returning from `picodeck_main` returns control to the launcher.

A key or gamepad button down when the system menu opens or closes reads as not held, with no edges and no chars, until it is released, so a game that compares `getButtons()` from one frame to the next does not take the key that closed the menu for a press ([API Input](API-Input.md)).

Lua has no `shouldExit()`: the bridge handles the system menu's "Exit App" itself and unwinds the script, so a Lua app just loops (or returns from `main.lua` to exit). Do not carry the call over when porting a native app to Lua; `picocalc.sys.shouldExit` is nil.

### The watchdog

`poll()` is also how the OS knows your app is alive. The hardware watchdog has a 10-second timeout, but Core 1 keeps feeding it for as long as `poll()` has run in the last 60 seconds, so a long load is not cut off at 10 seconds. An app that goes about 60 to 70 seconds without `poll()` is taken for hung and the device resets. `flush()` does not count, so call `poll()` during long loads too. Keys, the system menu and dev commands also wait for your next `poll()`.

### The API Surface

The `PicoCalcAPI` struct contains pointers to all OS subsystems. The full type definitions are in [`sdk/native/os.h`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/os.h).

#### Always available

Every firmware that has `api->version` (v0.0.8 on) has these tables, so they need no version check. A newer function inside a table still does: see [the version list](#what-each-version-added).

| Pointer | Type | Description |
|---------|------|-------------|
| `api->input` | `picocalc_input_t` | Button state, edge detection, character input |
| `api->display` | `picocalc_display_t` | Drawing primitives, framebuffer flush, effects |
| `api->fs` | `picocalc_fs_t` | SD card file I/O (open, read, write, list) |
| `api->sys` | `picocalc_sys_t` | Time, battery, reboot, menu items, logging, `poll()`, `shouldExit()` |
| `api->audio` | `picocalc_audio_t` | Tone generation, PCM streaming |
| `api->wifi` | `picocalc_wifi_t` | WiFi connect/disconnect/status |
| `api->tcp` | `picocalc_tcp_t` | Raw TCP/TLS sockets (non-blocking) |
| `api->ui` | `picocalc_ui_t` | Modal dialogs: text input, confirmation |
| `api->psram` | `picocalc_psram_t` | PIO PSRAM and QMI PSRAM allocation |
| `api->perf` | `picocalc_perf_t` | FPS counting, frame timing |
| `api->terminal` | `picocalc_terminal_t` | Terminal emulator widget |
| `api->http` | `picocalc_http_t` | HTTP/HTTPS client (pool of 8 connections) |
| `api->soundplayer` | `picocalc_soundplayer_t` | Sample playback, file player, MP3 player |
| `api->appconfig` | `picocalc_appconfig_t` | Per-app key/value config (`/data/<APP_ID>/config.json`) |
| `api->crypto` | `picocalc_crypto_t` | SHA-256, SHA-1, HMAC, AES-CTR, ECDH, signature verification |
| `api->graphics` | `picocalc_graphics_t` | Image loading (BMP/JPEG/PNG/GIF), drawing, scaling |
| `api->video` | `picocalc_video_t` | MJPEG video playback with audio |
| `api->modplayer` | `picocalc_modplayer_t` | MOD tracker music playback |
| `api->zip` | `picocalc_zip_t` | ZIP archive extraction |

#### Version 9 additions (`api->version >= 9`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->gamepad` | `picocalc_gamepad_t` | Logical gamepad (`PAD_*` buttons aliased to keys; see [API Gamepad](API-Gamepad.md)) |

`api->gamepad` sits after `version` in `PicoCalcAPI`, so older firmware does not have the field at all: check `api->version >= 9` before reading it.

#### Version 10 additions (`api->version >= 10`)

`api->gamepad->getAxis` (`AXIS_*`): analog sticks and triggers, at the end of `picocalc_gamepad_t`, so a version 9 table does not have it. See [API Gamepad](API-Gamepad.md).

#### What each version added

The `api->version` field indicates which additions are present:

- `1`, `2` — the tables above. No release ever reported less than 2
- `3` — `api->fs->browse` (modal file-browser overlay for native apps; system UI, it draws in the built-in 6x8 font and the app's font is restored. The same holds for `api->ui->textInput` / `text_input_show`)
- `4` — display clip rect (`setClipRect`/`getClipRect`/`clearClipRect`), mode-7 `drawPlane`, and native parity for `fillHLine`/`fillTriangle`/`setScrollArea`/`setScrollOffset`
- `5` — zip read-in-place handles
- `6` — fonts (`setFont`/`getFont`/`getFontWidth`/`getFontHeight`/`textWidth`/`loadFont`/`unloadFont`/`drawTextTransparent`)
- `7` — video time seek/position, progress OSD, `hasEnded`
- `8` — TLS verification: `http->setInsecure`, `tcp->connectEx` (`PCTCP_TLS`, `PCTCP_TLS_INSECURE`)
- `9` — `api->gamepad` (`getButtons`/`getButtonsPressed`/`getButtonsReleased`/`getLabel`, `PAD_*`)
- `10` — `api->gamepad->getAxis` (`AXIS_*`: analog sticks and triggers)

#### Which firmware has which version

| Firmware | `api->version` |
|----------|----------------|
| Before v0.0.8 | No `version` field: not supported |
| v0.0.8 | 2 |
| v0.0.9, v0.1.0 | 3 |
| v0.2.0, v0.3.0, v0.4.0 | 8 |
| v0.5.0, v0.6.0 | 9 |
| The release after v0.6.0 | 10 |

Versions 4 to 7 were never released on their own: firmware went from 3 to 8.

#### Handling firmware that is too old

`PICODECK_API_VERSION` in `os.h` is the version that copy of the header describes. It says what you can compile against, not what the device runs, so decide at run time from `api->version`. An app built against a newer header runs on older firmware until it touches something that firmware does not have, so test first and say why you are leaving:

```c
#define NEEDS_API 9   // this app reads api->gamepad

void picodeck_main(const PicoCalcAPI *api, const char *app_dir,
                   const char *app_id, const char *app_name) {
    if (api->version < NEEDS_API) {
        api->display->clear(PC_COLOR_BLACK);
        api->display->drawText(10, 10, "This app needs a newer PicoDeck.",
                               PC_COLOR_WHITE, PC_COLOR_BLACK);
        api->display->drawText(10, 26, "Update the firmware. Esc to go back.",
                               PC_COLOR_GRAY, PC_COLOR_BLACK);
        api->display->flush();
        while (!api->sys->shouldExit() &&
               !(api->input->getButtonsPressed() & BTN_ESC))
            api->sys->poll();
        return;
    }
    // ... the app ...
}
```

[`sdk/native/os.h`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/os.h) is the source of truth for the complete type definitions, the exact struct layout, and the latest `version` values.

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
- `-nostartfiles -nodefaultlibs`: Native apps do not use standard C runtime startup (CRT0), and no library is linked unless you add it: see [C library](#c-library).

### Example Makefile
A complete working example can be found in [`sdk/native/Makefile`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/Makefile).

```makefile
CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -fpie -fno-plt -Os -I.
LDFLAGS = -T linker.ld -Wl,--entry=picodeck_main -Wl,-pie \
          -Wl,--no-warn-rwx-segments -nostartfiles -nodefaultlibs

all: main.elf

main.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -o $@
```

### Floating point

The chip has a single-precision FPU, and these flags decide whether your code uses it:

| Flags | What you get |
|-------|--------------|
| None beyond `-mcpu=cortex-m33 -mthumb` | Soft float: float maths runs in libgcc's software routines, so it needs `-lgcc` |
| `-mfloat-abi=softfp -mfpu=fpv5-sp-d16` | Float maths runs on the FPU. Floats still pass in core registers, so calls to the API work as they do with soft float |
| `-mfloat-abi=hard` | Refused at launch (`ELF: hard-float ABI (-mfloat-abi=hard) not supported`). It passes floats in FPU registers, which the API does not read |

### C library

The flags above link nothing, so neither the C library nor the compiler's helper functions are there, and ordinary C can fail with `undefined reference`: `char b[256] = {0}` calls `memset`, a struct copy calls `memcpy`, float maths calls `__aeabi_fmul` and friends, and a 64-bit division calls `__aeabi_uldivmod`. Add what you use, after your sources:

| You use | Add |
|---------|-----|
| `memset`, `memcpy`, `strlen` and other functions that need no OS | `-lc` |
| Soft float, 64-bit division | `-lgcc` |
| `sqrt`, `sin`, `malloc`, `snprintf`, anything else that needs an OS underneath | `stubs.c`, and all of `-lc -lm -lgcc` |

[`sdk/native/stubs.c`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/stubs.c) supplies what newlib asks of an OS. With all of it:

```makefile
LDFLAGS = -T linker.ld -Wl,--entry=picodeck_main -Wl,-pie -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments -nostartfiles -nodefaultlibs -lc -lm -lgcc

main.elf: main.c stubs.c
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections main.c stubs.c $(LDFLAGS) -o $@
```

- **`malloc`** gets its memory from an array in `stubs.c`, 256 KB by default. It is part of your image, so it costs PSRAM whether used or not; choose its size with `-DPICODECK_C_HEAP_SIZE=<bytes>`. `malloc` returns `NULL` when it runs out. This heap is your app's own, separate from `psram->qmiAlloc`: never pass a pointer from one to the other's free.
- **Do not add `-lnosys`.** It does not link (`undefined reference to end`), and supplying `end` would be worse: its `_sbrk` has no limit, so `malloc` would grow past your image over the OS's heap.
- **There are no files or clock underneath.** `printf` prints nowhere, so format with `snprintf` and pass the text to `sys->log`. `fopen` and `time` do not link (`_open`, `_gettimeofday`): use `api->fs` and `sys->getTimeMs`.
- **To leave, return from `picodeck_main`.** `exit`, `abort` and a failed `assert` stop the app with a fault, which leaves a crash record.
- **`--specs=nano.specs`** links the much smaller newlib-nano, whose `printf` family silently prints nothing for `%f`: `"%d|%f|%.2f|%s"` gives `42|||s`. Add `-u _printf_float` to get it back. For a small program using `snprintf` and `malloc` the code was about 31 KB with full newlib, 3 KB with nano and 16 KB with nano and `%f`.

### C++

The headers compile as C++ and declare `picodeck_main` with C linkage, so it keeps its name. Include `app_abi.h`: without it the name is mangled, the linker only warns (`cannot find entry symbol picodeck_main`), and the app starts at the wrong address. Compile with `arm-none-eabi-g++` and turn off what needs a C++ runtime, which the link would otherwise ask for:

```makefile
CXXFLAGS = -mcpu=cortex-m33 -mthumb -fpie -fno-plt -Os -I. \
           -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
```

Classes, virtual calls and templates work. There is no C++ runtime beyond that:

- **Static constructors do not run**, because the loader does not walk `.init_array`. A global object with a constructor stays zeroed until you set it up from `picodeck_main`.
- **`new` and `delete` are not supplied** (`undefined reference to operator new[]`). Define them over `malloc` or `psram->qmiAlloc`, keeping the `api` pointer in a global first:

```cpp
static const PicoCalcAPI *g_app_api;  // set first thing in picodeck_main

void *operator new(size_t n)   { return g_app_api->psram->qmiAlloc(n); }
void *operator new[](size_t n) { return g_app_api->psram->qmiAlloc(n); }
void operator delete(void *p) noexcept   { g_app_api->psram->qmiFree(p); }
void operator delete[](void *p) noexcept { g_app_api->psram->qmiFree(p); }
void operator delete(void *p, size_t) noexcept   { g_app_api->psram->qmiFree(p); }
void operator delete[](void *p, size_t) noexcept { g_app_api->psram->qmiFree(p); }
```

- **No exceptions, RTTI or C++ standard library.** `-nodefaultlibs` leaves out `libstdc++`, and nothing has been tested with it linked in.

## Binary Loading Process

When the PicoDeck launcher starts a native app:
1. Core 1 is paused to prevent PSRAM heap contention during loading.
2. The `main.elf` file is read and the ELF header and program headers are validated. A malformed ELF file is refused with a reason (on screen, in `/system/error.log`).
3. The virtual address range of all `PT_LOAD` segments is computed. It may not span more than 7 MB (`ELF: image too large`).
4. **Split-mode loading**: A code segment (`PF_X`) of at most 16 KB is placed in SRAM for faster execution if `malloc` finds room there, and the other segments go into PSRAM via `umm_malloc`; otherwise everything goes into PSRAM. The SDK's `linker.ld` emits a single segment holding code, data and BSS, so for an app built with it that test is on the whole image. An image that does not fit one free PSRAM block is refused (`ELF: out of PSRAM`).
5. Code written to PSRAM uses the uncached alias (`0x15xxxxxx`) to bypass write-back cache, then XIP cache is invalidated. Execution uses the cached alias (`0x11xxxxxx`) so the 16KB XIP cache serves most instruction fetches.
6. `R_ARM_RELATIVE` relocations are applied with dual bias (code bias for SRAM, data bias for PSRAM).
7. Core 1 is resumed. The app runs on the **Process Stack Pointer (PSP)** on its own 64 KB stack in PSRAM (16 KB SRAM if it were ever available), guarded by `PSPLIM`: an overflow faults (crash record `PSP (native app)`) instead of corrupting memory. Interrupts keep using the main stack (MSP).
8. After the app exits, Core 1 is paused again while resources are freed, then resumed.

The application is expected to return control to the OS by returning from `picodeck_main`.

## Memory

- **Stack**: 64 KB in PSRAM (PSPLIM-guarded, runs on PSP).
- **SRAM heap**: a few KB, so treat it as none. No API call reaches it. Use `api->psram->qmiAlloc()`.
- **PSRAM heap**: about 6 MB in all (6,160,384 bytes), one heap shared by your app's image (code, data and BSS), its stack, its `api->psram->qmiAlloc()`/`api->psram->qmiFree()` blocks, the images, samples and other objects the API allocates for it, and what the OS itself holds. An image of nearly 6 MB leaves almost nothing for the rest. The image and each block need one contiguous free block, and every block costs at least 200 bytes. Set `"min_psram_kb"` in `app.json` to have the launcher refuse to start the app, with a reason on screen, unless the largest free block is at least that large.
- **PIO PSRAM**: 8MB secondary PSRAM available via `api->psram->pioRead()`/`api->psram->pioWrite()` for bulk data. Only addresses `0x48000` and up are yours; `pioRead/pioWrite/pioBulkRead/pioBulkWrite` refuse anything overlapping the OS region (the call does nothing and logs `[NATIVE] psram->… refused`).

**Important**: `malloc` is not an OS service. It exists only if you link the [C library](#c-library), and then it is your app's own heap inside its image. Do not mix `malloc`/`free` with `qmiAlloc`/`qmiFree`: they are separate heaps.

### Resources are freed at exit

Anything the app got through the API and did not free — files, images, samples and players, video/MOD players, terminals, AES/ECDH contexts, `qmiAlloc` blocks, HTTP/TCP connections, fonts, menu items, zip handles — is released when `picodeck_main` returns. Do not rely on double frees being harmless: a stale pointer whose address was reused by a newer handle frees that newer object.

### Per-app data, config, randomness

`/data/<APP_ID>` is not created for a native app, as it is for a Lua app, and `fs->open` in a missing folder returns `NULL`. Call `fs->mkdir` on it first. `appconfig->save` creates it for itself.

`appconfig`: nothing loads it for you. Call `api->appconfig->load(your_app_id)` first; another app's id is refused. On an app's first run, with no file yet, `load` returns false but the store is bound all the same, so `set` and `save` work: do not treat that false as fatal. `crypto->randomBytes` returns `bool` (false = buffer zeroed, no healthy generator). There is no system-config table in the native API.

## Debugging

- Native apps can log via `api->sys->log("message: %d", value)`. Output appears on USB serial at 115200 baud as the line you logged, with no prefix (a Lua app's log lines get `[APP]`).
- If the app crashes (HardFault), the fault handler displays register state, CFSR flags, and stack pointer info on the LCD. It detects whether the crash was in the app (PSP) or OS (MSP).
- A stack overflow raises a STKOF UsageFault (CFSR bit 20, 0x00100000) recorded in /system/crashlog.txt with the stack owner (PSP (native app) / PSP (Lua VM) / PSP (OS command) / MSP).

### In the simulator

Native apps also run in the PC simulator, under a CPU emulator against the same API table, which is the quickest way to iterate on logic and to run them under the E2E harness. See [Simulator and Testing](Simulator-and-Testing.md). It is much slower than the device, so test speed on hardware.

Memory and faults mostly follow the device: a `qmiAlloc` heap of what the image and stack leave of 6,160,384 bytes, and a refusal for an image that could not fit. A wild pointer, a write to low memory, or a call to a bad address stops the app, with an `error` outcome and an entry in `/system/error.log`, and the simulator returns to the launcher where the device would show its fault screen and reboot. It is not exact: images, samples and the OS's own allocations do not come out of the simulated heap, so the device has less free than the simulator shows, and reading address 0 returns zeros where the device returns boot ROM.

**The stack is the exception: running in the simulator does not mean it fits the device.** The device's is 64 KB, and here it can grow far past that with the app still running. The first time one does, the log says `stack passed 64 KB at PC=...`: treat that as a crash you have not had yet. On the device, API calls also run on your stack, so leave room.

These calls differ from the device:

| Call | In the simulator |
|------|------------------|
| `display->effect*` | Do nothing |
| `display->setBrightness` | Records the level, but nothing shows it |
| `display->getBackBuffer` | One fixed pointer: the buffer never swaps, and keeps the last frame |
| `display->flushRows`, `flushRegion` | Present the whole buffer, not just the rows |
| `graphics->pixels` | Returns `NULL` |
| `ui->textInput`, `textInputSimple`, `confirm` | Return false at once, with no dialog |
| `sys->setAudioCallback` | Ignored: the callback never runs |
| `sys->reboot` | Ends the app and returns to the launcher |
| `sys->log` | Adds `[APP] `. No width or precision, no `%f`, 7 arguments at most |
| `psram->pio*` | `pioAvailable` is false, and the transfers do nothing |
| `crypto` | Stubs (they write nothing, or return 0 or `NULL`), except `randomBytes` |
| `video` | Stubs: `newPlayer` returns `NULL` |

## See also

- [Writing Fast Apps](Writing-Fast-Apps.md) — build flags, where your code runs, presenting a frame
- [Lua SDK Reference](Lua-SDK-Reference.md) — Lua API documentation (the native table differs: [What only Lua has](#what-only-lua-has))
- [Global Variables and Permissions](Global-Variables-and-Permissions.md) — App requirements
- [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md) — Fault recovery and reboot behavior
