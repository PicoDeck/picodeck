---
title: "Crash Logging and Watchdog"
---

When something goes wrong, PicoDeck leaves a record on the SD card that survives the reboot, so it can be read later. An app with the `root-filesystem` requirement can read these files through `picocalc.fs`; otherwise read them from the card on a computer, or over the dev console.

## The records

| File | Written | Holds | Size |
|------|---------|-------|------|
| `/system/error.log` | While the OS runs | Failures the OS survived: an app's Lua error, a native loader error, a launch it refused | Emptied when it has passed 64 KB and the next entry comes |
| `/system/crashlog.txt` | At the next boot | `HARDFAULT` and `UNCLEAN EXIT` entries | Emptied when it has passed 64 KB and the next entry comes |
| `/system/running.txt` | At every launch, deleted when the app returns | Which app is running | Five lines |
| `/system/filesystem.log` | When the SD layer reports a card error | One line per error | No limit |

`error.log`, `crashlog.txt` and `filesystem.log` are appended to, so entries accumulate. They are never rotated: passing the size limit loses the old entries all at once.

---

## error.log

Every entry has the same shape, and ends with the state of the Lua heap and a blank line:

```
--- LUA ERROR [My App] ---
Runtime error:
/apps/my_app/main.lua:3: attempt to index a nil value (local 'x')
Heap: free=7865K largest=7865K frag=0%

```

The bracket holds the app's display name (`"name"` in `app.json`). The line under it says what was being done, and the line after that gives the reason. A Lua message is cut at 255 bytes, and an error value that is not a string or number is logged as `unknown error`.

| Kind | Written when |
|---|---|
| `LUA ERROR` | A Lua app stopped on an error. Second line: `Runtime error:`, `Load error:` (it did not compile), `Out of memory:`, `Out of memory loading app:` or `Failed to load app:` |
| `LUA PANIC` | An error outside any protected call. The device shows it and reboots |
| `NATIVE ERROR` | The loader refused a native app (`main.elf`) or could not start it |
| `APP FAILED` | The launcher refused to start an app: an invalid `id`, or `min_psram_kb` not met |
| `OTA REJECTED` | A firmware update failed its checks at boot; the image is renamed `update.bin.rejected` |
| `OTA IGNORED` | `/system/update.bin` was there with no update requested; it is renamed `update.bin.stale` |
| `OS ERROR` | The launcher itself failed to start |

**Reading a Lua error.** The position is the file and line: `/apps/my_app/main.lua:3:` for `main.lua`, and the same form for a module loaded with `require`. The screen shows the same message (dismiss it with **Esc**). There is no stack trace on the device, on the screen or in the log. In the simulator, its test channel's outcome for the app includes one.

To log your own errors, catch them with `pcall` and call `picocalc.sys.log` (USB serial) or write your own file: nothing lets an app add to `error.log`.

---

## running.txt

Written when an app starts and deleted when it returns to the launcher, so finding it at boot means the app never came back:

```
app=My App
id=com.example.myapp
type=lua
launched_at=42s
heap=free=7865K largest=7865K frag=0%
```

`type` is `lua` or `native`; `launched_at` is seconds since boot. Deliberate reboots (the system menu, `sys.reboot()`, the dev `reboot` command) delete it first.

At the next boot the marker is read and deleted, and what happens depends on what ended the session:

| What happened | Screen | Record |
|---|---|---|
| HardFault | "Recovered from crash" | `HARDFAULT` entry in `crashlog.txt`, naming the app |
| Watchdog reset | "App hung - watchdog reset" | `UNCLEAN EXIT` entry in `crashlog.txt` |
| Power off or reset button | None | None (the serial log mentions it): this is normal use |

---

## crashlog.txt

Both kinds of entry end with a blank line. An app that hung until the watchdog fired:

```
--- UNCLEAN EXIT ---
  Reason: watchdog timeout (Core 0 hung)
  app=My App
  id=com.example.myapp
  type=lua
  launched_at=42s
  heap=free=7865K largest=7865K frag=0%

```

A HardFault, with made-up values here:

```
--- HARDFAULT ---
  Uptime: 3m 12s
  App: My App
  PC   = 0x10045a3c
  LR   = 0x10045a11
  SP   = 0x2007f8c0
  CFSR = 0x00000200
  HFSR = 0x40000000
  SFSR = 0x00000000
  Stack: PSP (native app)
  BFAR = 0x00000000
  PRECISERR: precise data bus fault
  HFSR: FORCED escalation

```

