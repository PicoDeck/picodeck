---
title: "Global Variables and Permissions"
---

## Global Variables

These variables are automatically set when your app is launched:

| Variable | Type | Description |
|----------|------|-------------|
| `APP_DIR` | string | Absolute path to your app's directory (e.g., `"/apps/hello"`) |
| `APP_NAME` | string | Name of your app as defined in `app.json` |
| `APP_ID` | string | Reverse-DNS app identifier from `app.json` (e.g., `"net.picodeck.hello"`) |
| `APP_REQUIREMENTS` | table | Granted requirements as boolean fields (see [App Requirements](#app-requirements)) |

These globals are **copies for your convenience**. The OS enforces the sandbox
and the per-app stores from its own record of the running app, so changing
them grants nothing.

---

## App Requirements

Apps can request elevated requirements via the `requirements` array in `app.json`:

```json
{
  "requirements": ["root-filesystem"]
}
```

### Available Requirements

| Requirement | Description |
|------------|-------------|
| `filesystem` | Default sandbox access: read `/apps/<dir>/` and `/system/lib/`, read/write `/data/<appid>/` |
| `root-filesystem` | Full SD card read/write access (bypasses sandbox, though relative paths and `..` are still refused) |
| `http` | App needs WiFi/network connectivity (WiFi stays connected after boot time sync) |
| `audio` | App needs audio output |
| `clipboard` | Reserved for future use |
| `sysconfig` | `picocalc.sysconfig` (system-wide config) is registered; without it `picocalc.sysconfig` is nil. `get("wifi_pass")` always returns nil (write-only) |
| `system-update` | `picocalc.sys.applyUpdate` is registered — only for OS apps (id `net.picodeck.updater` / `net.picodeck.store`, or an app under `/system/`) |

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

This applies to Lua apps. A native app's `api->fs` goes straight to the card: it has full access whatever its requirements say.

Without `root-filesystem`, the filesystem API restricts access to:
- **Read**: `/apps/<dir>/` (your app's directory, by its directory name) and `/system/lib/`
- **Read/Write**: `/data/<appid>/` (auto-created app data directory)

Relative paths and any `..` are refused. The same check guards the image,
font and zip loaders, the sound loaders (`sound.sample`/`sampleplayer` paths,
`sample:load`, `sample:save`, `fileplayer:load`, `mp3player:load`),
`modplayer:load`, `video:load` and `crypto.sha256File`; a refused call returns
`false, "access denied"` (or `nil, "access denied"`). `fs.browse(start)` falls
back to your data directory when `start` is not readable.

### App ids

The `id` in `app.json` names your data directory (`/data/<id>`), so it must be
1-79 characters of `[A-Za-z0-9._-]`, with no `..` and no trailing `.`. It is
folded to lower case (the SD card ignores case), so `com.Example.App` and
`com.example.app` share one directory and `APP_ID` reads `com.example.app` for
both. The launcher refuses an app with an invalid id, a longer one included
(on-screen reason and an `/system/error.log` entry). A missing id defaults to
`local.<dir>`.

Use `picocalc.fs.appPath("filename")` to get paths within your app's data directory.
