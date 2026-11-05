---
title: "Global Variables and Permissions"
---

## Global Variables

These variables are automatically set when your app is launched:

| Variable | Type | Description |
|----------|------|-------------|
| `APP_DIR` | string | Absolute path to your app's directory (e.g., `"/apps/hello"`) |
| `APP_NAME` | string | Name of your app as defined in `app.json` |
| `APP_ID` | string | Reverse-DNS app identifier from `app.json`, folded to lower case (e.g., `"net.picodeck.hello"`) |
| `APP_REQUIREMENTS` | table | Declared requirements as boolean fields (see [App Requirements](#app-requirements)) |

These globals are **copies for your convenience**. The OS enforces the sandbox
and the per-app stores from its own record of the running app, so changing
them grants nothing.

---

## App Requirements

Apps declare requirements in the `requirements` array in `app.json` (the [`app.json` reference](App-Json-Reference.md) has the other fields):

```json
{
  "requirements": ["root-filesystem"]
}
```

Only some of these are permissions. Three names change what an app may do, one changes how WiFi behaves, and the rest only state a fact and do nothing on the device.

### Enforced: without it, the feature is not there

| Requirement | Without it | With it |
|------------|-------------|---------|
| `root-filesystem` | The [sandbox](#default-sandbox) below | Read and write anywhere on the card. Relative paths and `..` are still refused |
| `sysconfig` | `picocalc.sysconfig` is nil | `picocalc.sysconfig` (system-wide config) is registered. `get("wifi_pass")` always returns nil (write-only) |
| `system-update` | `picocalc.sys.applyUpdate` is nil | Registered, but only for the OS's own updaters (id `net.picodeck.updater` or `net.picodeck.store`, or an app under `/system/`). It also needs `root-filesystem` to read an update under `/system` |

### Changes behaviour: `http`

| Requirement | Effect |
|------------|--------|
| `http` | The launcher joins the saved WiFi network when the app starts, and WiFi stays up: it is not dropped after the boot's time sync or for a [`system_clock_khz`](App-Json-Reference.md#system_clock_khz) change |

**`http` is how any networked app gets a network, raw TCP included.** Nothing else enforces it: `picocalc.network.http.new`, `picocalc.tcp.new` and `picocalc.wifi` are there without it, and return objects and `true`. But with no `http` requirement, WiFi may well be off, so the connection fails with nothing naming the cause. (The exceptions: the user turned off Auto Disconnect in Settings, or your app calls `picocalc.wifi.connect` itself.) A [TCP](API-TCP.md) app needs `"http"` even though it makes no HTTP requests.

The simulator is always online, so a missing `http` works there and fails on a device.

### Only declared: nothing checks them

| Requirement | What it does |
|------------|--------|
| `filesystem` | Nothing. It names the default sandbox, which every app has anyway |
| `audio` | Sets `APP_REQUIREMENTS.audio`, and shows in the store. Sound works without it |
| `clipboard` | Nothing. Reserved; there is no clipboard API |

Declare `audio` if your app makes sound, since the store lists it. Any other name is ignored without complaint: `"video"` and `"network"` do nothing.

### Native apps

The requirements above are enforced by the Lua bridge. A [native app](Native-Loading.md) has full access to the card and memory whatever it declares, `/system/config.json` with its WiFi password included, so native apps are not sandboxed. `http`, `system_clock_khz` and `min_psram_kb` work the same for native apps, since the launcher handles them.

### Checking Requirements in Lua

The `APP_REQUIREMENTS` global is a table with boolean fields — `.root_filesystem`, `.http`, `.audio`. `sysconfig` and `system-update` have no field: test `picocalc.sysconfig ~= nil` / `picocalc.sys.applyUpdate ~= nil`.

```lua
if APP_REQUIREMENTS.root_filesystem then
    -- App has full filesystem access
    local entries = picocalc.fs.listDir("/system")
else
    -- Restricted to sandbox
    local path = picocalc.fs.appPath("save.json")
end
```

### Default Sandbox

Without `root-filesystem`, a Lua app can reach only:
- **Read**: `/apps/<dir>/` (your app's folder) and `/system/lib/`
- **Read and write**: `/data/<APP_ID>/` (your app's data folder)

Relative paths and any `..` are refused. Your data folder does not exist until something creates it: `fs.appPath`, `fs.browse`, `config.save`, a [save slot](API-Game.md) or your own `fs.mkdir`. Until then, a direct `fs.open` of a path inside it returns `nil, "cannot open file"`. Use `picocalc.fs.appPath("filename")` to get a path there; it creates the folder.

#### What a refused call does

The same check guards `picocalc.fs` and every loader that takes a path, but a refusal looks different from call to call. As a rule, **loaders that build an image or font object raise an error**, so wrap them in `pcall` if the path can be outside the sandbox; everything else returns.

| Call | Refused result |
|------|----------------|
| `graphics.image.load`, `image.loadRegion`, `image.preload` | **raises** `access denied` |
| `graphics.font.new` | **raises** `access denied: <path>` |
| `display.loadFont` | `nil, "access denied: <path>"` |
| `sound.sample`, `sound.sampleplayer` (with a path), `sample:load`, `fileplayer:load`, `mp3player:load`, `crypto.sha256File` | `nil, "access denied"` |
| `sample:save`, `modplayer:load`, `video:load` | `false, "access denied"` |
| `zip.open` | `nil, "permission denied"` |
| `zip.extract`, `fs.rename`, `fs.copy` | `false, "permission denied (source)"` or `(destination)` |
| `fs.open`, `fs.stat` | `nil, "permission denied"` |
| `fs.delete`, `fs.deleteRecursive` | `false, "permission denied"` |
| `fs.mkdir` | `false`, no message |
| `fs.readFile` | `nil`, no message |
| `fs.listDir`, `fs.glob` | `{}`, an empty table, the same as an empty directory |
| `fs.exists`, `fs.isDir` | `false` |
| `fs.size` | `-1` |
| `require` | **raises**, as for a module that does not exist |

`fs.browse(start)` falls back to your data directory when `start` is not readable.

### App ids

The `id` in `app.json` names your data directory (`/data/<id>`). The rules, and the App Store's stricter ones, are in the [`app.json` reference](App-Json-Reference.md#id).
