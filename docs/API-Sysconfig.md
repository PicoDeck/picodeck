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
- **Returns:** (string or nil) The value associated with the key, or `nil` if the key does not exist

```lua
local ssid = picocalc.sysconfig.get("wifi_ssid")
if ssid then
    picocalc.repl.print("WiFi SSID: " .. ssid)
end
```

---

#### `picocalc.sysconfig.set(key [, value])`
Set a system configuration value. Pass `nil` as the value (or omit it) to delete the key.

- **Parameters:**
  - `key` (string): Configuration key name
  - `value` (string or nil, optional): Value to set, or `nil` to delete the key
- **Returns:** None

```lua
picocalc.sysconfig.set("brightness", "80")
picocalc.sysconfig.set("old_key")  -- deletes "old_key"
```

---

#### `picocalc.sysconfig.load()`
Reload the configuration from `/system/config.json` on the SD card, discarding any unsaved in-memory changes.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was loaded successfully

```lua
if picocalc.sysconfig.load() then
    picocalc.repl.print("Config reloaded")
end
```

---

#### `picocalc.sysconfig.save()`
Write the current in-memory configuration to `/system/config.json` on the SD card.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was written successfully

```lua
picocalc.sysconfig.set("timezone", "America/New_York")
picocalc.sysconfig.save()
```

---

### Well-Known Keys

| Key | Description |
|---|---|
| `"wifi_ssid"` | WiFi network name for auto-connect on boot |
| `"wifi_pass"` | WiFi password |
| `"brightness"` | Display brightness level |
| `"tz_offset"` | Clock offset from UTC in minutes (may be negative) |
| `"dim_timeout_s"` | Idle screen-dim timeout in seconds; `"0"` disables dimming (default `60`) |
| `"battery_pct"` | `"1"` shows the battery percentage inside the header's battery icon instead of a fill bar (system menu → Settings → Battery %) |
