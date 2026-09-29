---
title: "API Performance"
---

Performance monitoring utilities for apps.

## The OS FPS counter (Show FPS)

The OS can count frames and draw an FPS counter over any app, Lua or native, with no app code: system menu → Settings → **Show FPS**. Each press of Enter moves it on: **Off → Top right → Top left → Bottom right → Bottom left**, so you can move a counter that covers your HUD. The setting is the `show_fps` key in [API Sysconfig](API-Sysconfig.md) and takes effect when the menu closes. The counter is drawn only while an app runs, not in the launcher or over the system menu.

- **What it counts.** Frames presented per second, counted over a window of at least 1 s: `flush()` and `flushRegion()` count one frame each. For an app that presents only with `flushRows()`, a call whose `y0` is at or above the previous call's `y0` starts a new frame, so a top-to-bottom sweep of bands is one frame.
- **`perf.endFrame()` sets the number.** While your app calls `endFrame()` (within the last 2 s), the counter shows `endFrame()` calls per second instead: your loop's logical frame rate, even when you present less often (an emulator running two game frames per present shows 60, not 30). It is counted per 1 s window, not from the 30-frame average `getFPS()` returns, so the two can differ by a frame or two.
- **What it looks like.** `FPS: n` in the built-in 6×8 font, colour-coded like `drawFPS()`: green ≥55, yellow ≥30, red below; `FPS: --` in grey until the first second is counted; numbers stop at 999. It sits in an opaque black 52×12 box with its text 8 px in from the screen edges; the top corners are just below the standard header (`picocalc.ui.drawHeader`), where `drawFPS()` draws by default.
- **Where it is drawn.** The OS draws it into your frame just before each present, so it is in your framebuffer after `flush()` and in screenshots. With `flushRows()` and `flushRegion()` it is drawn only into the rows you send; when the box lies outside them the OS updates it on the panel directly (only when its text changes) and leaves your draw buffer as it was. While the hardware-scroll offset (`setScrollOffset`) is not 0, partial flushes skip it.

`drawFPS()` and `getFPS()` stay for apps that want their own counter.

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
Ends timing a frame and updates FPS calculation. Call at the **end** of your game loop. While you call it, the OS FPS counter (Show FPS, above) shows your `endFrame()` calls per second.

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
