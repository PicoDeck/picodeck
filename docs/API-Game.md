---
title: "API — Game Framework"
---

Game-framework helpers: a 2D camera, a scene manager, and per-app persistent save files. Three sub-namespaces: `picocalc.game.camera`, `picocalc.game.scene`, and `picocalc.game.save`.

## picocalc.game.camera

A 2D camera with position, zoom, target following, world bounds, and screen shake. It draws nothing and owns nothing on screen: you ask it where a world point lands, with `getOffset` or `worldToScreen`, and draw there yourself. The sprite manager (`picocalc.graphics.sprite`) does not read a camera.

The camera's position is the world point at the **centre** of the screen, so a new camera at (0, 0) shows world (0, 0) at screen (160, 160):

```
screen = (world - position) * zoom + 160 + shake
```

### Functions

#### `picocalc.game.camera.new()`
Create a new camera, positioned at (0, 0) with zoom 1.0 and no target, bounds or shake.

- **Parameters:** None
- **Returns:** (userdata) PicoDeckCamera object

```lua
local camera = picocalc.game.camera.new()
```

---

### Camera Methods

Objects returned by `picocalc.game.camera.new()`.

#### `camera:setPosition(x, y)`
Set the camera position: the world point to show at the centre of the screen.

- **Parameters:**
  - `x` (number): World X coordinate
  - `y` (number): World Y coordinate
- **Returns:** None

```lua
camera:setPosition(160, 160)  -- world (0, 0) at the screen's top-left corner
```

---

#### `camera:move(dx, dy)`
Move the camera by a relative offset.

- **Parameters:**
  - `dx` (number): X delta
  - `dy` (number): Y delta
- **Returns:** None

```lua
camera:move(4, 0)  -- pan right
```

---

#### `camera:getPosition()`
Get the camera position in world coordinates.

- **Parameters:** None
- **Returns:** (number, number) World `x, y` coordinates

```lua
local x, y = camera:getPosition()
```

---

#### `camera:setZoom(zoom)`
Set the camera zoom factor. 1.0 is unscaled. It scales what `getOffset` and the conversions return: the drawing, and any scaling of it, is yours.

- **Parameters:**
  - `zoom` (number): Zoom factor, clamped to 0.1-10
- **Returns:** None

```lua
camera:setZoom(2.0)  -- 2x zoom
```

---

#### `camera:getZoom()`
Get the current zoom factor.

- **Parameters:** None
- **Returns:** (number) Current zoom factor

---

#### `camera:setTarget(x, y [, lag])`
Follow a world position: `camera:update(dt)` eases the camera towards it, covering about 63% of the distance every `lag` seconds. Call it again each frame to track a moving object.

- **Parameters:**
  - `x`, `y` (number): World position to follow
  - `lag` (number, optional): Smoothing time constant in seconds (default 0.15)
- **Returns:** None

```lua
camera:setTarget(player.x, player.y)
```

---

#### `camera:clearTarget()`
Stop following the current target.

- **Parameters:** None
- **Returns:** None

---

#### `camera:setBounds(minX, minY, maxX, maxY)`
Constrain the camera to a world rectangle: `camera:update(dt)` moves it back so that the screen shows nothing outside the rectangle. `setPosition` and `move` are not checked until then, and shake is added after. The arguments are two corners, not a size. A rectangle smaller than the view lines up with its right and bottom edges.

- **Parameters:**
  - `minX` (number): Left edge of the world
  - `minY` (number): Top edge of the world
  - `maxX` (number): Right edge of the world
  - `maxY` (number): Bottom edge of the world
- **Returns:** None

```lua
camera:setBounds(0, 0, 1024, 1024)
```

---

#### `camera:clearBounds()`
Remove the world bounds constraint.

- **Parameters:** None
- **Returns:** None

---

#### `camera:getBounds()`
Get the current world bounds.

- **Parameters:** None
- **Returns:** (number, number, number, number) `minX, minY, maxX, maxY`, or nothing at all when the camera is unbounded

---

#### `camera:shake(amplitude, duration)`
Shake the camera on both axes. The shake is a random offset, fading out over `duration`, chosen by each `camera:update(dt)` and included in `getOffset` and the conversions. A new shake replaces the one running.

- **Parameters:**
  - `amplitude` (number): Shake intensity in pixels
  - `duration` (number): Shake duration, counted down by the `dt` given to `camera:update(dt)` (seconds when `dt` is)
- **Returns:** None

```lua
camera:shake(4, 0.3)  -- rumble for 0.3 s
```

---

#### `camera:shakeX(amplitude, duration)`
Shake the camera horizontally only.

- **Parameters:**
  - `amplitude` (number): Shake intensity in pixels
  - `duration` (number): Shake duration in `camera:update(dt)` units (seconds)
