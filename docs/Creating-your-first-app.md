---
title: "Creating your first app"
---

## Your First App

Creating an app is incredibly straightforward. You only need two files in a folder of your own, at `/apps/<dir>/` on the SD card. Call this one `myapp`.

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
local gp = pc.gamepad

-- Main app loop
while true do
    -- Read the keyboard and the gamepad once per frame
    pc.input.update()

    -- B (Esc on the keyboard) exits back to the launcher
    if gp.getNavPressed() & gp.PAD_B ~= 0 then
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
./picodeck-simulator-linux --sd-card mycard --launch myapp
```

That is the name of the [downloaded simulator](Simulator-and-Testing.md); one you built is `build_sim/picodeck_simulator`.

**On a device:** copy the `myapp` folder into `apps/` on the card. With the device at the launcher, the system menu's **USB Disk Mode** makes the card a USB drive, so it can stay in the slot; [Getting started on a device](Getting-started-on-a-device.md#5-copy-your-own-apps) has the steps. Then pick the app in the launcher: with no `category` it is under Demos.

Either way, it draws "Hello, World!", and Esc (or B on a game controller) returns to the launcher.

Keep the `input.update()` call even in a loop that ignores the keys. It, or `picocalc.sys.sleep()`, is what reads the keyboard, so it is also what opens the system menu: on a device, a loop that calls neither can only be left by switching it off.

### Pacing

The loop needs no `sleep`: `flush()` waits for the previous frame's transfer to the panel (about 16.4 ms for a full frame), which already limits a simple app to roughly 40-60 frames per second. To hold a lower rate, such as 30 FPS to save battery, bracket each frame with `picocalc.perf.beginFrame()` and `picocalc.perf.endFrame()` and call `picocalc.perf.setTargetFPS(30)` once; see [API Performance](API-Performance.md). `picocalc.sys.sleep(ms)` is for waiting, not for pacing. A screen that only a key changes should not redraw at all while it waits: see [Writing Fast Apps](Writing-Fast-Apps.md).

### Menus and lists

Read a menu's moves, confirm and back from [`picocalc.gamepad.getNavPressed()`](API-Gamepad.md#picocalcgamepadgetnavpressed). One call drives the menu from the keyboard (arrows, Enter, Esc), from keys the player rebinds in Settings > Controls, and from a Bluetooth controller: it returns the D-pad with auto-repeat while a direction is held, the other buttons' presses, and A for Enter and B for Esc. Name keys in on-screen hints with `getLabel()`, which follows a rebind. Keep typed text on the keyboard (`getChar()`, or `picocalc.ui.textInput()`, which a controller can only accept or cancel).

```lua
local pc = picocalc
local gp = pc.gamepad
local items = {"New game", "Options", "Quit"}
local sel = 1

