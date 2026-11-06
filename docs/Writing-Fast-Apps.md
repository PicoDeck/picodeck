---
title: "Writing Fast Apps"
---

What makes an app slow on this device, and what to do about it. Figures are measured on the device at 200 MHz unless they say otherwise. Where something has not been measured, this page says what the code does and gives no number.

## How the device spends its time

Most of the CPU's time goes on waiting for memory, not on instructions. Your code and the OS's (in flash), and the Lua heap, every Lua object, the Lua VM's stack, and a native app's whole image and stack (in PSRAM), all reach the CPU through one 16 KB cache with 8-byte lines, shared with the second core. Lua workloads stall on it for 58-85% of their cycles, at a 96-98% hit rate.

So:

- **What costs is the memory you touch**, more than the instructions you run. Writing to fresh PSRAM costs about 0.85 µs per 8 bytes, so creating a string costs about 0.2 µs per byte however it is filled.
- **Measure whole frames, not single calls.** The layout of the code alone moves a short call by ±40% between firmware builds (the same `fillRect` took 10.5 µs in one and 15.3 µs in another). Use `picocalc.perf.getFrameTime()`, and the simulator only for logic: it has no such cache, so its timings predict nothing here.
- **Turn WiFi off when you benchmark.** Network activity on the second core adds noise of its own.

The framebuffers are the exception: they are in SRAM, which does not go through that cache.

## Waiting for a key: do not spin

A loop with no wait keeps the CPU at full speed however little it does, and `flush()` does not change that: it waits for the previous frame's transfer by polling. An app that redraws and flushes an unchanging screen also keeps the panel's bus in constant use. No battery figures have been measured, but this is the one thing on this page that costs a player something while your app does nothing.

- `picocalc.sys.sleep(ms)` puts the CPU to sleep between keyboard polls.
- `picocalc.perf.setTargetFPS(fps)` with `perf.endFrame()` sleeps until each frame's deadline. For a game, 30 instead of a free-running loop also gives the second core time to decode music outside your frames ([MP3Player](API-Audio-and-Sound.md#mp3player)).
- On a screen that only a key changes, draw only when something changed:

```lua
local needs_draw, last_draw = true, 0
while true do
    input.update()
    local pressed = input.getButtonsPressed()
    if pressed ~= 0 then
        -- handle it
        needs_draw = true
    end
    local now = sys.getTimeMs()
    if needs_draw or now - last_draw >= 1000 then
        needs_draw, last_draw = false, now
        draw()
        disp.flush()
    else
        sys.sleep(16)
    end
end
```

The once-a-second redraw is for what changes by itself: a clock or battery in your header, and the OS's own overlays, because a toast and the Show FPS counter are drawn into your frame only when you present one. A terminal's cursor needs one about every 520 ms, since its blink moves on only when you render. Key repeat you time in frames needs the loop to run as before while a key is held.

## Presenting a frame

| Present at 200 MHz | Time |
|---------|------|
| `flush()`, the whole 320 × 320 | 16-17 ms |
| Rows 0-239 | 13.07 ms |
| An 80-row strip | 4.52 ms |

- `flush()` starts the transfer and returns, and the next one waits for it, so a frame's drawing can overlap the previous transfer.
- **Send only what changed**: `flushRows()` and `flushRegion()` ([API Display and Graphics](API-Display-and-Graphics.md)).
- **A faster clock slows the panel.** At 300 MHz the bus to the panel drops to 75 MHz, and a full flush takes 23.22 ms (a 43 fps ceiling) and the 80-row strip 5.82 ms. Raise the clock only if your app is limited by the CPU.
- **Reading back is expensive.** `fb:getPixels` costs 0.16-0.2 µs per byte, about 24 ms for a 320 × 240 block, plus the collector's work for that much garbage. `setPixels` is about four times cheaper per byte.

## Lua

What single calls cost, from one firmware build (±40% between builds, as above):

| Call | µs |
|------|----|
| `input.isKeyDown` | 4.8 |
| `string.format("%d", n)` | 15 |
| `tostring(n)` | 21 |
| `img:draw` | 31 |
| `disp.drawText` | 49 |

