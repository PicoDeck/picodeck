---
title: "Lua SDK Reference"
---

This page lists every Lua API available to PicoDeck apps, with a link to its reference. All of them hang off the `picocalc` global, except the standard libraries.

## API Sections

- [Global Variables and Permissions](Global-Variables-and-Permissions.md) — `APP_DIR` and friends, requirements, the sandbox
- [`app.json` Reference](App-Json-Reference.md) — every manifest field
- [API 3D](API-3D.md) — Flat-shaded 3D meshes, camera, light, fog, sky and sprites (`picocalc.gfx3d`)
- [API Audio and Sound](API-Audio-and-Sound.md) — Audio Playback (WAV, MP3, QOA) & Tones
- [API Chargrid](API-Chargrid.md) — Character-cell grids: text, shapes, palettes and ramps drawn in one call (`picocalc.chargrid`), and deterministic noise (`picocalc.noise`)
- [API Crypto](API-Crypto.md) — Cryptographic Primitives
- [API Display and Graphics](API-Display-and-Graphics.md) — Graphics & Display
- [API Filesystem](API-Filesystem.md) — Filesystem (SD Card)
- [API Game](API-Game.md) — The game framework: scenes, camera, save slots, object pools (`picocalc.game`)
- [API Gamepad](API-Gamepad.md) — Rebindable Gamepad Buttons, Analog Sticks and Triggers (`picocalc.gamepad`)
- [API Input](API-Input.md) — Keyboard & Button Input
- [API JSON](API-JSON.md) — Encode and decode JSON (`picocalc.json`)
- [API Modplayer](API-Modplayer.md) — Tracker Module Music (MOD, XM, S3M)
- [API Network and WiFi](API-Network-and-WiFi.md) — WiFi & HTTP Client
- [API Performance](API-Performance.md) — Performance Monitoring
- [API REPL](API-Repl.md) — A scrolling text console with line input (it does not evaluate Lua)
- [API Sysconfig](API-Sysconfig.md) — System-Wide Configuration
- [API System and Config](API-System-and-Config.md) — System Functions & Per-App Config
- [API TCP](API-TCP.md) — Raw TCP and TLS sockets (`picocalc.tcp`)
- [API Terminal](API-Terminal.md) — Terminal Emulator Widget
- [API UI](API-UI.md) — Standard UI Components
- [API Video](API-Video.md) — MJPEG Video Playback
- [API Zip](API-Zip.md) — Read and extract ZIP archives (`picocalc.zip`)
- [Standard Lua Libraries](Standard-Lua-Libraries.md) — What Lua's standard library has here, and `require`
- [Library Download](Library-Download.md) — Stream an HTTP(S) download to a file (`download.lua`)
- [Library Panels](Library-Panels.md) — Interactive comics (`panels.lua`)

---

## Example: Complete Game Loop

```lua
-- Initialize
local x, y = 160, 160
local color = picocalc.display.WHITE

-- Main loop
while true do
    picocalc.perf.beginFrame()
    picocalc.input.update()
    
    -- Handle input
    local pressed = picocalc.input.getButtonsPressed()
    if pressed & picocalc.input.BTN_UP ~= 0 then y = y - 5 end
    if pressed & picocalc.input.BTN_DOWN ~= 0 then y = y + 5 end
    if pressed & picocalc.input.BTN_ESC ~= 0 then return end
    
    -- Draw
    picocalc.display.clear(picocalc.display.BLACK)
    picocalc.display.fillRect(x - 10, y - 10, 20, 20, color)
    picocalc.perf.drawFPS()
    picocalc.display.flush()
    
    picocalc.perf.endFrame()  -- flush() already paces the loop; see perf.setTargetFPS
end
```

---

## Notes

- The display uses a **double-buffered framebuffer in SRAM** (2× 200 KB). DMA flushes run in the background while the CPU draws the next frame.
- Call `picocalc.input.update()` and `picocalc.display.flush()` **once per frame**.
- The system menu key (Shift+F5, reported as F10) is automatically intercepted by the OS to show the system menu overlay; pressing it while the menu is open closes the menu again. Keys down when the menu opens or closes stay hidden from your app until they are released ([`input.update()`](API-Input.md)).
- All file paths must be absolute (e.g., `APP_DIR .. "/data.txt"` to read a file your app ships).
- Use `picocalc.fs.appPath("filename")` for per-app data storage — it auto-creates the directory.
- Without the `root-filesystem` requirement, file access is sandboxed: read your app's directory and `/system/lib/`, read and write `/data/<APP_ID>/` ([details](Global-Variables-and-Permissions.md#default-sandbox)).
- Per-app config (`picocalc.config`, also available as `picocalc.appconfig`) is isolated per app at `/data/<APP_ID>/config.json`. Only `picocalc.sysconfig` (`/system/config.json`) is shared across all apps.

---

### Native App Development

If you prefer to write apps in C or C++ instead of Lua, see the [Native App Development](Native-Loading.md) guide. The C API mirrors the Lua modules documented here.