while true do
    pc.input.update()
    local nav = gp.getNavPressed()
    if nav & gp.PAD_UP ~= 0 then sel = math.max(1, sel - 1) end
    if nav & gp.PAD_DOWN ~= 0 then sel = math.min(#items, sel + 1) end
    if nav & gp.PAD_B ~= 0 then return end                  -- B or Esc: back
    if nav & gp.PAD_A ~= 0 and items[sel] == "Quit" then return end

    pc.display.clear(pc.display.BLACK)
    for i, item in ipairs(items) do
        local color = (i == sel) and pc.display.YELLOW or pc.display.WHITE
        pc.display.drawText(40, 100 + i * 16, item, color)
    end
    local a = gp.getLabel(gp.PAD_A) or "?"
    pc.display.drawText(4, 300, a .. ": select  Esc: back", pc.display.GRAY)
    pc.display.flush()
    pc.sys.sleep(16)  -- nothing moves on its own: don't spin
end
```

`getNavPressed()` is new in firmware 0.7.0: set `"min_firmware": "0.7.0"` in the `app.json` of an app that uses it, or check `if gp and gp.getNavPressed then` to support older firmware. [`apps/hello`](https://github.com/PicoDeck/picodeck/tree/master/apps/hello) is a complete example.

### Reading game controls

For a game, read the logical gamepad, `picocalc.gamepad`, instead of raw keys. Its buttons (D-pad, A, B, X, Y, L, R, Start, Select) are aliases for keys that players can rebind in the system menu under Settings > Controls, so your game gets rebinding without any code for it. Use the D-pad for movement, A for the main action or to confirm, B for the second action or to go back, and Start to pause. Keep Esc on `picocalc.input` for quitting (`getNavPressed()` reports Esc as B, and in a game B is an action), and keep text entry on the keyboard.

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

### Editor setup

An editor with [lua-language-server](https://luals.github.io/) (the "Lua" extension in VS Code) can complete and check the whole `picocalc` API. Copy [`sdk/lua/picocalc.lua`](https://github.com/PicoDeck/picodeck/blob/master/sdk/lua/picocalc.lua) into your project as `libs/picocalc.lua`, and put the `.luarc.json` from the [SDK README](https://github.com/PicoDeck/picodeck/blob/master/sdk/README.md#editor-setup-for-lua) next to `app.json`. Neither runs on the device, so [leave them out of the zip](Publishing-Apps.md#quick-version) when you publish.

### Add an icon (optional)

Put an `icon.png` (or `icon.bmp`) next to `app.json`. The launcher draws it at 24×24 pixels beside the app's name. Other sizes are scaled to fit with nearest-neighbour sampling, so draw at 24×24, or at an exact multiple such as 48×48, to keep the pixels crisp. Icons are drawn opaque: transparent PNG pixels come out black, so fill the whole square. The built-in icons use a dark navy background (RGB 12, 16, 48) behind their artwork. Without an icon, the launcher draws a game cartridge in the colour of the app's [category](App-Json-Reference.md#category), with the first letter of the app's name on its label.

### Sample apps

Every first-party app is a working example: in the [`apps/`](https://github.com/PicoDeck/picodeck/tree/master/apps) folder of the PicoDeck repository, or in its own repository for the ones the App Store installs. These are the ones to read first. All of them are Lua apps unless marked, and all run in the simulator.

| App | Shows |
|---|---|
| [`hello`](https://github.com/PicoDeck/picodeck/tree/master/apps/hello) | The smallest drawing demo |
| [`snake`](https://github.com/PicoDeck/picodeck/tree/master/apps/snake) | A complete small game |
| [`minesweeper`](https://github.com/PicoDeck/minesweeper) | A complete grid game |
| [`platformer_demo`](https://github.com/PicoDeck/picodeck/tree/master/apps/platformer_demo) | The [game framework](API-Game.md): camera, scenes, saves |
| [`guinea_pig`](https://github.com/PicoDeck/picodeck/tree/master/apps/guinea_pig) | A bigger game, with art and sound |
| [`3dtest`](https://github.com/PicoDeck/picodeck/tree/master/apps/3dtest) | A spinning solid drawn with the display primitives, and an FPS counter |
| [`graphicstest`](https://github.com/PicoDeck/picodeck/tree/master/apps/graphicstest) | Image loading and drawing |
| [`fonttest`](https://github.com/PicoDeck/picodeck/tree/master/apps/fonttest) | Built-in fonts and a loaded `.pfn` |
| [`panels_demo`](https://github.com/PicoDeck/picodeck/tree/master/apps/panels_demo) | The [Panels](Library-Panels.md) library, from `/system/lib/panels.lua`, which the firmware writes to the card |
| [`terminal_example`](https://github.com/PicoDeck/picodeck/tree/master/apps/terminal_example) | The [terminal](API-Terminal.md) widget |
| [`calculator`](https://github.com/PicoDeck/calculator) | An app split into modules with [`require`](Standard-Lua-Libraries.md#requirename) |
| [`editor`](https://github.com/PicoDeck/editor), [`filemanager`](https://github.com/PicoDeck/filemanager) | Tools with the `root-filesystem` [requirement](Global-Variables-and-Permissions.md#app-requirements) |
| [`llmchat`](https://github.com/PicoDeck/picodeck/tree/master/apps/llmchat), [`wikipedia`](https://github.com/PicoDeck/picodeck/tree/master/apps/wikipedia) | Network apps with the `http` requirement |
| [`systest`](https://github.com/PicoDeck/picodeck/tree/master/apps/systest) | A tour of the system APIs |
| [`sdk/native`](https://github.com/PicoDeck/picodeck/tree/master/sdk/native) | **Native, C.** The template to [start a native app](Native-Loading.md) from |

### Prefer C?

If you want to build high-performance apps or prefer working in C (or a subset of C++), check out our guide on [Native App Development](Native-Loading.md).
