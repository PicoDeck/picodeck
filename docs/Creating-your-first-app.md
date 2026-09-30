---
title: "Creating your first app"
---

## Your First App

Creating an app is incredibly straightforward. You only need two files in a directory inside `/apps/` on the SD Card:

1. `app.json`: A simple file describing your app.

```
1     {
2         "name": "My Awesome App",
3         "description": "The best app ever.",
4         "version": "1.0"
5     }
```

2. `main.lua`: The entry point for your code, which runs in a simple loop.

```
 1     -- The 'picocalc' global is your gateway to the hardware
 2     local pc = picocalc
 3 
 4     -- Main app loop
 5     while true do
 6         -- Read the keyboard once per frame
 7         pc.input.update()
 8 
 9         -- Check for the ESC key to exit back to the launcher
10         if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then
11             return
12         end
13 
14         -- Drawing
15         pc.display.clear(pc.display.BLACK)
16         pc.display.drawText(100, 150, "Hello, World!", pc.display.WHITE)
17         pc.display.flush() -- Push your changes to the screen
18     end
```

The loop needs no `sleep`: `flush()` waits for the previous frame's transfer to the panel (about 16 ms for a full frame), which already limits a simple app to roughly 40-60 frames per second. To hold a lower rate, such as 30 FPS to save battery, bracket each frame with `picocalc.perf.beginFrame()` and `picocalc.perf.endFrame()` and call `picocalc.perf.setTargetFPS(30)` once; see [API Performance](API-Performance.md). `picocalc.sys.sleep(ms)` is for waiting, not for pacing.

### Reading game controls

Apps that only need Esc read the keyboard masks as above. For a game, read the logical gamepad, `picocalc.gamepad`, instead of raw keys. Its buttons (D-pad, A, B, X, Y, L, R, Start, Select) are aliases for keys that players can rebind in the system menu under Settings > Controls, so your game gets rebinding without any code for it. Use the D-pad for movement, A for the main action or to confirm, B for the second action or to go back, and Start to pause. Keep Esc on `picocalc.input` for quitting, and keep text entry on the keyboard.

```lua
local pc = picocalc
local pad = pc.gamepad   -- nil on firmware older than API version 9
local x = 160

while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end

    if pad then
        if pad.getButtons() & pad.PAD_LEFT ~= 0 then x = x - 2 end
        if pad.getButtons() & pad.PAD_RIGHT ~= 0 then x = x + 2 end
    else
        -- Older firmware: read the keys directly.
        if pc.input.getButtons() & pc.input.BTN_LEFT ~= 0 then x = x - 2 end
        if pc.input.getButtons() & pc.input.BTN_RIGHT ~= 0 then x = x + 2 end
    end

    pc.display.clear(pc.display.BLACK)
    pc.display.fillRect(x - 4, 150, 8, 8, pc.display.WHITE)
    -- Name the key from getLabel() so the hint stays right after a rebind.
    local key = pad and pad.getLabel(pad.PAD_A) or "Enter"
    pc.display.drawText(4, 4, "Press " .. key .. " to jump", pc.display.WHITE)
    pc.display.flush()
end
```

See [API Gamepad](API-Gamepad.md) for the buttons, the default keys and `getLabel()`.

### Add an icon (optional)

Put an `icon.png` (or `icon.bmp`) next to `app.json`. The launcher draws it at 24×24 pixels beside the app's name. Other sizes are scaled to fit with nearest-neighbour sampling, so draw at 24×24, or at an exact multiple such as 48×48, to keep the pixels crisp. Icons are drawn opaque: transparent PNG pixels come out black, so fill the whole square. The built-in icons use a dark navy background (RGB 12, 16, 48) behind their artwork. Without an icon, the launcher draws a game cartridge in the app's category colour, with the first letter of the app's name on its label.

### Prefer C or C++?

If you want to build high-performance apps or prefer working in C/C++, check out our guide on [Native App Development](Native-Loading.md).