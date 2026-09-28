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
Returns the frames presented per second, averaged over the last 30 frames: the time from one `endFrame()` to the next, including the wait `setTargetFPS` adds. A game paced at 30 reports 30 however little work its frames take.

- **Parameters:** None
- **Returns:** (number) Frames per second

```lua
local fps = picocalc.perf.getFPS()
```

---

#### `picocalc.perf.getFrameTime()`
Returns the last frame's work in milliseconds: from the previous `endFrame()` (or `beginFrame()`, for the first frame) to this `endFrame()`, without the `setTargetFPS` wait. Compare it with the target's frame period to see the headroom.

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
Set target frame rate for automatic frame pacing. When set, `endFrame()` waits until the frame's deadline, which advances by 1/fps from the previous one (in microseconds, so 30 fps is 33 333 µs, not 33 ms). A frame that ends late restarts the schedule from that moment instead of rushing later frames. Pass `0` to disable frame limiting.

- **Parameters:**
  - `fps` (number): Target frames per second (`0` = unlimited)
- **Returns:** None

```lua
picocalc.perf.setTargetFPS(30)  -- Cap at 30 FPS
picocalc.perf.setTargetFPS(0)   -- Disable frame limiting
```
