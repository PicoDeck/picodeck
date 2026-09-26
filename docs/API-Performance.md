---
title: "API Performance"
---

Performance monitoring utilities for apps.

## picocalc.perf

### Functions

#### `picocalc.perf.beginFrame()`
Starts timing a frame. Call at the **beginning** of your game loop.

- **Parameters:** None
- **Returns:** None

```lua
while true do
    picocalc.perf.beginFrame()
    -- Game logic
    picocalc.perf.endFrame()
end
```

---

#### `picocalc.perf.endFrame()`
Ends timing a frame and updates FPS calculation. Call at the **end** of your game loop.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.perf.getFPS()`
Returns the current FPS, averaged over the last 30 frames.

- **Parameters:** None
- **Returns:** (number) Frames per second

```lua
local fps = picocalc.perf.getFPS()
```

---

#### `picocalc.perf.getFrameTime()`
Returns the last frame's duration in milliseconds.

- **Parameters:** None
- **Returns:** (number) Milliseconds

```lua
local ms = picocalc.perf.getFrameTime()
```

---

#### `picocalc.perf.drawFPS([x, y])`
Convenience function to draw the FPS counter on screen. Color-coded: green ≥55 FPS, yellow ≥30, red <30.

- **Parameters:**
  - `x` (number, optional): X coordinate. Defaults to the top-right corner: the text ends 8px from the right edge.
  - `y` (number, optional): Y coordinate. Defaults to 24, just below the standard header (`picocalc.ui.drawHeader`), so the counter never covers its status icons.
- **Returns:** None

```lua
picocalc.perf.drawFPS()  -- Draw at default position
```

---

#### `picocalc.perf.setTargetFPS(fps)`
Set target frame rate for automatic frame pacing. When set, `endFrame()` will sleep to maintain the target rate. Pass `0` to disable frame limiting.

- **Parameters:**
  - `fps` (number): Target frames per second (`0` = unlimited)
- **Returns:** None

```lua
picocalc.perf.setTargetFPS(30)  -- Cap at 30 FPS
picocalc.perf.setTargetFPS(0)   -- Disable frame limiting
```
