---
title: "API — Game Framework"
---

Game-framework helpers: a 2D camera, a scene manager, and per-app persistent save files. Three sub-namespaces: `picocalc.game.camera`, `picocalc.game.scene`, and `picocalc.game.save`.

## picocalc.game.camera

A 2D camera with position, zoom, target following, world bounds, and screen shake.

### Functions

#### `picocalc.game.camera.new()`
Create a new camera, positioned at (0, 0) with zoom 1.0.

- **Parameters:** None
- **Returns:** (userdata) PicoDeckCamera object

```lua
local camera = picocalc.game.camera.new()
```

---

### Camera Methods

Objects returned by `picocalc.game.camera.new()`.

#### `camera:setPosition(x, y)`
Set the camera position in world coordinates.

- **Parameters:**
  - `x` (number): World X coordinate
  - `y` (number): World Y coordinate
- **Returns:** None

```lua
camera:setPosition(160, 160)
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
Set the camera zoom factor. 1.0 is unscaled.

- **Parameters:**
  - `zoom` (number): Zoom factor
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
Follow a world position: `camera:update(dt)` eases the camera towards it. Call it again each frame to track a moving object.

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
Constrain the camera to a world rectangle — the camera will not scroll outside it. The arguments are two corners, not a size.

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
Shake the camera on both axes.

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
- **Returns:** (number, number) Screen `sx, sy` coordinates

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
Advance target following and screen shake. Call once per frame.

- **Parameters:**
  - `dt` (number): Seconds since the last update
- **Returns:** None

```lua
camera:update(1 / 30)
```

---

#### `camera:getOffset()`
Get the current draw offset (camera position plus shake). Subtract this from world coordinates when drawing.

- **Parameters:** None
- **Returns:** (number, number) Offset `ox, oy`

```lua
local ox, oy = camera:getOffset()
picocalc.display.fillRect(player.x - ox, player.y - oy, 8, 8, 0xFFFF)
```

---

## picocalc.game.scene

A scene manager. Scenes are table-like objects with `update()`, `draw()`, `enter()`, and `exit()` lifecycle methods.

### Functions

#### `picocalc.game.scene.new()`
Create a new (empty) scene object.

- **Parameters:** None
- **Returns:** (userdata) PicoDeckScene object

---

#### `picocalc.game.scene.add(name, scene)`
Register a scene under a name.

- **Parameters:**
  - `name` (string): Scene name
  - `scene` (table): Scene object with lifecycle methods
- **Returns:** None

```lua
picocalc.game.scene.add("menu", menuScene)
```

---

#### `picocalc.game.scene.remove(name)`
Remove a registered scene.

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
Switch to another scene. Calls `exit()` on the current scene, then `enter()` on the next.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** None

```lua
picocalc.game.scene.switch("play")
```

---

#### `picocalc.game.scene.push(name)`
Overlay a scene on top of the current one without exiting it.

- **Parameters:**
  - `name` (string): Scene name
- **Returns:** None

```lua
picocalc.game.scene.push("pause")
```

---

#### `picocalc.game.scene.pop()`
Pop the overlaid scene and return to the previous one.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.game.scene.getCurrent()`
Get the currently active scene.

- **Parameters:** None
- **Returns:** (table or nil) Current PicoDeckScene, or `nil` if no scene is active

---

#### `picocalc.game.scene.update()`
Call `update()` on the current scene. Call once per frame.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.game.scene.draw()`
Call `draw()` on the current scene. Call once per frame.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.game.scene.objectPool(name, factory)`
Get (or create) the per-scene object pool with the given name.

- **Parameters:**
  - `name` (string): Pool name
  - `factory` (function): Factory used to create the pool's objects (required)
- **Returns:** (table) The object pool

```lua
local bullets = picocalc.game.scene.objectPool("bullets", function() return {} end)
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

Per-app save slots, one JSON file each at `/data/<app id>/saves/<key>.json`.

- Keys are 1-128 bytes of `[A-Za-z0-9._-]`, contain no `..` and do not start
  with `.`. Any other key is refused: `set` returns `false, "invalid save name"`,
  `get` returns nil, `exists`/`delete` return false.
- The value passed to `set` must be a **table** (nested tables, strings,
  numbers, booleans). Integer-keyed tables come back with string keys; whole
  floats keep `.0` so `math.type` survives.
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
- **Returns:** `true`, or `false, err`

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

A play scene that follows a player sprite with the camera and saves the high score on exit.

```lua
local camera = picocalc.game.camera.new()

local score = 0
local player = { x = 160, y = 160 }
function player:getPosition()
    return self.x, self.y
end

local play = {}

function play:enter()
    score = 0
    camera:setTarget(player)
    camera:setBounds(0, 0, 1024, 1024)
end

function play:update()
    picocalc.input.update()
    local buttons = picocalc.input.getButtons()
    if buttons & picocalc.input.BTN_RIGHT ~= 0 then player.x = player.x + 2 end
    if buttons & picocalc.input.BTN_LEFT  ~= 0 then player.x = player.x - 2 end
    if buttons & picocalc.input.BTN_DOWN  ~= 0 then player.y = player.y + 2 end
    if buttons & picocalc.input.BTN_UP    ~= 0 then player.y = player.y - 2 end
    score = score + 1
    camera:update()
end

function play:draw()
    picocalc.display.clear(0x0000)
    local ox, oy = camera:getOffset()
    picocalc.display.fillRect(player.x - 4 - ox, player.y - 4 - oy, 8, 8, 0xF800)
    picocalc.display.drawText(4, 4, "SCORE " .. score, 0xFFFF)
    picocalc.display.flush()
end

function play:exit()
    local best = (picocalc.game.save.get("highscore") or {}).score or 0
    if score > best then
        picocalc.game.save.set("highscore", { score = score })
    end
end

picocalc.game.scene.add("play", play)
picocalc.game.scene.switch("play")

while true do
    picocalc.game.scene.update()
    picocalc.game.scene.draw()
    picocalc.sys.sleep(16)
end
```
