---
title: "Creating your first app"
---

## Your First App

Creating an app is incredibly straightforward. You only need two files in a folder of your own, at `/apps/<name>/` on the SD card. Call this one `myapp`.

1. `app.json`: A simple file describing your app. The [`app.json` reference](App-Json-Reference.md) has every field.

```json
{
    "id": "com.example.myapp",
    "name": "My Awesome App",
    "description": "The best app ever.",
    "version": "1.0"
}
```

The `id` names your app's data folder, so pick one and keep it: reverse-DNS, in lower case, is what the App Store accepts when you [publish](Publishing-Apps.md).

2. `main.lua`: The entry point for your code, which runs in a simple loop.

```lua
-- The 'picocalc' global is your gateway to the hardware
local pc = picocalc

-- Main app loop
while true do
    -- Read the keyboard once per frame
    pc.input.update()

    -- Check for the ESC key to exit back to the launcher
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then
        return
    end

    -- Drawing
    pc.display.clear(pc.display.BLACK)
    pc.display.drawText(100, 150, "Hello, World!", pc.display.WHITE)
    pc.display.flush() -- Push your changes to the screen
end
```

### Run it

**On your computer:** make a folder to stand in for the card, put your app in its `apps/` folder, and point the [simulator](Simulator-and-Testing.md) at it (the folder, not `apps/`).

```
mycard/
└── apps/
    └── myapp/
        ├── app.json
        └── main.lua
```

```sh
./picodeck_simulator --sd-card mycard --launch myapp
```

**On a device:** copy the `myapp` folder into `apps/` on the card. With the device at the launcher, the system menu's **USB Disk Mode** makes the card a USB drive, so it can stay in the slot; [Getting started on a device](Getting-started-on-a-device.md#5-copy-your-own-apps) has the steps. Then pick the app in the launcher: with no `category` it is under Demos.

Either way, it draws "Hello, World!" and Esc returns to the launcher.

### Pacing

The loop needs no `sleep`: `flush()` waits for the previous frame's transfer to the panel (about 16 ms for a full frame), which already limits a simple app to roughly 40-60 frames per second. To hold a lower rate, such as 30 FPS to save battery, bracket each frame with `picocalc.perf.beginFrame()` and `picocalc.perf.endFrame()` and call `picocalc.perf.setTargetFPS(30)` once; see [API Performance](API-Performance.md). `picocalc.sys.sleep(ms)` is for waiting, not for pacing.

### Reading game controls

Apps that only need Esc read the keyboard masks as above. For a game, read the logical gamepad, `picocalc.gamepad`, instead of raw keys. Its buttons (D-pad, A, B, X, Y, L, R, Start, Select) are aliases for keys that players can rebind in the system menu under Settings > Controls, so your game gets rebinding without any code for it. Use the D-pad for movement, A for the main action or to confirm, B for the second action or to go back, and Start to pause. Keep Esc on `picocalc.input` for quitting, and keep text entry on the keyboard.

```lua
local pc = picocalc
local pad = pc.gamepad   -- nil on firmware older than API version 9
local x, lit = 160, false

while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end

    local left, right, action
    if pad then
        local held = pad.getButtons()
        left = held & pad.PAD_LEFT ~= 0
        right = held & pad.PAD_RIGHT ~= 0
        action = pad.getButtonsPressed() & pad.PAD_A ~= 0
    else
        -- Older firmware: read the keys directly.
        local held = pc.input.getButtons()
        left = held & pc.input.BTN_LEFT ~= 0
        right = held & pc.input.BTN_RIGHT ~= 0
        action = pc.input.getButtonsPressed() & pc.input.BTN_ENTER ~= 0
    end
    if left then x = x - 2 end
    if right then x = x + 2 end
    if action then lit = not lit end

    pc.display.clear(pc.display.BLACK)
    pc.display.fillRect(x - 4, 150, 8, 8, lit and pc.display.YELLOW or pc.display.WHITE)
    -- Name the key from getLabel() so the hint stays right after a rebind
    -- (nil when the player left A unbound).
    local key = "Enter"
    if pad then key = pad.getLabel(pad.PAD_A) or "?" end
    pc.display.drawText(4, 4, "Press " .. key .. " to light up", pc.display.WHITE)
    pc.display.flush()
end
```

