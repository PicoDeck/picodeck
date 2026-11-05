---
title: "API System and Config"
---

System-level functions and persistent configuration storage.

## picocalc.sys

### Functions

#### `picocalc.sys.getTimeMs()`
Returns milliseconds since boot.

- **Parameters:** None
- **Returns:** (number) Milliseconds since system startup

```lua
local start = picocalc.sys.getTimeMs()
-- do work
local elapsed = picocalc.sys.getTimeMs() - start
```

---

#### `picocalc.sys.getTimeUs()`
Returns microseconds since boot as a 32-bit integer that wraps (it goes negative after ~35.8 minutes and wraps every ~71.6 minutes).

- **Parameters:** None
- **Returns:** (integer) Microseconds since startup, modulo 2^32

Subtract two readings **as integers** for an interval: the difference is exact for gaps under ~35 minutes, even across the wrap. Do not convert a reading to a float first (a float holds microseconds exactly only up to ~16.8 s). In the simulator the value moves in 1 ms steps unless it runs with virtual time.

```lua
local t0 = picocalc.sys.getTimeUs()
-- work
local us = picocalc.sys.getTimeUs() - t0
```

---

#### `picocalc.sys.sleep(ms)`
Sleeps for the specified number of milliseconds. Does not consume input events.

`sys.sleep()` and `input.update()` both service the OS while they run: HTTP,
TCP and sound callbacks, the system menu and dev commands. Callbacks can
therefore fire inside `input.update()`.

- **Parameters:**
  - `ms` (number): Milliseconds to sleep
- **Returns:** None

```lua
picocalc.sys.sleep(100)  -- Sleep for 100ms
```

---

#### `picocalc.sys.getBattery()`
Returns the battery charge level. The level is read from the keyboard controller in the background every 5 seconds (paused while the app goes a second without `input.update()` or `sys.sleep()`); this returns the latest reading without waiting.

- **Parameters:** None
- **Returns:** (number) Battery percentage (0-100), or -1 only before the first reading (a few seconds after boot). On USB power it keeps returning the last level read.

```lua
local battery = picocalc.sys.getBattery()
if battery >= 0 then
    print("Battery: " .. battery .. "%")
end
```

---

#### `picocalc.sys.isUSBPowered()`
Checks if the device is powered via USB (GP24 VBUS sense).

- **Parameters:** None
- **Returns:** (boolean) `true` when USB power is connected

> Not reliable for USB detection: GP24 is the Pico 2's VBUS sense pin, but on
> Pico W-family boards, including the PicoCalc's Pimoroni Pico Plus 2 W, that
> pin belongs to the wireless chip, so this currently reads `false` even on
> USB power. A fix (reading VBUS through the CYW43) is pending.

---

#### `picocalc.sys.resetIdleTimer()`
Resets the idle screen-dim timer. Call on user activity to keep the display from dimming (see the `dim_timeout_s` key in [API Sysconfig](API-Sysconfig.md)). An app starts with the timer fresh and the screen lit. The key press that wakes a screen dimmed while an app runs does not reach the app.

- **Parameters:** None
- **Returns:** None

```lua
if pressed ~= 0 then
    picocalc.sys.resetIdleTimer()
end
```

---

#### `picocalc.sys.log(message)`
Logs a message to the USB serial debug output (115200 baud).

- **Parameters:**
  - `message` (string): Message to log
- **Returns:** None

```lua
picocalc.sys.log("Debug info: x=" .. tostring(x))
```

---

#### `picocalc.sys.exit()`
Ends the app and returns to the launcher, from any call depth. It does the same as `return` at the top level of `main.lua`.

- **Parameters:** None
- **Returns:** Never returns

```lua
picocalc.sys.exit()
```

**An exit cannot be caught.** `sys.exit()`, the system menu's Exit App and the dev `exit` command all raise an error that keeps coming back: `pcall`, `xpcall` and `coroutine.resume` catch the raise, and the next Lua instruction after them raises it again.

**What runs at exit.** Nothing of yours is called when the app exits, Exit App included, so an app that means to save on the way out does not get to. Save when the value changes:

