---
title: "API System Configuration"
---

> `picocalc.sysconfig` exists only for apps whose `app.json` declares the
> `"sysconfig"` requirement (otherwise it is nil). `get("wifi_pass")` always
> returns nil — the WiFi password is write-only through this API (`set`
> works). Note that `/system/config.json` itself is readable by a
> `root-filesystem` app through `picocalc.fs`.

System-wide configuration stored at `/system/config.json` on the SD card. This is distinct from per-app configuration (`picocalc.config`). System configuration persists across all apps and reboots.

## picocalc.sysconfig

### Functions

#### `picocalc.sysconfig.get(key)`
Get a system configuration value by key.

- **Parameters:**
  - `key` (string): Configuration key name
- **Returns:** (string or nil) The value associated with the key, or `nil` if the key does not exist. Unlike `picocalc.config.get` it takes no fallback.

```lua
local ssid = picocalc.sysconfig.get("wifi_ssid")
if ssid then
    picocalc.sys.log("WiFi SSID: " .. ssid)
end
```

---

#### `picocalc.sysconfig.set(key [, value])`
Set a system configuration value in memory; call `save()` to keep it. A `value` of `nil` or `""`, or none at all, deletes the key. A number is accepted and reads back as a string.

Most keys only take effect when the OS next reads them: see [Well-Known Keys](#well-known-keys). `brightness`, for one, changes nothing until a reboot.

**Limits.** Keys are cut to 31 characters and values to 127, silently. All keys and values share one 1 KB store (each costs its length plus 2 bytes); a new key, or a longer value, that does not fit is ignored and the old value kept, also silently. `set` returns nothing, so read the key back if it matters. This store is the OS's, shared by every app and with little room: keep your own settings in [`picocalc.config`](API-System-and-Config.md#picocalcconfig).

- **Parameters:**
  - `key` (string): Configuration key name
  - `value` (string, number or nil, optional): Value to set, or `nil` to delete the key
- **Returns:** None

```lua
picocalc.sysconfig.set("dim_timeout_s", "120")
picocalc.sysconfig.set("old_key")  -- deletes "old_key"
```

---

#### `picocalc.sysconfig.load()`
Reload the configuration from `/system/config.json` on the SD card, discarding any unsaved in-memory changes.

The OS loads this file at boot, so you only need this to drop your own unsaved changes.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was loaded; `false` if there is no file yet, which leaves the store empty

```lua
if picocalc.sysconfig.load() then
    picocalc.sys.log("Config reloaded")
end
```

---

#### `picocalc.sysconfig.save()`
Write the current in-memory configuration to `/system/config.json` on the SD card.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was written successfully

```lua
picocalc.sysconfig.set("tz_offset", "-300")  -- UTC-5
picocalc.sysconfig.save()
```

---

### Well-Known Keys

These are the keys the OS and its apps read, and when they read them. Values are all strings. An app that sets one with `read at boot` here needs a reboot to see the effect.

| Key | Values | Read | Description |
|---|---|---|---|
| `"wifi_ssid"` | Up to 127 characters | At boot, when an `http` app starts, and by the menu | WiFi network name for auto-connect |
| `"wifi_pass"` | Up to 63 characters | The same | WiFi password. `get` always returns nil |
| `"wifi_auto_disconnect"` | `"0"` keeps WiFi up after the boot's time sync; anything else, or no key, disconnects unless an app needs it | At boot, and the menu changes it live | System menu → Settings → Auto Disconnect |
| `"brightness"` | `"0"`-`"255"`; default 128 | At boot | Backlight level. A value under 16 restores as 16, so a saved 0 cannot boot to a dark screen. To change it now, use `picocalc.display.setBrightness` |
| `"dim_timeout_s"` | Seconds; `"0"` disables dimming; default 60 | At boot | Idle screen-dim timeout |
| `"tz_offset"` | Minutes from UTC, may be negative (`"330"`, `"-300"`) | At every clock read | Clock offset. System menu → Settings → Timezone sets it |
| `"battery_pct"` | `"1"`, or anything else for off | At every header draw | Shows the battery percentage inside the header's battery icon instead of a fill bar (system menu → Settings → Battery %) |
| `"show_fps"` | `"tr"`, `"tl"`, `"br"`, `"bl"`; no key (or `"0"`) is off, and the menu removes the key for Off | When an app starts and when the system menu closes | Corner of the OS FPS counter (Settings → Show FPS; see [API Performance](API-Performance.md)) |
| `"bt_enabled"` | `"1"`: on. No key is off, and the menu removes the key for Off | At boot | Bluetooth controllers (system menu → Settings → Bluetooth; see [API Gamepad](API-Gamepad.md#bluetooth-controllers)) |
| `"dev_mode"` | `"1"`: on | At boot, and the menu changes it live | Developer rows in the system menu (Settings → Developer Mode) |
| `"store_url"` | A catalog URL, up to 127 characters | By the Store app, when it fetches | Points the Store at another catalog, for testing one |
| `"store_download_base"` | An `http://` or `https://` URL, up to 127 characters; default `https://github.com` | By the Store app, at each download | Where the Store downloads release assets: `<base>/<repo>/releases/download/<tag>/<asset>`, for an indexer that serves its own |
| `"editor_font"` | A font number | By the Editor app, when it starts | The Editor's last font |

Nothing reads any other key, such as a `timezone`: an unknown key is stored and ignored, and it takes room in the 1 KB store.