- **Returns:** None

---

#### `camera:shakeY(amplitude, duration)`
Shake the camera vertically only.

- **Parameters:**
  - `amplitude` (number): Shake intensity in pixels
  - `duration` (number): Shake duration in `camera:update(dt)` units (seconds)
- **Returns:** None

---

#### `camera:stopShake()`
Stop any active shake immediately.

- **Parameters:** None
- **Returns:** None

---

#### `camera:worldToScreen(wx, wy)`
Convert world coordinates to screen coordinates.

- **Parameters:**
  - `wx` (number): World X coordinate
  - `wy` (number): World Y coordinate
- **Returns:** (integer, integer) Screen `sx, sy` coordinates, whole pixels (truncated toward zero)

```lua
local sx, sy = camera:worldToScreen(player.x, player.y)
```

---

#### `camera:screenToWorld(sx, sy)`
Convert screen coordinates to world coordinates.

- **Parameters:**
  - `sx` (number): Screen X coordinate
  - `sy` (number): Screen Y coordinate
- **Returns:** (number, number) World `wx, wy` coordinates

---

#### `camera:update(dt)`
Advance target following, apply the bounds and advance screen shake. Call once per frame.

- **Parameters:**
  - `dt` (number): Seconds since the last update (required)
- **Returns:** None

```lua
camera:update(1 / 30)
```

---

#### `camera:getOffset()`
Get the screen position of world (0, 0), with the camera position, zoom and shake folded in: `screen = world * zoom + offset`. The values are whole pixels (truncated toward zero).

- **Parameters:** None
- **Returns:** (integer, integer) Offset `ox, oy`

```lua
local ox, oy = camera:getOffset()
local z = camera:getZoom()
picocalc.display.fillRect(player.x * z + ox, player.y * z + oy, 8, 8, 0xFFFF)
```

---

## picocalc.game.scene

A scene manager: a stack of named scenes, of which the top one runs. Nothing drives it for you: your main loop calls `scene.update(dt)` and `scene.draw()`, polls the input and presents the frame.

A scene is a plain table with up to four functions, any of which can be left out: `update(dt)`, `draw()`, `enter()` and `exit()`.

**They are called as plain functions, with no `self`.** Write `function play.update(dt)`, not `function play:update(dt)`: with the colon, `self` receives `dt` and `dt` is nil. Keep a scene's state in local variables, or reach its table by name.

```lua
local play = {}
function play.enter() play.t = 0 end
function play.update(dt) play.t = play.t + dt end
function play.draw()
    picocalc.display.clear(0)
    picocalc.display.drawText(4, 4, string.format("%.1f s", play.t), 0xFFFF)
end
picocalc.game.scene.add("play", play)
picocalc.game.scene.switch("play")
```

**An error in a scene function stops the app,** like any other Lua error: the player sees the error screen and it goes in `/system/error.log`. To carry on after one, call `scene.update` or `scene.draw` inside `pcall`.

**Nothing runs when the player exits.** Exit App in the system menu stops the app on the spot: no scene's `exit()`, no code after your main loop and no `<close>` handler runs (the one exception is the callback of a menu option the player just changed, `sys.addMenuOption`). `exit()` runs only when `switch` or `pop` leaves a scene. So save when the data changes, not on the way out.

Limits: 16 registered scenes, a stack 4 deep and names of up to 63 bytes. Going past any of them raises an error.

### Functions

#### `picocalc.game.scene.new()`
Create a new, empty scene: the same as writing `{}`.

- **Parameters:** None
- **Returns:** (table) An empty table to put the scene's functions in

---

#### `picocalc.game.scene.add(name, scene)`
Register a scene under a name. Adding a name that exists replaces the scene it stands for, on the stack too, and calls neither `exit()` nor `enter()`.

- **Parameters:**
  - `name` (string): Scene name
  - `scene` (table): Scene table with its functions
- **Returns:** None

```lua
picocalc.game.scene.add("menu", menuScene)
```

---

#### `picocalc.game.scene.remove(name)`
Remove a registered scene, and take it off the stack if it is there. Its `exit()` does not run, and the scene below it becomes current without an `enter()`. An unknown name does nothing.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** None

---

#### `picocalc.game.scene.has(name)`
Check whether a scene is registered.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** (boolean) `true` if the scene exists

---

#### `picocalc.game.scene.switch(name)`
Switch to another scene. Calls `exit()` on the current scene, empties the stack (a scene below the top gets no `exit()`), then calls `enter()` on the next. An unknown name raises an error. Start your app with this: until the first `switch` no scene is current.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** None

```lua
picocalc.game.scene.switch("play")
```

---

