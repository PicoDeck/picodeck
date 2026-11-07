---
title: "Simulator and Testing"
---

The PC simulator runs the firmware's Lua bridge and launcher on your computer, with an SDL2 window in place of the LCD, so you can run and debug apps without a device.

- **Lua apps** run on the same bridge as the device.
- **Native (ELF) apps** run under the Unicorn CPU emulator, against the same `PicoCalcAPI` table as on the device. They are much slower than on the RP2350's real cores, so judge performance on hardware.
- **In the browser**, [picodeck.net/try](https://picodeck.net/try/) runs the same sources compiled to WebAssembly (built by [PicoDeck/web-sim](https://github.com/PicoDeck/web-sim)). It runs Lua apps and native apps compiled into the page, but has no networking and cannot run ELF apps.

The simulator is not the device: read [the differences](#simulator-vs-device) before trusting a result. The first half of this page is for app authors. [Working on PicoDeck itself](#working-on-picodeck-itself) comes after.

## Get the simulator

**Download it (x86-64 Linux).** Each [release](https://github.com/PicoDeck/picodeck/releases/latest) has a `picodeck-simulator-linux`. It needs the SDL2 and libcurl shared libraries, and it comes with no apps and no card, so make a [card folder](#the-sd-card-is-a-folder) first.

```sh
curl -LO https://github.com/PicoDeck/picodeck/releases/latest/download/picodeck-simulator-linux
chmod +x picodeck-simulator-linux
./picodeck-simulator-linux --sd-card mycard
```

**Or build it.** You need CMake, C and C++ compilers, and the SDL2, SDL2_image and libcurl development packages (Fedora: `SDL2-devel SDL2_image-devel libcurl-devel`; Debian/Ubuntu: `libsdl2-dev libsdl2-image-dev libcurl4-openssl-dev`). The Pico SDK and an ARM toolchain are not needed. The first build downloads Lua, mbedTLS and Unicorn. CI builds and tests on Ubuntu; other systems are not tested.

```sh
git clone --recurse-submodules https://github.com/PicoDeck/picodeck.git
cd picodeck
make simulator           # build_sim/picodeck_simulator
make simulator-run       # build it and run it, with the checkout as the card
```

Without `--recurse-submodules`, `make simulator` stops and gives the command that fetches them. A source archive from GitHub has no submodules, so it does not build.

## The SD card is a folder

`--sd-card` names the card's **root**, not the apps folder. Each app is in `<root>/apps/<dir>/`, as on a real card:

```
mycard/
├── apps/
│   └── myapp/
│       ├── app.json
│       └── main.lua
├── data/               created for you: each app's saved data
└── system/
    └── lib/            copy from the repo if you use loadlib
```

```sh
./picodeck-simulator-linux --sd-card mycard
```

One you built is `build_sim/picodeck_simulator`, with the same flags.

The simulator creates `apps/`, `data/` and `system/` in an empty folder, so it starts with a blank card. It stops with a message, and creates nothing, if the path is not a folder, or if the folder holds app folders but no `apps/` (you passed `mycard/apps`).

With no `--sd-card`, a simulator you built uses the source tree it was built from as the card, with the repository's `apps/` and `system/lib/`. A downloaded one has no source tree, so it needs the flag and says so. Files your app writes land in that folder's `data/`: delete it to start clean. An app needing `/system/lib` needs that folder on your card too: copy `system/lib` from the [repository](https://github.com/PicoDeck/picodeck/tree/master/system/lib), or [bundle the library](Publishing-Apps.md#bundle-the-libraries-you-use).

## Launching your app

Pick it in the launcher with the arrow keys and Enter, or start it from the command line. `--launch` takes the app's id, its name or its folder name:

```sh
./picodeck-simulator-linux --sd-card mycard --launch myapp
```

The launcher reads `/apps` at boot and each time an app exits. `main.lua` is read on every launch, so to try an edit, press Esc in your app (if it handles that) or use the system menu's Exit App, then Enter.

An error stops the app with its message on screen, as on the device; press Esc to return. It also goes to `/system/error.log` in the card folder.

`sys.log` prints `[APP] message` on the simulator's standard output, and `print` prints there too.

## Keys

| On your computer | PicoCalc key |
|---|---|
| Arrow keys, Enter, Esc, Tab, Backspace, Delete | The same keys |
| F1 to F5 | F1 to F5 |
| **F10** | The system menu key (Shift+F5, reported as F10, on the device). Shift+F5 does not open it here |
| F12 | Saves a screenshot to `/screenshots/` on the card (Shift+Esc on the device) |
| Letters, digits, punctuation | The same characters, unshifted: Shift makes no capitals or symbols here. `inject_char`, [below](#driving-it-from-a-script), types anything |
| Shift, Ctrl | Shift, Ctrl |
| Home | Sym (`BTN_FN`) |

F6 to F9 and Alt have no key. A game controller plugged into the computer drives `picocalc.gamepad`: D-pad or left stick, A/B/X/Y, shoulders as L/R, Start, Back as Select, Guide to open or close the system menu, and both sticks and the triggers for `getAxis`. It works with hotplug, unless `--no-gamepad` is given.

## Driving it from a script

The simulator listens on a control port: TCP on `127.0.0.1`, port **7878** by default. `--port N` picks another, and `--port 0` picks a free one and prints it as `[Socket] TCP port: N`. It also opens a UNIX socket, `./picodeck_control`, in the current folder (`--unix-socket PATH` or `none` changes that).

The framing is **one JSON-RPC 2.0 message per line**, in both directions. A reply carries your `id`. Lines with no `id` are notifications, such as `app.exited`. This drives an app with nothing but Python's standard library:

```python
import base64, json, socket, time

sock = socket.create_connection(("127.0.0.1", 7878))
lines = sock.makefile("r")
last_id = 0

def call(method, **params):
    global last_id
    last_id += 1
    message = {"jsonrpc": "2.0", "id": last_id, "method": method, "params": params}
    sock.sendall((json.dumps(message) + "\n").encode())
    while True:
        reply = json.loads(lines.readline())
        if reply.get("id") == last_id:
            return reply.get("result", reply.get("error"))

call("launch_app", name="myapp")
time.sleep(1)
print(call("get_running_app"))
open("shot.png", "wb").write(base64.b64decode(call("screenshot")["data"]))
call("inject_button", button="esc", action="click")
time.sleep(1)
print(call("get_last_outcome"))
for line in call("get_log_buffer", tail=20)["lines"]:
    print(line["src"], line["text"])
```

| Method | Does |
|---|---|
| `launch_app {name}` | Starts an app; replies `{queued, busy, launch_id}`. It rescans `/apps` once if the name is not found |
| `exit_app`, `get_running_app`, `get_last_outcome` | Asks the app to exit; what runs; how the last one ended: `result` is `returned`, `error`, `exit_sentinel` (Exit App, `sys.exit`) or `load_failed` |
| `rescan_apps` | Reads `/apps` again, after editing an `app.json` |
| `inject_button {button, action}` | `action` is `click`, `press` or `release`; `button` is `up down left right enter esc menu tab backspace del shift ctrl fn f1`-`f5` |
| `inject_char {char}` | Types one character |
| `screenshot {format}` | The screen, base64 in `data`: `png` (default) or `raw` (RGB565) |
| `get_pixel {x, y, w, h}` | A pixel's `rgb565`, `r`, `g` and `b`, or with `w` and `h` (up to 16) a `pixels` array |
| `display_stats {x, y}` | `present_count` (frames shown) and counts of what is on screen; `x` and `y` add `pixel_at` |
| `display_diff {action}` | `action` `capture` keeps the screen; `compare` replies `{changed_pixels, change_pct, bbox}` against it |
| `get_log_buffer {since_seq, tail}` | `{lines, next_seq, dropped, more}`; each line has `seq`, `t_ms`, `text` and a source `src`: `lua`, `native`, `os` or `err` |
| `subscribe {logs}` | With `logs` true, pushes `log` notifications |
| `read_file {path}` | Replies `{path, size, data}`, with `data` in base64 |
| `write_file {path, data}` | `data` is base64, and the folder must exist. A request line over 256 KB (about 190 KB of file) is dropped with no reply, so copy a bigger file into the card folder yourself |
| `delete_file {path}` | A file, not a folder |
| `list_dir {path}` | Replies `{path, entries}`; each entry has `name`, `is_dir` and `size`. Names starting with a dot are left out |
| `disk_info` | `{free_kb, total_kb}` |
| `dev_command {cmd}` | Runs a [dev command](#dev-commands) |
| `get_heap_info`, `get_audio_state`, `get_mp3_stats`, `get_crash_log` | Read state; none takes a parameter |
| `get_terminal_buffer` | `{cols, rows, cursor_x, cursor_y, lines}` of the newest `picocalc.terminal`, or an error if the app has none |
| `set_wifi_state {status, error_code, error_str}` | `connecting` or `failed` pin what `wifi.getStatus()` reports, with the network still working. `disconnected` or `not_available` take the network away, and `connected` or `online` give it back. `error` fails requests and connects, with `error_str` in the message. Anything else is refused |
| `get_wifi_state` | `{status, ssid, ip}` |
| `set_battery {percent, charging}` | What the battery calls report. A value you leave out goes back to its default, 100 and false |
| `get_backlight` | `{level, dimmed}` |
| `get_input_state` | `{issued_seq, consumed_seq}`: each `inject_*` replies an `input_seq`, and the OS has read that input once `consumed_seq` reaches it |
| `get_button_state`, `get_display_buffer`, `get_sd_slow_mode` | The button masks; the screen as base64 RGB565; what `fs.setSlowMode` last set |
| `play_tone {frequency, duration_ms}`, `stop_audio` | Starts a tone; stops tones and streams |
| `set_sd_busy {ms, after_reads}` | Reports the card busy to audio streaming for `ms`, once `after_reads` more of its reads have gone through |
| `ping`, `clear_log_buffer`, `wait_for_idle` | `{version, uptime_ms}`; empties the log; `{idle}`, whether no app is running, at once |
| `set_time_multiplier {multiplier}`, `step_time {ms}` | Drive the virtual clock (below) |
| `shutdown` | Quits the simulator |

A method ignores a parameter it does not know, so a misspelt one usually shows up as the default being used. An error reply has `code` and `message`: the script above returns it in place of the result, so look for `message`.

**Typing, from a script.** `inject_button enter` is a key press with no character, so it does not finish a `ui.textInput`, which reads characters. Type it instead, and Backspace too, as `"\b"`:

```python
call("inject_char", char="a")
time.sleep(0.2)                # one character per frame, so give the app one
call("inject_char", char="\n")  # Enter's character: ui.textInput returns "a"
```

`ui.confirm` answers to `inject_button enter`, and to `esc`.

**What the log holds.** `get_log_buffer` has `sys.log` lines, not `print` lines, which only reach the simulator's standard output. So log with `picocalc.sys.log` what a script needs to read.

### Testing your own app

Run with `--test-mode` for repeatable runs: error screens return at once, `math.random` and the string hash get a constant seed, and the clock starts at 2026-01-01. Add `--virtual-time` to make the clock the OS and apps read virtual: sleeps cost almost no real time, and `step_time` moves it by hand. `--real-umm` puts the firmware's heap allocator under your app, for real fragmentation and the real heap size.

A repository checkout has more help. [`tests/e2e/picodeck_simulator.py`](https://github.com/PicoDeck/picodeck/blob/master/tests/e2e/picodeck_simulator.py) is a Python client for all of the above. [`tests/e2e/lib/picotest.lua`](https://github.com/PicoDeck/picodeck/blob/master/tests/e2e/lib/picotest.lua) is a small test kit for inside a Lua app: copy it into your app, `require("picotest")`, and each case reports to the log and to a results file.

`make test-lua` checks the syntax of the apps in the repository's `apps/` only. A Lua 5.4 `luac -p main.lua` does the same for yours.

### Coding assistants: MCP

[`tools/picodeck_mcp.py`](https://github.com/PicoDeck/picodeck/blob/master/tools/picodeck_mcp.py) is a [Model Context Protocol](https://modelcontextprotocol.io/) server, the way a coding assistant such as Claude Code calls external tools. It offers the simulator and a USB-connected device as tools: `start_simulator`, `push_app`, `launch_app`, `keypress`, `pad`, `screenshot`, `get_log_buffer` and more. It needs `pip install mcp pyserial`; the repository's `.mcp.json` registers it for tools that read that file.

It picks a running simulator if one answers, and otherwise a connected device, whatever you meant: **with a device plugged in, check `get_status` first.** Pass `sd_card_path` to `start_simulator`, and `push_app` replaces the app's folder on the target, so never push from a folder inside the card it writes to.

## Simulator vs device

Checked by running the simulator, at firmware API version 10. Where a row gives what the device does, that comes from reading the firmware's source unless the page for that API says it has been measured. **Colour, timing and performance are only ever right on the device.**

This is what is known to differ. The Lua bridge is the firmware's own code; the file, network and display layers under it are the simulator's, written to follow the firmware's rules.

### Missing

| API | Simulator | Device |
|---|---|---|
| `picocalc.crypto` | `nil` in Lua. Native: stubs that return 0 or NULL and write nothing, except `randomBytes`, which works | Present |
| `picocalc.video` | `player()` returns `nil, "failed to create video player"`. Native: `newPlayer` returns NULL, the rest do nothing | Decodes MJPEG |
| PIO PSRAM | `pioPsramSize()` is 0, writes return 0, reads return nil. Native: `pioAvailable()` is false and the other `pio*` calls do nothing | 8 MB, apps from `0x48000` |
| Bluetooth pads | A scripted radio (`bt sim ...`, below) | Real pads |
| USB Disk Mode | Does nothing | [Works](Getting-started-on-a-device.md#5-copy-your-own-apps) |
| `system_clock_khz` | Accepted, with no effect on speed. Native `sys->getClockKhz()` always says 125000 | Changes the clock, and can [drop WiFi](App-Json-Reference.md#system_clock_khz) |
| Watchdog | None | Resets a stuck app |
| Backlight | `display.setBrightness` and idle dim change a level nothing draws (the `get_backlight` method reads it). Idle dim is off unless `--idle-dim` | The real backlight. Dims after 60 s, and the waking key is swallowed |

### Memory

A Lua app has more memory here than on the device. A native app has the device's heap size, with less competing for it.

| What | Simulator | Device |
|---|---|---|
| Lua heap | Host memory, reported as 8 MB. `psram_largest_block` is roughly the free total and fragmentation is always 0. `--real-umm` gives the firmware's allocator, though objects are about twice their device size on a 64-bit host | About 6 MB, shared with images, and it fragments |
| Native `qmiAlloc` | The device's 6,160,384 bytes and 200-byte blocks, less the image and the stack, and an image that cannot fit is refused the same way. Images, samples and the OS's own allocations come from elsewhere | One heap holds all of those too |
| Native stack | 256 KB, with pages below it mapped on demand, so deep recursion passes. The first write past 64 KB logs `stack passed 64 KB at PC=...` and the app goes on | 64 KB; overflowing it faults |
| Native direct register access | Reads or writes emulated memory (the heap sits at `0x40000000`, the framebuffer at `0xD0000000`) | The peripheral |

A native app's wild access, write through NULL, bad instruction or call through NULL stops it here as on the device, with a report on screen and in `/system/error.log`. A read through NULL does not fault on either. The device then reboots; the simulator returns to the launcher.

### Network

The default build uses your computer's network through libcurl, not the firmware's stack, and is always online. So the `http` requirement, a dropped link and the boot's auto-disconnect never show. What an app may rely on is the same: requests refused while one runs, timeouts, a body cut short failing and large bodies arriving whole. `make simulator-net` builds the firmware's own stack, without TLS.

| Case | Simulator | Device |
|---|---|---|
| Missing `"http"` requirement | Works | WiFi may be off, and the connection fails |
| A reader far behind | The transfer pauses once the ring plus 32 KB is waiting, and nothing is lost | Fails: `receive buffer overflow` (HTTP), `receive overflow` (TCP) |
| WiFi loses a frame | Never happens | The transfer pauses for about a second |
| A TCP server not reading | `conn:write` still takes data, more slowly | `conn:write` returns -1 past 16 KB waiting |
| Certificates and the clock | Your system's, through libcurl; no wait | The bundled CA list; waits for SNTP to set the clock |
| Connection error text | `curl error: ...`, `connect: Connection refused` | `Mongoose error: socket error`, and others |

### Files

Open modes and `rename` follow the device's rules. These do not:

| Case | Simulator | Device |
|---|---|---|
| Path case | Case-sensitive, on a case-sensitive host: `/SYSTEM/LIB/x.lua` is not found | FAT: not case-sensitive |
| `seek` past the end | Neither clamps nor extends the file before a write | Grows the file at once, with undefined data in the gap |
| A file open for writing, opened again | Allowed | Refused. Both allow 16 open files in all |
| `fs.setSlowMode` | Accepted, with no effect on speed | Drops the card to 1 MHz |
| `fs.diskInfo` | The host's disk, capped at 32 GB | The card |
| Directory order | The host's: tmpfs newest first, ext4 hashed | FAT order. With more than 64 apps, which ones list differs |

### Display and graphics

The simulator has its own display code, so it can differ in any primitive. These are known.

| What | Simulator | Device |
|---|---|---|
| Colour | RGB on your monitor | The panel's: check colour on hardware |
| Native `effect*` post-effects | Do nothing (Lua's work) | Work |
| Native `getBackBuffer` pointer | One fixed address, whose buffer never swaps | Changes at every `flush()`; fetch it each frame |
| Native `flushRows`, `flushRegion` | Present the whole buffer | Send just those rows |
| Native `graphics->pixels` | NULL for a valid image | The pixels |

### Input

| What | Simulator | Device |
|---|---|---|
| Menu key | F10 | Shift+F5 |
| Typing with Shift | Gives the unshifted character | Capitals and symbols |
| Native `ui->textInput`, `textInputSimple`, `confirm` | Return false at once, with no dialog | Dialogs |

### System

| What | Simulator | Device |
|---|---|---|
| `sys.reboot()`, native `sys->reboot`, the menu's Reboot | Ends the app and returns to the launcher. The config and app list from boot stay | Reboots |
| `sys.triggerFault()` | The simulator crashes (exit status 139), leaving `running.txt` | HardFault, crash record, reboot |
| Battery | 100%, not charging, unless `set_battery` | Real |
| Wall clock (`sys.getClock()`, the header's clock) | Not set: `synced` stays false and `epoch` 0, and the header shows no clock. `--test-mode` starts it at 2026-01-01 | Set over the network once WiFi is up |
| Tracebacks | Sent to the test log (source `err`) | None |
| Speed | Faster for Lua, far slower for native | Much of the time goes to PSRAM cache misses |
| Native `sys->log` | Adds `[APP]`. No width or precision (`%5d` prints `2`), `%f` prints `<float>` and misaligns what follows, `%lld` misaligns too, and arguments past the seventh print `?` | `vprintf`, with no prefix |
| Native `sys->setAudioCallback` | Ignored | Calls back on Core 1 |

## Dev commands

A running device takes text commands over its USB serial port, at 115200 baud (`/dev/serial/by-id/usb-Raspberry_Pi_PicoDeck_Device_*` on Linux; try `picocom` or `screen`). Type `help` for the list. Each reply line starts with `[DEV]`. The log shares the port.

In the simulator, send the same words through the [`dev_command`](#driving-it-from-a-script) method. Only some exist there, and the rest reply `Unknown command`; the last column gives what to use instead.

| Command | Does | Device | Simulator |
|---|---|---|---|
| `ping` | Checks the link | yes | yes |
| `help` | Lists commands | yes | no |
| `ver`, `status` | Build; app, uptime, WiFi, SD, battery | yes | no (`get_running_app`, `--build-info`) |
| `launch <app>` | Starts an app by id, name or folder | yes | no (`launch_app`) |
| `exit` | Asks the running app to exit | yes | yes |
| `list [all]` | Lists apps; `all` adds hidden ones | yes | yes (to the log) |
| `keypress <key>`, `keydown`, `keyup` | Injects keys | yes | no (`inject_button`, `inject_char`) |
| `pad <buttons> [ms]` | Drives a test gamepad | yes | yes |
| `bt ...` | Bluetooth pads | yes | yes, with `bt sim ...` |
| `screenshot`, `screenshot64` | The screen: binary, or base64 | yes | no (`screenshot`) |
| `ls <dir>`, `mkdir <path>` | Lists; creates recursively | yes | no (`list_dir`) |
| `put <path> <size>`, `get <path>` | Sends or fetches a file, over USB | yes | no (`write_file`, `read_file`) |
| `putb64 <path> <size>`, `getb64 <path>` | The same as base64, on any serial link | yes | no (`write_file`, `read_file`) |
| `unzip <zip> <dest>` | Extracts an archive on the card | yes | yes |
| `rm <path>` | Deletes a file or folder, recursively | yes | yes |
| `mv <src> <dst>` | Moves within the card, never overwriting | yes | yes |
| `crashlog [clear]` | Prints or deletes `/system/crashlog.txt` | yes | no (`get_crash_log`) |
| `reboot`, `reboot-flash` | Reboots, or reboots to BOOTSEL | yes | no |
| `reboot-ota` | Applies a staged, signed update | yes | no |
| `usb` | USB Disk Mode | yes | no |
| `audiostat [reset]` | Audio timing | yes | yes |
| `stack` | Stack peaks | yes | no |
| `psram [full]`, `mp3stats` | PSRAM test; MP3 counts | yes | no (`get_heap_info`, `get_mp3_stats`) |
| `alarmpool`, `kbdstat`, `netstat`, `xipstat`, `splash`, `lognoise`, `mp3bench`, `prof` | Firmware diagnostics | yes | no |

While an app runs: `reboot` and `reboot-flash` act at once (no teardown); `reboot-ota` is dropped; `usb` waits for the launcher. Exit to the launcher before flashing. `get`, `put`, `screenshot` and the base64 forms share the port with the log, so each ends with a checksum to check against, and other log lines are dropped during a transfer and counted when it ends. On the device, a native app that never calls `sys->poll()` does not see commands until the launcher.

### Details

- `stack` prints `msp_peak`, `core1_peak`, the running app's stack peak and
  `os_cmd_peak` (the last launcher command's peak on its 32 KB PSRAM stack).
- `exit` with no app running replies `Error: exit: no app running`.
- `list` prints the apps the launcher shows; `list all` adds the hidden ones
  (a `Hidden apps:` section tagged `[.test]` or `[.dev]`, before the
  `Total: N apps, M hidden` line). Apps under `/apps/.test` and `/apps/.dev`
  are not shown by the launcher and do not count towards its 64-app limit,
  but `launch <dir name or id>` still starts them (a listed app of the same
  name wins; `.test` before `.dev`). `launch .test/<name>` and
  `launch .dev/<name>` look only in that root, so nothing listed can run in
  their place; the whole argument is at most 69 characters, and a longer one
  is refused. The MCP `push_app(..., hidden="test"|"dev")` stages there and
  `launch_app(..., hidden="test"|"dev")` uses the root form.
- `mv <src> <dst>` moves or renames a file or directory on the card, across
  directories (`mv /apps/old_probe /apps/.dev/old_probe`). It creates missing
  parent directories of the target and refuses: an existing target, anything
  at or under `/system` (either side), the top-level directories (`/`,
  `/apps`, `/data`, `/system`), a directory moved into itself, and the
  running app's directory. Paths are canonicalised (`\` and `//`, no `.`/`..`
  or names ending in `.` or a space), and the checks compare directories by
  their identity on the card, not by spelling; if that cannot be checked (the
  card's 16 open files are all in use) the move is refused with "cannot verify
  the move".
- `unzip <zip> <dest>` extracts an archive on the card (it is how `push_app`
  installs on a device). It runs while an app does, except over that app's own
  directory: a destination that is the directory, inside it or above it is
  refused (`Error: unzip: an app is running from <dir>: exit it first`), since
  an open file cannot be replaced. A file that cannot be opened for writing
  stops the extraction with `Error: unzip failed: cannot open <path>`; what was
  already written stays. The MCP `push_app` checks `status` before the upload
  and again right before the `unzip`, and stops with `an app is running` if one
  is, whichever app it is.
- `pad <buttons|none|off> [hold_ms]` drives a test gamepad, as a connected
  controller would: `pad up+a` holds Up and A until the next `pad` command,
  `pad a 100` holds A for 100 ms, `pad none` releases everything, `pad off`
  disconnects it, and `home` opens the system menu (and closes it again when
  it is open: `pad home 100`). Buttons:
  `up down left right a b x y l r start select home`, joined by `+`. Axes join
  them as `name=value`: `lx ly rx ry` from -1 to 1, `lt rt` from 0 to 1, so `pad
  lx=-0.5+rt=1` sets the left stick and the right trigger. An axis is what a
  stick would report: the [dead zone](API-Gamepad.md#analog-sticks-and-triggers)
  applies (`lx=-0.6` reads about -0.575), and it sets no button (a real stick
  past half way also holds the D-pad: `pad left+lx=-0.6`). Replies `Pad: up+a`,
  axes to three places (`Pad: left+lx=-0.600`). The MCP `pad` tool sends it
  (`pad("home, up, a")` presses each in turn).
- `bt` reports Bluetooth (`available`, `enabled`, `power`, `scanning`,
  `link`, `peer`, `ready`, `profile`, `reports`, `paired`, `found`,
  `radio_in_use`, `bus_errors`, then `note="..."`); `bt on|off`, `bt
  scan`, `bt scan stop`, `bt found`, `bt paired`, `bt connect <addr>`, `bt
  disconnect` and `bt forget <addr>` do what the Bluetooth page does.
- Bluetooth controllers are a scripted radio in the simulator: `bt sim add
  <addr> <class hex> <name>` puts a device in pairing mode in range (`bt sim
  clear` empties it), `bt sim press <buttons|none>` sends the connected pad's
  state, `bt sim drop` drops its link and `bt sim wake <addr>` makes a paired
  pad reconnect. The Bluetooth page, the setting and the pairing file are
  the firmware's.

## Working on PicoDeck itself

This part is for changing the firmware and simulator. It needs a [clone with submodules](#get-the-simulator).

### Running the tests

- `pip install -r tests/e2e/requirements.txt`, then
  `SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto` (release simulator).
- Sanitizers: `make simulator-asan` / `simulator-tsan`, then
  `PICODECK_SIM_BINARY=build_sim_asan/picodeck_simulator pytest tests/e2e -n auto`.
- Firmware network stack in the simulator: `make simulator-net`,
  `PICODECK_SIM_BINARY=build_sim_net/picodeck_simulator pytest tests/e2e/test_network_firmware.py`.
- On a real device: `pytest tests/e2e --target hw:/dev/serial/by-id/<PicoDeck device>`
  runs the `hardware`/`both` tests.
- Host unit tests: `make test-unit`; fuzzers: `make fuzz`.

[`tests/e2e/README.md`](https://github.com/PicoDeck/picodeck/blob/master/tests/e2e/README.md) has the rest.

### More on the control protocol

The [method table](#driving-it-from-a-script) covers an app author's needs. Further details:

- `launch_app` → `{queued, busy, launch_id}`; `app.exited` notifications carry
  `{name, id, found, result: returned|error|exit_sentinel|load_failed, error,
  runtime_ms, launch_id}`; `get_last_outcome` returns the last one.
- `get_log_buffer {since_seq, tail}` → `{lines:[{seq,t_ms,src,text}], next_seq,
  dropped, more}`; `subscribe {"logs":true}` pushes `log` notifications.
- `inject_*` return `input_seq`; `get_input_state` reports `consumed_seq`.
- `dev_command {"cmd": "..."}` runs `ping`/`exit`/`unzip`/`rm`/`mv`/`list [all]`/`pad`/`bt`/`audiostat [reset]`
  through the firmware's handlers.
- `step_time {ms}` and `set_time_multiplier` drive the virtual clock.
- Flags: `--test-mode` (error screens return at once; constant `math.random`
  seed and string hash seed; clock pinned to 2026-01-01), `--virtual-time`,
  `--unix-socket PATH|none`, `--real-umm` (firmware allocator; real
  fragmentation), `--build-info`, `--gamepad` / `--no-gamepad`. A host game
  controller is not read under `--test-mode` unless `--gamepad` is given, so
  tests never see it. The browser demo
  ([picodeck.net/try](https://picodeck.net/try/)) reads controllers too, once
  the browser has seen one of their buttons pressed.