A is F4 until the player rebinds it, and Enter is not bound by default. The built-in games also take Enter on their menus and end screens, alongside A, whenever Enter pressed no gamepad button that frame (`pad.getButtonsPressed() == 0`), so a player who binds Enter to a button gets only that button.

See [API Gamepad](API-Gamepad.md) for the buttons, the default keys and `getLabel()`.

### Add an icon (optional)

Put an `icon.png` (or `icon.bmp`) next to `app.json`. The launcher draws it at 24×24 pixels beside the app's name. Other sizes are scaled to fit with nearest-neighbour sampling, so draw at 24×24, or at an exact multiple such as 48×48, to keep the pixels crisp. Icons are drawn opaque: transparent PNG pixels come out black, so fill the whole square. The built-in icons use a dark navy background (RGB 12, 16, 48) behind their artwork. Without an icon, the launcher draws a game cartridge in the colour of the app's [category](App-Json-Reference.md#category), with the first letter of the app's name on its label.

### Sample apps

Every first-party app is a working example in the [`apps/`](https://github.com/PicoDeck/picodeck/tree/master/apps) folder of the PicoDeck repository. These are the ones to read first. All of them are Lua apps unless marked, and all run in the simulator.

| App | Shows |
|---|---|
| [`hello`](https://github.com/PicoDeck/picodeck/tree/master/apps/hello) | The smallest drawing demo |
| [`snake`](https://github.com/PicoDeck/picodeck/tree/master/apps/snake) | A complete small game |
| [`minesweeper`](https://github.com/PicoDeck/picodeck/tree/master/apps/minesweeper) | A complete grid game |
| [`platformer_demo`](https://github.com/PicoDeck/picodeck/tree/master/apps/platformer_demo) | The [game framework](API-Game.md): camera, scenes, saves |
| [`guinea_pig`](https://github.com/PicoDeck/picodeck/tree/master/apps/guinea_pig) | A bigger game, with art and sound |
| [`3dtest`](https://github.com/PicoDeck/picodeck/tree/master/apps/3dtest) | A spinning solid drawn with the display primitives, and an FPS counter |
| [`graphicstest`](https://github.com/PicoDeck/picodeck/tree/master/apps/graphicstest) | Image loading and drawing |
| [`fonttest`](https://github.com/PicoDeck/picodeck/tree/master/apps/fonttest) | Built-in fonts and a loaded `.pfn` |
| [`panels_demo`](https://github.com/PicoDeck/picodeck/tree/master/apps/panels_demo) | The [Panels](Library-Panels.md) library. Needs `/system/lib/panels.lua` on the card |
| [`terminal_example`](https://github.com/PicoDeck/picodeck/tree/master/apps/terminal_example) | The [terminal](API-Terminal.md) widget |
| [`calculator`](https://github.com/PicoDeck/picodeck/tree/master/apps/calculator) | An app split into modules with [`require`](Standard-Lua-Libraries.md#requirename) |
| [`editor`](https://github.com/PicoDeck/picodeck/tree/master/apps/editor), [`filemanager`](https://github.com/PicoDeck/picodeck/tree/master/apps/filemanager) | Tools with the `root-filesystem` [requirement](Global-Variables-and-Permissions.md#app-requirements) |
| [`llmchat`](https://github.com/PicoDeck/picodeck/tree/master/apps/llmchat), [`wikipedia`](https://github.com/PicoDeck/picodeck/tree/master/apps/wikipedia) | Network apps with the `http` requirement |
| [`systest`](https://github.com/PicoDeck/picodeck/tree/master/apps/systest) | A tour of the system APIs |
| [`sdk/native`](https://github.com/PicoDeck/picodeck/tree/master/sdk/native) | **Native, C.** The template to [start a native app](Native-Loading.md) from |

### Prefer C or C++?

If you want to build high-performance apps or prefer working in C/C++, check out our guide on [Native App Development](Native-Loading.md).