#### `picocalc.game.scene.push(name)`
Put a scene on top of the stack and call its `enter()`. The scene below gets no `exit()` and stays on the stack, but it stops running: only the top scene updates and draws. For a pause menu over the game, call the game's own draw function from the menu's `draw`, as the [example](#example) does.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** None

```lua
picocalc.game.scene.push("pause")
```

---

#### `picocalc.game.scene.pop()`
Leave the top scene and return to the one below. Calls `exit()` on the top scene, then `enter()` on the revealed one **again**, so an `enter()` that resets a game also resets it on the way back from a pause menu: put first-time setup somewhere else. Popping the last scene raises an error.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.game.scene.getCurrent()`
Get the name of the current scene, the top of the stack.

- **Parameters:** None
- **Returns:** (string or nil) The name it was added under, as `switch` takes it, or `nil` if no scene is current

---

#### `picocalc.game.scene.update([dt])`
Call `update(dt)` on the current scene. Call once per frame. With no scene current, or none with an `update`, it does nothing.

- **Parameters:**
  - `dt` (number, optional): Passed on to the scene, normally the seconds since the last frame (default 0.016)
- **Returns:** None

---

#### `picocalc.game.scene.draw()`
Call `draw()` on the current scene. Call once per frame. Presenting the frame is still up to you, here or in the scene.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.game.scene.objectPool(name, factory)`
Creates an object pool with the given name. Each call creates a new pool, even with a name used before, up to 8 per app (further calls raise an error).

- **Parameters:**
  - `name` (string): Pool name, a label only: it becomes the table's `name` field
  - `factory` (function): Factory used to create the pool's objects (required)
- **Returns:** (table) The object pool

```lua
local bullets = picocalc.game.scene.objectPool("bullets", function() return {} end)
```

---

### Object Pool Methods

A pool holds up to 64 idle objects. Call these with method syntax on the table returned by `objectPool()`. A pool hands objects back as they were released and does not reset them, and does not keep count of the ones you hold.

#### `pool:acquire()`
Takes the most recently released object from the pool, or calls the factory to make a new one if the pool is empty. An error in the factory is raised to the caller.

- **Returns:** (any) The object

#### `pool:release(obj)`
Returns an object to the pool for reuse. If the pool already holds 64 objects, the object is silently dropped. Don't release one twice: the pool would hand it out twice.

- **Parameters:**
  - `obj` (any): Object to return
- **Returns:** None

#### `pool:prewarm(count)`
Calls the factory to add idle objects to the pool ahead of time, stopping when it holds 64. An error in the factory is raised to the caller.

- **Parameters:**
  - `count` (number): Number of objects to create (clamped to 0-64)
- **Returns:** None

#### `pool:clear()`
Discards all idle objects in the pool. Objects currently in use are not affected.

- **Returns:** None

```lua
local bullets = picocalc.game.scene.objectPool("bullets", function() return {x = 0, y = 0} end)
bullets:prewarm(16)
local b = bullets:acquire()
-- ... use b ...
bullets:release(b)
```

---

#### `picocalc.game.scene.setGlobal(key, value)`
Store a value shared across all scenes.

- **Parameters:**
  - `key` (string): Key name
  - `value` (any): Value to store
- **Returns:** None

---

#### `picocalc.game.scene.getGlobal(key)`
Read a value shared across scenes.

- **Parameters:**
  - `key` (string): Key name
- **Returns:** (any) Stored value, or `nil`

---

#### `picocalc.game.scene.clearGlobals()`
Clear all values shared across scenes.

- **Parameters:** None
- **Returns:** None

---

## picocalc.game.save

Per-app save slots, one JSON file each at `/data/<APP_ID>/saves/<key>.json`.

- Keys are 1-128 bytes of `[A-Za-z0-9._-]`, contain no `..` and do not start
  with `.`. Any other key is refused: `set` returns `false, "invalid save name"`,
  `get` returns nil, `exists`/`delete` return false.
- The value passed to `set` must be a **table** (nested tables, strings,
  numbers, booleans). An array stays an array. A table with holes in its integer
  keys, or with string keys as well, comes back with all its keys as strings
  (`[1]` becomes `["1"]`). Whole floats keep `.0` so `math.type` survives.
- Writes happen when `set` is called, and your exit code does not run when the
  player picks Exit App, so call `set` when the data changes rather than on the
  way out.
- Saves used to live in a shared `/saves/<key>.json`. The first time an app
  reads a key (`get`/`exists`) that it has no slot for and the old file
  exists, the file is **copied** into the app's slot; the old file is never
  modified or removed.
- `list()` returns only this app's slot names.

### Functions

#### `picocalc.game.save.set(key, value)`
Save a value under a key.

- **Parameters:**
  - `key` (string): Key name
  - `value` (table): Value to save
- **Returns:** `true`, or `false, err` for an invalid key or a failed write. A value that is not a table, or that holds a function or other value JSON cannot encode, raises an error.

```lua
picocalc.game.save.set("highscore", { score = 12500 })
```

---

#### `picocalc.game.save.get(key)`
Load a saved value.

- **Parameters:**
  - `key` (string): Key name
- **Returns:** (any) Saved value, or `nil` if the key does not exist

```lua
local best = (picocalc.game.save.get("highscore") or {}).score or 0
```

---

#### `picocalc.game.save.exists(key)`
Check whether a key has a saved value.

- **Parameters:**
  - `key` (string): Key name
- **Returns:** (boolean) `true` if the key exists

---

#### `picocalc.game.save.delete(key)`
Delete a saved value.

- **Parameters:**
  - `key` (string): Key name
- **Returns:** (boolean) `false` if the slot did not exist or the key is invalid

---

#### `picocalc.game.save.list()`
List all saved keys.

- **Parameters:** None
- **Returns:** (table) Array of key name strings

```lua
for _, key in ipairs(picocalc.game.save.list()) do
    picocalc.sys.log("save key: " .. key)
end
```

---

## Example

A play scene that follows a player with the camera, and a pause scene pushed over it. The high score is saved as soon as it is beaten, because your exit code does not run when the player picks Exit App. Esc quits and Enter pauses.

The scene reads `picocalc.gamepad` (see [API Gamepad](API-Gamepad.md)), which needs firmware with API version 9. To also run on older firmware, check for it once and fall back to `picocalc.input`:

```lua
local pad = picocalc.gamepad
local input = picocalc.input
-- held mask, and the masks to test it against
local read = pad and pad.getButtons or input.getButtons
local RIGHT = pad and pad.PAD_RIGHT or input.BTN_RIGHT
```

```lua
local scene = picocalc.game.scene
local save = picocalc.game.save
local display = picocalc.display
local input = picocalc.input
-- The gamepad is the recommended way to read game input: players can rebind it
-- in Settings > Controls. Esc and Enter stay on picocalc.input.
local pad = picocalc.gamepad

local camera = picocalc.game.camera.new()
camera:setBounds(0, 0, 1024, 1024)

local player = { x = 160, y = 160 }
local coin = { x = 260, y = 160 }
local score = 0
local best = (save.get("highscore") or {}).score or 0
local quit = false

local play = {}

-- Runs again each time the pause scene pops, so it must not reset the game.
function play.enter()
    camera:setTarget(player.x, player.y)
end

function play.update(dt)
    input.update()
    local pressed = input.getButtonsPressed()
    if pressed & input.BTN_ESC ~= 0 then
        quit = true
    elseif pressed & input.BTN_ENTER ~= 0 then
        scene.push("pause")
        return
    end

    local buttons = pad.getButtons()
    if buttons & pad.PAD_RIGHT ~= 0 then player.x = player.x + 2 end
    if buttons & pad.PAD_LEFT  ~= 0 then player.x = player.x - 2 end
    if buttons & pad.PAD_DOWN  ~= 0 then player.y = player.y + 2 end
    if buttons & pad.PAD_UP    ~= 0 then player.y = player.y - 2 end

    if math.abs(player.x - coin.x) < 8 and math.abs(player.y - coin.y) < 8 then
        score = score + 1
        coin.x = math.random(16, 1008)
        coin.y = math.random(16, 1008)
        if score > best then
            best = score
            save.set("highscore", { score = best })
        end
    end

    camera:setTarget(player.x, player.y)
    camera:update(dt)
end

function play.draw()
    display.clear(display.BLACK)
    local ox, oy = camera:getOffset()
    display.fillRect(coin.x - 4 + ox, coin.y - 4 + oy, 8, 8, display.YELLOW)
    display.fillRect(player.x - 4 + ox, player.y - 4 + oy, 8, 8, display.RED)
    display.drawText(4, 4, "SCORE " .. score .. "  BEST " .. best, display.WHITE)
end

local pause = {}

function pause.update(dt)
    input.update()
    if input.getButtonsPressed() & input.BTN_ENTER ~= 0 then
        scene.pop()
    end
end

function pause.draw()
    play.draw()    -- only the top scene runs, so draw the game behind the menu
    display.drawText(142, 156, "PAUSED", display.WHITE)
end

scene.add("play", play)
scene.add("pause", pause)
scene.switch("play")

local last = picocalc.sys.getTimeMs()
while not quit do
    local now = picocalc.sys.getTimeMs()
    scene.update((now - last) / 1000)
    last = now
    scene.draw()
    display.flush()  -- also paces the loop
end
```
