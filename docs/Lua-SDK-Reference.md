---
title: "Lua SDK Reference"
---

This document provides a complete reference for all Lua APIs available to PicoDeck apps.

## API Sections

- [Global Variables and Permissions](Global-Variables-and-Permissions.md)
- [API 3D](API-3D.md) — Flat-shaded 3D meshes, camera, light, fog, sky and sprites (`picocalc.gfx3d`)
- [API Audio and Sound](API-Audio-and-Sound.md) — Audio Playback (WAV, MP3) & Tones
- [API Crypto](API-Crypto.md) — Cryptographic Primitives
- [API Display and Graphics](API-Display-and-Graphics.md) — Graphics & Display
- [API Filesystem](API-Filesystem.md) — Filesystem (SD Card)
- [API Gamepad](API-Gamepad.md) — Rebindable Gamepad Buttons (`picocalc.gamepad`)
- [API Input](API-Input.md) — Keyboard & Button Input
- [API Modplayer](API-Modplayer.md) — Tracker Module Music (MOD, XM, S3M)
- [API Network and WiFi](API-Network-and-WiFi.md) — WiFi & HTTP Client
- [API Performance](API-Performance.md) — Performance Monitoring
- [API REPL](API-Repl.md) — Interactive Lua REPL
- [API Sysconfig](API-Sysconfig.md) — System-Wide Configuration
- [API System and Config](API-System-and-Config.md) — System Functions & Per-App Config
- [API Terminal](API-Terminal.md) — Terminal Emulator Widget
- [API UI](API-UI.md) — Standard UI Components
- [API Video](API-Video.md) — MJPEG Video Playback
- [Standard Lua Libraries](Standard-Lua-Libraries.md)

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
- The Menu key (F10) is automatically intercepted by the OS to show the system menu overlay; pressing it while the menu is open closes the menu again.
- All file paths must be absolute (e.g., `"/apps/myapp/data.txt"` or `APP_DIR .. "/data.txt"`).
- Use `picocalc.fs.appPath("filename")` for per-app data storage — it auto-creates the directory.
- Without `root-filesystem` permission, file access is sandboxed to your app's directory and `/data/<appid>/`.
- Per-app config (`picocalc.config`, also available as `picocalc.appconfig`) is isolated per app at `/data/<APP_ID>/config.json`. Only `picocalc.sysconfig` (`/system/config.json`) is shared across all apps.

---

### Native App Development

If you prefer to write apps in C or C++ instead of Lua, see the [Native App Development](Native-Loading.md) guide. The C API mirrors the Lua modules documented here.