- **`App`** is `(none -- OS/launcher)` when no app was running.
- **`Stack`** is where the fault happened: `MSP (OS)`, `PSP (Lua VM)`, `PSP (native app)` or `PSP (OS command)`.
- **`STACK OVERFLOW (MSPLIM/PSPLIM)`** is a line of its own after `App` when a stack hit its limit. `PC` and `LR` then read `(unreliable: STKOF)`, as the CPU may have stacked garbage.
- **`During boot (attempt N)`** follows `Stack` when the fault came before the launcher started.
- **The line after those** is the fault address: `SFAR` for a security fault, `BFAR` for any other fault that set a status bit (it holds a valid address only on a precise data bus fault), and otherwise an `IPSR = ...` diagnostic line.
- **The last lines** name each status bit that was set, in words: `UNDEFINSTR`, `IBUSERR`, `DIVBYZERO`, `STKOF` and the rest.

For a fault inside a native app, the fault screen and the serial log also print `PC-ELF` and `LR-ELF`: those addresses as your ELF has them, to look up with `arm-none-eabi-addr2line -e main.elf`. They are not in this file, so note them from the screen.

---

## What happens at a HardFault

1. The handler runs on the core that faulted.
2. It prints the registers and the decoded status bits to the LCD and to USB serial, so you can read them with no adapter attached.
3. It keeps the record in the watchdog's scratch registers, which survive the reset.
4. It holds the screen for about 3 seconds and reboots.
5. At the next boot, once the SD card is mounted, the record becomes a `HARDFAULT` entry in `/system/crashlog.txt`.

---

## Boot-loop protection

While booting, scratch 0 counts boot attempts. A boot that faults or hangs until the watchdog counts as a failure; reaching the launcher resets the count. After 3 failed boots the boot watchdog stays off, and a fault at that point shows "Boot failed N times - halted" and stops instead of rebooting. Power-cycle to retry (on batteries, a USB replug does not clear it).

---

## filesystem.log

Card and filesystem errors are appended here, as distinct from ordinary results such as "file not found". Only four FatFS results count:

| Name in the log | FatFS result | Meaning |
|---|---|---|
| `DISK_ERR(1)` | `FR_DISK_ERR` | Low-level I/O error from the SPI layer |
| `INT_ERR(2)` | `FR_INT_ERR` | Internal FatFS assertion failure |
| `NOT_READY(3)` | `FR_NOT_READY` | Physical drive not ready |
| `NO_FILESYSTEM(13)` | `FR_NO_FILESYSTEM` | No valid FAT/exFAT volume found |

Each line has the milliseconds since boot, the result, the operation and the path when there is one:

```
[    1234ms] DISK_ERR(1) on sdcard_fopen: /apps/myapp/main.lua
[    5678ms] NOT_READY(3) on sdcard_fwrite
```

The operations are `sdcard_init`, `sdcard_remount`, `sdcard_fopen`, `sdcard_fread`, `sdcard_fwrite`, `sdcard_list_dir(opendir)` and `sdcard_list_dir(readdir)`.

After 3 errors in a row nothing more is written (a failing card would otherwise fill the log with one line per block) until a write to the card succeeds and the count starts again. The serial console keeps reporting them, once a second. The file has no size limit, so delete it now and then if a card is failing.

---

## Watchdog

The watchdog resets the device 10 seconds after it was last fed. Boot steps feed it at each checkpoint. What feeds it while an app runs:

- **A Lua app:** the OS's service hook, which runs about once a millisecond while the Lua VM is executing, and `sys.sleep`, `input.update` and the OS's own dialogs. A Lua loop cannot trip it, since the hook runs inside the loop; one C call that takes 10 seconds can. The keyboard is only read in `input.update()` and `sys.sleep()`, so a Lua loop that calls neither cannot open the system menu.
- **A native app:** `api->sys->poll()`, or Core 1 on its behalf, for as long as the last `poll()` is under 60 seconds old. So a native app that stops polling is reset after about 70 seconds, and until then also ignores the system menu.
- **The launcher:** its own loop.

After a watchdog reset, the next boot prints `watchdog_caused_reboot=1` to the serial console and, if an app was running, leaves the record described under [running.txt](#runningtxt).

An app has to let the system run at least once in those 10 seconds. A Lua app does so all the time while Lua code runs, and the calls that can take long (`conn:waitConnected`, `conn:waitData`, `fs.copy`, `fs.read`, `fs.readFile` and `fs.write` of a large file, `picocalc.zip.extract` and the archive read and extract methods) keep feeding it themselves, whatever the size; the menu and Exit App wait until they return.

---

## Testing the crash handler

`picocalc.sys.triggerFault()` writes to address 0, which triggers an immediate bus fault. Use it from a Lua app to check the whole crash, reboot and log cycle on a device:

```lua
-- apps/crashtest/main.lua
picocalc.sys.log("About to trigger fault...")
picocalc.sys.triggerFault()
-- device will reboot; check /system/crashlog.txt afterwards
```

In the simulator the same call ends the simulator's process with a segmentation fault, and writes no record.