```lua
volume = 80
picocalc.config.set("volume", volume)
picocalc.config.save()
```

- A `__close` handler (a `local x <close>` variable) does not finish: the exit raises again at its first instruction. It runs in full on a normal return and on an ordinary error.
- A `__gc` handler does run, as the VM closes, however the app ended. So a last write can be done from one, if the object stays referenced until then:

  ```lua
  level = 1
  saver = setmetatable({}, { __gc = function()  -- a global, so it lives to the end
      picocalc.config.set("level", level)
      picocalc.config.save()
  end })
  ```
- Choosing Exit App first runs the callbacks of `addMenuOption` rows that changed. `addMenuItem` callbacks do not run; add your own "Save and quit" item if you want one.
- An app that hangs or faults runs none of this. See [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md).

---

#### `picocalc.sys.reboot()`
Triggers a system reboot via the watchdog timer. The simulator has no reset to model: there the app ends and the launcher takes over.

- **Parameters:** None
- **Returns:** Never returns

```lua
picocalc.sys.reboot()
```

---

#### `picocalc.sys.getClock()`
Returns the current time as a table. Time is synchronized via NTP when WiFi is connected.

- **Parameters:** None
- **Returns:** (table) Clock data with fields:
  - `synced` (boolean): `true` if time has been synchronized via NTP. Until then `hour`, `min`, `sec` and `epoch` are all `0`, and HTTPS/TLS connections refuse to start (see `conn:setInsecure`)
  - `hour` (number): Current hour (0-23, adjusted for timezone)
  - `min` (number): Current minute (0-59)
  - `sec` (number): Current second (0-59)
  - `epoch` (number): UTC Unix timestamp in seconds

```lua
local clock = picocalc.sys.getClock()
if clock.synced then
    local time_str = string.format("%02d:%02d:%02d", clock.hour, clock.min, clock.sec)
    picocalc.display.drawText(10, 10, time_str, picocalc.display.WHITE)
end
```

---

#### `picocalc.sys.getMemInfo()`
Returns a snapshot of heap memory usage.