- **Do not format a number you have not changed.** Keep the string and rebuild it when the value moves, rather than every frame.
- **Do not build strings in loops.** Each `..` makes a new string, at about 0.2 µs per byte plus the collector's work. Collect the pieces in a table and use `table.concat`, or draw them separately.
- **Do not write pixels from Lua.** A per-pixel gradient drawn with Lua calls took 70-100 ms a frame in a demo app. Draw it once into an image and blit that.
- **Keep what you use in locals.** A global or a field such as `picocalc.display.fillRect` is a table lookup at every use, and a local is a VM register.
- **Avoid a new table per frame.** Reuse them. Every allocation that is not a small Lua object costs at least one 200-byte block of the heap.
- **Try the generational collector.** `collectgarbage("generational")` cut frame work by 2-16% on most screens of a demo app and raised it 12% on one, so measure it on yours.
- C-call-heavy Lua pays about 10-50% more because the VM's stack is in PSRAM. Pure Lua loops do not.
- **Collisions cost movers times sprites.** With collisions on, `moveWithCollisions` walks every sprite in the manager once per axis moved, however far the move: about 2.7 µs a pair at 200 MHz. Moving every sprite along one axis each frame took 8 ms at 50 sprites, 28 ms at 100, 64 ms at 150 and 107 ms at 200, before drawing anything, so about 110 sprites use all of a 30 fps frame, and about 80 if they move diagonally. With collisions off the same call takes 14 ms at 200, because it only sets the position. Leave them off on sprites that don't need them.
- Numbers are 32-bit: integers are exact to 2^31 and floats to 2^24.

## Files and sound

- SD reads are polled by the CPU, so they stop your app while they run: a 16 KB `readFile` takes about 24 ms. Load at a level's start, not during a frame.
- Music costs frame time, because the second core shares the cache. For a real-time game: MP3 if the game paces itself with `setTargetFPS` and its frames leave time to spare, otherwise QOA, then WAV. The figures are in [API Audio and Sound](API-Audio-and-Sound.md#mp3player).
- `gfx3d` keeps a scene's working data in about 24 KB of fast memory that the MP3 decoder lends it until the app first loads an MP3 (`mp3player:load`) or plays a video with audio. From then until the app exits, scenes run from slower memory. So in a `gfx3d` game, QOA or WAV music keeps that speed and MP3 music gives it up: about 30% of the frame's work on the device in a scene of about 800 triangles (8.4 ms against 12.0 ms at 200 MHz). Nothing else changes: the picture is the same either way.

## Native apps

- **Build with `-O2`.** The sample Makefile uses `-Os` for size. The shipped emulators and games use `-O2` or `-O3`, some with `-flto`. The firmware's own Lua VM gained about 10% on bytecode and 20-45% on C-call paths from `-O2`, for 23 KB of flash.
- **Turn on the FPU** if you use `float`: `-mfloat-abi=softfp -mfpu=fpv5-sp-d16`. Without it float maths runs in software ([Floating point](Native-Loading.md#floating-point)).
- **Your code and data run from PSRAM**, behind the shared cache, like Lua: the loader's SRAM option needs room in an SRAM heap of a few KB, which no real app fits. A line's place in the cache is its address modulo 8 KB, and each place holds two lines, so keep what a hot loop touches small and contiguous.
- **Write pixels through `display->getBackBuffer()`.** The buffer is in SRAM, so it is the one fast memory you have. Pixels are byte-swapped RGB565. Fetch the pointer each frame, because `flush()` swaps the buffers.
- **Call `sys->poll()` once a frame.** It reads the keyboard, feeds the watchdog and serves the system menu, so a call per row or per sprite does that work for nothing.
- **To wait, pace the loop.** The native API has no sleep call. `perf->setTargetFPS(30)` once and `perf->endFrame()` each frame sleep until the frame's deadline, and a loop around `sys->poll()` with neither keeps the CPU at full speed. Present only when something changes, so that the panel's bus rests too.

## Measuring

- **Show FPS** (system menu → Settings) shows the frame rate of any app, with no code.
- `picocalc.perf.getFrameTime()` gives the last frame's work, without the `setTargetFPS` wait. See [API Performance](API-Performance.md).
- With the device on USB, the `prof` dev command and `tools/pc_prof.py` in the PicoDeck repository sample where the time goes, by firmware function.