- **Parameters:** None
- **Returns:** (table) with fields:
  - `psram_free` (number): Free bytes in the Lua PSRAM heap
  - `psram_used` (number): Used bytes in the Lua PSRAM heap
  - `psram_total` (number): Total bytes in the Lua PSRAM heap
  - `psram_largest_block` (number): Largest single free block — what one big allocation can get (compare [`min_psram_kb`](App-Json-Reference.md#min_psram_kb) in app.json)
  - `psram_fragmentation` (number): 0-100
  - `small_pool_slabs`, `small_pool_bytes` (number): Lua small-object pool slabs (4 KB each, carved from the PSRAM heap) and their bytes
  - `small_pool_objects`, `small_pool_object_bytes` (number): live objects of up to 128 B in those pools and their bytes
  - `sram_free` (number): Free bytes in the system SRAM heap (via `mallinfo`; the SRAM heap is only a few KB)
  - `sram_used` (number): Used bytes in the system SRAM heap
  - `pio_psram_available` (boolean), `pio_psram_size` (number): mainboard PIO PSRAM
  - `xip_cache_hit_rate` (number): the flash/PSRAM cache's hit rate in percent (0-100) since the previous `getMemInfo()` call, or `-1` when nothing was counted, when the hardware counters filled up meanwhile (a busy app fills them in about 40 s), or in the simulator. Call `getMemInfo()` once to start a measurement and again to read it.

```lua
local mem = picocalc.sys.getMemInfo()
picocalc.sys.log(string.format("PSRAM: %dKB free / %dKB total",
    mem.psram_free // 1024, mem.psram_total // 1024))
```

---

#### `picocalc.sys.addMenuItem(label, callback)`
Adds a custom item to the system menu overlay (the menu key: Shift+F5, reported as F10). Maximum **4 items per app**, counting the option rows of `addMenuOption` too; a fifth raises `too many menu items (max 4)`.

- **Parameters:**
  - `label` (string): Menu item text, cut to 31 bytes
  - `callback` (function): Function to call when the item is selected
- **Returns:** None

The callback runs after the menu has closed and has given your screen back
(when memory allows; see below), with your clip rect and a clean keyboard. It
may call `picocalc.sys.exit()` or raise an error. What it draws goes into the
back buffer, as any drawing does, and shows at your next `flush`.

```lua
picocalc.sys.addMenuItem("Restart Level", function()
    level = 1
end)
```

**Your screen after the menu.** The system menu darkens your frame and draws
over it. When it closes it puts your screen back, both framebuffers in the
roles they had: the frame that was showing is on the panel again, and the
back buffer holds what you had drawn into it (a half-drawn frame included). So
an app that redraws only what changed, or waits for a key without drawing,
needs no repaint after the menu. If memory is too short for the copy (it
needs a free 400 KB block of the Lua heap while the menu is open, on top of
the 200 KB the menu uses itself, and must still leave an 8 KB block for
Settings → Controls), the screen stays darkened until the app redraws. An app that presents only with `flushRows`
(no buffer swap) gets its other buffer on the panel after the menu: the rows
it flushes again come back, and any row it never flushes again stays stale.
The menu also resets a hardware scroll offset (see
`setScrollOffset` in [API Display and Graphics](API-Display-and-Graphics.md)).

**Your keys after the menu.** Your app reads nothing of what is down when the
menu opens or closes, the key or controller button that closed it included,
until each is released and pressed again; a callback that runs after the menu
closes reads the same. See [`input.update()`](API-Input.md).

---

#### `picocalc.sys.addMenuOption(label, options, current, callback)`
Adds a value row to the system menu overlay, shown as `Label: value <>`.
It shares the limit of **4 items per app** with `addMenuItem`.

On firmware older than this function, `picocalc.sys.addMenuOption` is `nil`.
Check before calling:

```lua
if picocalc.sys.addMenuOption then
    picocalc.sys.addMenuOption("Focus", { "20 min", "25 min" }, "25 min", set_focus)
else
    -- older firmware: offer the setting in the app instead
end
```

- **Parameters:**
  - `label` (string): Row text, at most 12 bytes
  - `options` (table): 1 to 16 strings, each at most 14 bytes. The values are
    copied: changing the table afterwards changes nothing.
  - `current` (string): The value shown first; must be one of `options`
  - `callback` (function): `callback(value, index)`, with the new value and
    its 1-based index
- **Returns:** None

Left and Right step the value (they stop at the ends); Enter steps forward and
wraps from the last value to the first. Stepping does not close the menu.

The callback runs after the menu has closed (like an `addMenuItem` callback),
only for rows whose value differs from the one the row had when the menu
opened. Callbacks of changed option rows run in row order, before the callback
of an action row chosen with Enter. The row keeps its value between visits.
Choosing Exit App also runs the callbacks of changed rows, before the app
exits. A bad argument raises an error and uses no slot.

If a callback clears the menu (`clearMenuItems`), the callbacks still pending
from that visit do not run. To rebuild rows, set a flag and rebuild from your
main loop.

```lua
picocalc.sys.addMenuOption("Focus", { "20 min", "25 min", "30 min" }, "25 min",
    function(value, index) focus_minutes = tonumber(value:match("%d+")) end)
```

---

#### `picocalc.sys.clearMenuItems()`
Removes all app-registered menu items, action rows and option rows alike.
Called automatically on app exit. Called from a menu callback, it also cancels
the callbacks still pending from that menu visit (see `addMenuOption`).

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.sys.triggerFault()`
Deliberately triggers a HardFault by writing to address 0. **For crash-handler testing only — never call in production code.**

The fault handler will display register state on screen, save crash data via watchdog scratch registers, then reboot. On the next boot the crash data is written to `/system/crashlog.txt`.

- **Parameters:** None
- **Returns:** Never returns

```lua
-- Test that crash logging works
picocalc.sys.triggerFault()
```

---

#### `picocalc.sys.getVersion()`
Get the PicoDeck firmware version string.

- **Parameters:** None
- **Returns:** (string) Version string: `"0.6.0"` on a release, and on a development build what `git describe` gives (`"0.6.0-12-g272bb8c"`)

```lua
local version = picocalc.sys.getVersion()
picocalc.sys.log("PicoDeck version: " .. version)
```

---

#### `picocalc.sys.getPowerStatus()`
Get detailed power status, as the keyboard controller reports it (the same reading as the charging bolt in the header).

- **Parameters:** None
- **Returns:** (table) With fields:
  - `charging` (boolean): Whether the battery is currently charging
  - `percent` (number): Battery percentage (0-100), or -1 before the first reading, as `getBattery()`

```lua
local power = picocalc.sys.getPowerStatus()
if power.charging then
    picocalc.sys.log("Charging: " .. power.percent .. "%")
end
```

---

#### `picocalc.sys.applyUpdate(path)`
Flash a firmware image (`picodeck.bin`) from the SD card. **Only present**
for apps that declare `"system-update"` AND are OS apps (id
`net.picodeck.updater` / `net.picodeck.store`, or installed under `/system/`).
`path` goes through the sandbox like any other: to read `/system/update.bin` the
app also needs `"root-filesystem"`, or the call returns `false, "access denied"`.

Before anything is flashed:
- `/system/update.sha256` (exactly 64 hex digits + optional newline) must match
  the image, and `/system/update.sig` (DER ECDSA P-256 over the SHA-256 of the
  image) must verify against the key built into the running firmware;
- a confirmation dialog names the file; declining returns `false, "cancelled"`
  (keys pressed before the dialog appears are ignored).

The image is copied to `/system/update.bin` and the device reboots; the boot
checks the checksum and signature again before writing flash. An image the
boot refuses is renamed `update.bin.rejected` and logged (`OTA REJECTED` in
`/system/error.log`); it is never retried. A `/system/update.bin` found at boot
without an update request is renamed `update.bin.stale`.

- **Parameters:**
  - `path` (string): Path to the `.bin` image on the SD card
- **Returns:** does not return on success; `false, err` on failure

```lua
local ok, err = picocalc.sys.applyUpdate("/system/update.bin")
```

(Release assets: `picodeck.bin`, `picodeck.sha256`, `picodeck.sig`.
Signing: `tools/sign_update.py`. Local builds embed a TEST key whose private
half is in the repo; release builds use the `UPDATE_SIGNING_KEY` CI secret.)

---

#### `picocalc.sys.pauseBackground()`
Pause Core 1 background tasks (WiFi polling, audio decode, HTTP). Useful before intensive SD card operations.

- **Parameters:** None
- **Returns:** (boolean) `true` if Core 1 successfully paused

```lua
picocalc.sys.pauseBackground()
-- perform intensive SD card operations
picocalc.sys.resumeBackground()
```

---

#### `picocalc.sys.resumeBackground()`
Resume Core 1 background tasks after `pauseBackground()`.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.sys.resumeBackground()
```

---

#### `picocalc.sys.loadlib(name)`
Load a shared Lua library from `/system/lib/<name>.lua` and return its result. Libraries are standard Lua files that return a table of functions.

- **Parameters:**
  - `name` (string): Library name (without `.lua` extension)
- **Returns:** Whatever the library script returns (typically a table)

Returns `nil, errmsg` when `/system/lib/<name>.lua` cannot be opened. Only `.lua` source is loaded (bytecode is rejected).

```lua
local download = picocalc.sys.loadlib("download")
```

JSON is not loaded this way. `picocalc.json` is a built-in namespace, always present (see [API-JSON](API-JSON.md)); there is no `json.lua` library:

```lua
local data = picocalc.json.decode(raw)
```

The libraries in the repo's `system/lib/` are the ones that exist. Each is a plain file that has to be present on the SD card at `/system/lib/`; release packaging does not install them for you:

| Name | File | Docs |
|------|------|------|
| `"download"` | `download.lua` | [Library-Download](Library-Download.md) |
| `"panels"` | `panels.lua` | [Library-Panels](Library-Panels.md) |
| `"widgets"` | `widgets.lua` | [Library-Widgets](Library-Widgets.md) |

`require` (see [Standard-Lua-Libraries](Standard-Lua-Libraries.md)) also searches `/system/lib`, after your app's folder. Since nothing installs these files on a user's card, [a published app should bundle what it uses](Publishing-Apps.md#bundle-the-libraries-you-use).

---

#### `picocalc.sys.pioPsramRead(addr, len)`
Read bytes from PIO PSRAM (mainboard 8MB).

Apps may use addresses from `0x48000` to the end of the chip; anything below
(the OS's MP3 ring and reserved video region), negative, or past the end
raises an error.

- **Parameters:**
  - `addr` (number): Byte address
  - `len` (number): Number of bytes to read
- **Returns:** (string) Data, or `nil` if PIO PSRAM not available

```lua
local data = picocalc.sys.pioPsramRead(0x48000, 256)
```

---

#### `picocalc.sys.pioPsramWrite(addr, data)`
Write bytes to PIO PSRAM.

Apps may use addresses from `0x48000` to the end of the chip; anything below
(the OS's MP3 ring and reserved video region), negative, or past the end
raises an error.

- **Parameters:**
  - `addr` (number): Byte address
  - `data` (string): Bytes to write
- **Returns:** (number) Bytes written (0 if unavailable)

```lua
local written = picocalc.sys.pioPsramWrite(0x48000, myData)
```

---

#### `picocalc.sys.pioPsramSize()`
Get PIO PSRAM size.

- **Parameters:** None
- **Returns:** (number) Size in bytes (0 if not available)

```lua
local size = picocalc.sys.pioPsramSize()
if size > 0 then
    picocalc.sys.log("PIO PSRAM: " .. (size // 1024) .. " KB")
end
```

---

#### `picocalc.sys.qmiPsramAlloc(size)`
Allocate a buffer in QMI PSRAM (Lua heap). Low-level; prefer standard Lua tables for most uses.

Returns a bounds-checked buffer handle (userdata), not a pointer. It is freed
by `qmiPsramFree` (idempotent) or when garbage-collected; `qmiPsramRead/Write`
raise on an out-of-range offset/length or a freed handle. The handle can be
passed to `picocalc.graphics.image.loadFromBuffer(handle [, len])`.

The block's contents start uninitialised: write before you read.

- **Parameters:**
  - `size` (integer): Bytes to allocate. Zero or less raises `size must be positive`.
- **Returns:** (userdata) Handle, or `nil` when the heap has no block that large

```lua
local buf = picocalc.sys.qmiPsramAlloc(4096)
```

---

#### `picocalc.sys.qmiPsramFree(handle)`
Free a QMI PSRAM allocation.

- **Parameters:**
  - `handle` (userdata): Handle from `qmiPsramAlloc`
- **Returns:** None

```lua
picocalc.sys.qmiPsramFree(buf)
```

---

#### `picocalc.sys.qmiPsramWrite(handle, offset, data)`
Write to a QMI PSRAM buffer.

- **Parameters:**
  - `handle` (userdata): Handle from `qmiPsramAlloc`
  - `offset` (number): Byte offset within the buffer
  - `data` (string): Bytes to write
- **Returns:** (number) Bytes written

```lua
picocalc.sys.qmiPsramWrite(buf, 0, "Hello PSRAM")
```

---

#### `picocalc.sys.qmiPsramRead(handle, offset, len)`
Read from a QMI PSRAM buffer.

- **Parameters:**
  - `handle` (userdata): Handle from `qmiPsramAlloc`
  - `offset` (number): Byte offset within the buffer
  - `len` (number): Number of bytes to read
- **Returns:** (string) Data

```lua
local data = picocalc.sys.qmiPsramRead(buf, 0, 11)
```

---

## picocalc.config

Persistent **per-app** key-value configuration storage, stored at `/data/<APP_ID>/config.json`. Each app gets its own isolated store. The same store is also available under the alias `picocalc.appconfig`: same data, two names.

`picocalc.sysconfig` is the **system-wide** store shared by all apps (`/system/config.json`); see [API Sysconfig](API-Sysconfig.md). It exists only for apps whose `app.json` declares the `"sysconfig"` requirement (otherwise it is nil).

### Which store to use

| Store | For | Shape |
|---|---|---|
| `picocalc.config` | This app's settings | Up to 32 string values, 255 bytes each |
| `picocalc.game.save` | This app's progress, scores, anything larger | Any Lua table, one JSON file per named slot ([API Game](API-Game.md)) |
| `picocalc.fs` + `picocalc.json` | Your own files and formats | Whatever you write ([API Filesystem](API-Filesystem.md), [API JSON](API-JSON.md)) |
| `picocalc.sysconfig` | The OS's own settings (WiFi, brightness, time zone) | Needs the `"sysconfig"` requirement |

### Limits

- **32 keys.** A new key past that is refused: `set` returns `false, "config full"`. Replacing an existing key still works, and deleting one makes room.
- **Keys up to 31 bytes, values up to 255.** Longer ones are refused, never cut short: `set` returns `false, "key too long"` or `false, "value too long"` and an existing key keeps its old value.
- **Values are strings.** A number is accepted and reads back as a string (`set("n", 5)`, then `get("n")` is `"5"`); any other type raises an argument error. A string with a NUL byte in it is refused.

The store loads itself the first time you call any of its functions, so an explicit `load()` is not needed. It takes about 9 KB of the Lua heap once it holds a key, freed when the app exits.

A `config.json` you edit by hand or write yourself can hold more than this. `load` then `save` never deletes what the store could not load: more than 32 keys, over-long keys or values, and values that are not strings (numbers, `true`, nested arrays and objects) are written back as they were. `get` does not see them; `set` or deleting that key replaces it, and `clear` and `reset` drop them with everything else.

### Functions

#### `picocalc.config.get(key [, fallback])`
Retrieves a configuration value.

- **Parameters:**
  - `key` (string): Configuration key
  - `fallback` (string or number, optional): Returned, as a string, if the key does not exist
- **Returns:** (string or nil) Value, or `fallback`, or `nil` if the key does not exist and no fallback was given

```lua
local highscore = picocalc.config.get("highscore", "0")
local volume = tonumber(picocalc.config.get("volume", 80))
```

---

#### `picocalc.config.set(key [, value])`
Sets a configuration value in memory; call `save()` to keep it. A `value` of `nil` or `""`, or none at all, deletes the key.

- **Parameters:**
  - `key` (string): Configuration key
  - `value` (string, number or nil): Value to store, or `nil` to delete
- **Returns:** `true`, or `false` and a reason when nothing was stored (`"config full"`, `"key too long"`, `"value too long"`, `"empty key"`, `"value contains a NUL byte"`, `"out of memory"`)

Deleting a key that does not exist returns `true`.

```lua
picocalc.config.set("highscore", "1000")
picocalc.config.set("old_key", nil)  -- Delete key

local ok, err = picocalc.config.set("notes", very_long_text)
if not ok then
    picocalc.sys.log("config: " .. err)  -- "value too long": use game.save
end
```

---

#### `picocalc.config.save()`
Saves the current configuration to `/data/<APP_ID>/config.json`, creating `/data/<APP_ID>` if needed. The file is replaced in one step, so a failed save leaves the previous file.

- **Parameters:** None
- **Returns:** (boolean) `true` on success, `false` on error. It also returns `false`, and writes nothing, if the file exists but could not be read when the store loaded (a card error, or no memory for it), since saving would delete the keys in it; a later `load()` that works, or `reset()`, ends that.

```lua
if picocalc.config.save() then
    picocalc.sys.log("Config saved")
end
```

---

#### `picocalc.config.load()`
Reloads the configuration from `/data/<APP_ID>/config.json`, dropping unsaved changes. Not needed before the first `get`: the store loads itself.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was read, `false` otherwise. On a first run there is no file yet, so `false` is normal: the store is simply empty.

```lua
picocalc.config.load()
```

---

#### `picocalc.config.clear()`
Clears all keys from the current app's configuration in memory. Does not delete the config file on disk; call `save()` afterwards to persist the change.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.config.clear()
picocalc.config.save()  -- persist the empty config
```

---

#### `picocalc.config.reset()`
Resets the app configuration by deleting the config file from the SD card and clearing the in-memory state.

- **Parameters:** None
- **Returns:** (boolean) `true`, whether or not there was a file to delete

```lua
if picocalc.config.reset() then
    picocalc.sys.log("App config reset to defaults")
end
```
