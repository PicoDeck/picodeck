---
title: "API Display and Graphics"
---

Graphics and display functions. The display is **320×320 pixels** with RGB565 color format.

## picocalc.display

Coordinates and sizes may be floats: they round to nearest (`fillRect(10.5, …)`
draws at x = 11). NaN, inf and values beyond ±2^24 are argument errors.
Colour components, brightness and effect factors clamp to their range.

Every primitive clips to the clip rect (`setClipRect`, default the screen)
once, before drawing, so huge or off-screen shapes (a line to 1e7, a
radius-1e6 circle) cost only their visible pixels.

### Functions

#### `picocalc.display.clear([color])`
Clears the entire framebuffer to the specified color.

- **Parameters:**
  - `color` (number, optional): RGB565 color value. Defaults to `BLACK` if omitted.
- **Returns:** None

```lua
picocalc.display.clear(picocalc.display.BLACK)
```

---

#### `picocalc.display.setPixel(x, y, color)`
Sets a single pixel at the specified coordinates.

- **Parameters:**
  - `x` (number): X coordinate (0-319)
  - `y` (number): Y coordinate (0-319)
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.setPixel(160, 160, picocalc.display.WHITE)
```

---

#### `picocalc.display.fillRect(x, y, width, height, color)`
Draws a filled rectangle.

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Rectangle width in pixels
  - `height` (number): Rectangle height in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillRect(10, 10, 50, 30, picocalc.display.RED)
```

---

#### `picocalc.display.drawRect(x, y, width, height, color)`
Draws a rectangle outline (1-pixel border).

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Rectangle width in pixels
  - `height` (number): Rectangle height in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.drawRect(10, 10, 100, 50, picocalc.display.BLUE)
```

---

#### `picocalc.display.drawLine(x0, y0, x1, y1, color)`
Draws a line between two points.

- **Parameters:**
  - `x0` (number): Starting X coordinate
  - `y0` (number): Starting Y coordinate
  - `x1` (number): Ending X coordinate
  - `y1` (number): Ending Y coordinate
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.drawLine(0, 0, 319, 319, picocalc.display.GREEN)
```

---

#### `picocalc.display.drawText(x, y, text, fg_color [, bg_color])`
Draws text using the active font (default 6×8, ASCII 0x20–0x7E). Set the active font with `picocalc.display.setFont`. Pass `bg_color = false` to draw with a transparent background — only glyph pixels are written, so the text can sit over existing art.

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `text` (string): Text to draw
  - `fg_color` (number): Foreground RGB565 color
  - `bg_color` (number or false, optional): Background RGB565 color. Defaults to `BLACK`. Pass `false` for a transparent background (glyph pixels only).
- **Returns:** (number) Pixel width of the drawn text

```lua
local width = picocalc.display.drawText(10, 10, "Hello!", picocalc.display.WHITE)
picocalc.display.drawText(10, 20, "Overlay", picocalc.display.WHITE, false)  -- transparent bg
```

---

#### `picocalc.display.textWidth(text)`
Calculates the pixel width of text without drawing it.

- **Parameters:**
  - `text` (string): Text to measure
- **Returns:** (number) Width in pixels

```lua
local width = picocalc.display.textWidth("Hello World")
```

---

#### `picocalc.display.flush()`
Flushes the internal framebuffer to the LCD via DMA. **Call once per frame** after all drawing is complete.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.display.flush()
```

---

#### `picocalc.display.flushRows(y0, y1)`
Pushes rows `y0`–`y1` (inclusive) of the **current draw buffer** to the LCD via non-blocking DMA, **without swapping buffers**. Rows are clamped to 0–319; the band always spans the full screen width.

Because there is no swap, subsequent drawing continues into the same buffer — ideal for repeatedly updating a small horizontal band (status bar, HUD, terminal line) while the rest of the screen keeps its last presented contents. Mixing `flushRows` with the normal double-buffered `flush()` cycle is the job of `flushRegion()` instead.

- **Parameters:**
  - `y0` (number): First row (inclusive)
  - `y1` (number): Last row (inclusive)
- **Returns:** None

```lua
-- Redraw just a score bar without touching the play field
picocalc.display.fillRect(0, 0, 320, 16, picocalc.display.BLACK)
picocalc.display.drawText(4, 4, "SCORE " .. score, picocalc.display.WHITE, false)
picocalc.display.flushRows(0, 15)
```

---

#### `picocalc.display.flushRegion(y0, y1)`
Like `flush()`, but transfers only rows `y0`–`y1` (inclusive): the front/back buffers are **swapped**, and the flushed band is then copied back into the new back buffer so both buffers stay in sync for that region. Rows are clamped to 0–319.

Use this when your app follows the normal draw-then-flush double-buffered cycle but only a horizontal band changed — cheaper than a full-screen transfer, and later full `flush()` calls will not flicker. Costs one extra band-sized copy compared to `flushRows()`.

- **Parameters:**
  - `y0` (number): First row (inclusive)
  - `y1` (number): Last row (inclusive)
- **Returns:** None

```lua
-- Only the animation strip in the middle changed this frame
picocalc.display.flushRegion(120, 200)
```

---

#### `picocalc.display.getWidth()`
Returns the display width in pixels.

- **Parameters:** None
- **Returns:** (number) 320

---

#### `picocalc.display.getHeight()`
Returns the display height in pixels.

- **Parameters:** None
- **Returns:** (number) 320

---

#### `picocalc.display.setBrightness(level)`
Sets the display backlight brightness.

- **Parameters:**
  - `level` (number): Brightness value (0-255, where 255 is full brightness) (clamped)
- **Returns:** None

```lua
picocalc.display.setBrightness(128)  -- 50% brightness
```

---

#### `picocalc.display.rgb(r, g, b)`
Converts 8-bit RGB components to a 16-bit RGB565 color value.

- **Parameters:**
  - `r` (number): Red component (0-255) (clamped)
  - `g` (number): Green component (0-255) (clamped)
  - `b` (number): Blue component (0-255) (clamped)
- **Returns:** (number) RGB565 color value

```lua
local purple = picocalc.display.rgb(128, 0, 128)
picocalc.display.clear(purple)
```

---

#### `picocalc.display.drawCircle(cx, cy, radius, color)`
Draw a circle outline.

- **Parameters:**
  - `cx` (number): Center X coordinate
  - `cy` (number): Center Y coordinate
  - `radius` (number): Circle radius in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.drawCircle(160, 160, 50, picocalc.display.WHITE)
```

---

#### `picocalc.display.fillCircle(cx, cy, radius, color)`
Draw a filled circle.

- **Parameters:**
  - `cx` (number): Center X coordinate
  - `cy` (number): Center Y coordinate
  - `radius` (number): Circle radius in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillCircle(160, 160, 50, picocalc.display.RED)
```

---

#### `picocalc.display.fillVLine(x, y0, y1, color)`
Draw an optimized vertical line.

- **Parameters:**
  - `x` (number): X coordinate
  - `y0` (number): Top Y coordinate
  - `y1` (number): Bottom Y coordinate
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillVLine(100, 10, 300, picocalc.display.GREEN)
```

---

#### `picocalc.display.fillHLine(y, x0, x1, color)`
Draw an optimized horizontal line.

- **Parameters:**
  - `y` (number): Y coordinate
  - `x0` (number): Left X coordinate
  - `x1` (number): Right X coordinate
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillHLine(50, 10, 300, picocalc.display.GREEN)
```

---

#### `picocalc.display.fillTriangle(x0, y0, x1, y1, x2, y2, color)`
Draw a filled triangle.

- **Parameters:**
  - `x0`, `y0` (number): First vertex
  - `x1`, `y1` (number): Second vertex
  - `x2`, `y2` (number): Third vertex
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillTriangle(160, 40, 60, 280, 260, 280, picocalc.display.RED)
```

---

#### `picocalc.display.fillVLineGradient(x, y0, y1, colorTop, colorBottom)`
Draw a vertical line with gradient between two colors.

- **Parameters:**
  - `x` (number): X coordinate
  - `y0` (number): Top Y coordinate
  - `y1` (number): Bottom Y coordinate
  - `colorTop` (number): RGB565 color at the top
  - `colorBottom` (number): RGB565 color at the bottom
- **Returns:** None

```lua
picocalc.display.fillVLineGradient(160, 0, 319, picocalc.display.BLUE, picocalc.display.BLACK)
```

---

#### `picocalc.display.drawTexturedColumn(x, y0, y1, image, texX, texY0, texY1)`
Draw a vertical column of pixels sampled from a texture image. Useful for raycasting renderers.

- **Parameters:**
  - `x` (number): Screen X coordinate
  - `y0` (number): Screen top Y coordinate
  - `y1` (number): Screen bottom Y coordinate
  - `image` (userdata): Image object from `picocalc.graphics.image.load()` or `.new()`
  - `texX` (number): Texture X coordinate to sample from
  - `texY0` (number): Texture top Y coordinate
  - `texY1` (number): Texture bottom Y coordinate
- **Returns:** None

```lua
-- Draw a column from a wall texture (raycasting)
picocalc.display.drawTexturedColumn(x, wallTop, wallBottom, wallTexture, texCol, 0, 63)
```

---

#### `picocalc.display.setClipRect(x, y, w, h)`
Restrict all drawing primitives to a rectangle. Useful for split-screen, UI panels, and partial redraws. `clear()` and the framebuffer effects are NOT clipped (they are whole-buffer by design). The rectangle is clamped to the 320×320 screen. The clip rect is reset to full screen automatically at app launch and exit, so apps always start (and leave the launcher) unclipped.

- **Parameters:**
  - `x` (number): Clip rect left
  - `y` (number): Clip rect top
  - `w` (number): Clip rect width
  - `h` (number): Clip rect height
- **Returns:** None

```lua
picocalc.display.setClipRect(0, 0, 160, 320)   -- left half only
-- ... draw player 1 view ...
picocalc.display.clearClipRect()                -- back to full screen
```

---

#### `picocalc.display.getClipRect()`
Return the current clip rectangle.

- **Returns:** (number, number, number, number) `x, y, w, h`

---

#### `picocalc.display.clearClipRect()`
Restore the clip rectangle to the full screen.

- **Returns:** None

---

#### `picocalc.display.drawPlane(image, camX, camY, camZ, [angle], [horizonY], [scale])`
Render a Mode 7-style perspective ground plane (SNES F-Zero / Mario Kart floor). The camera sits at `(camX, camY)` in texture space, `camZ` units above the plane, facing `angle` radians (0 = toward +Y in texture space). Rows below `horizonY` are filled. Power-of-two texture dimensions (64/128/256) wrap seamlessly; other sizes clamp at the edges. Respects the clip rect.

- **Parameters:**
  - `image` (userdata): Ground texture image
  - `camX` (number): Camera X in texture space
  - `camY` (number): Camera Y in texture space
  - `camZ` (number): Camera height above the plane
  - `angle` (number, optional): Facing in radians (default 0)
  - `horizonY` (number, optional): Horizon scanline (default 120)
  - `scale` (number, optional): FOV/zoom tuning, larger = further view (default 1.0)
- **Returns:** None

```lua
local floor = picocalc.graphics.image.load(APP_DIR .. "/track.png")  -- 256x256
while true do
    picocalc.display.clear(picocalc.display.rgb(64, 64, 128))  -- sky
    picocalc.display.drawPlane(floor, x, y, 20.0, angle, 120, 40.0)
    picocalc.display.flush()
end
```

---

#### `picocalc.display.setFont(fontId)`
Set the active bitmap font for `drawText` and `textWidth`.

- **Parameters:**
  - `fontId` (number): One of the `FONT_*` constants, or an id returned by `loadFont`. An id that is not currently loaded (unknown, unloaded, or never loaded) is ignored and the active font is left unchanged.
- **Returns:** None

```lua
picocalc.display.setFont(picocalc.display.FONT_8X12)
picocalc.display.drawText(10, 10, "Larger text", picocalc.display.WHITE)
```

---

#### `picocalc.display.getFont()`
Get the current font ID.

- **Parameters:** None
- **Returns:** (number) Font ID constant

```lua
local currentFont = picocalc.display.getFont()
```

---

#### `picocalc.display.getFontWidth()`
Get the maximum glyph advance in pixels of the current font. For a proportional font this is the widest glyph, not every glyph's width — use `textWidth` to measure a specific string.

- **Parameters:** None
- **Returns:** (number) Max advance in pixels

```lua
local charWidth = picocalc.display.getFontWidth()
```

---

#### `picocalc.display.getFontHeight()`
Get the character height in pixels of the current font.

- **Parameters:** None
- **Returns:** (number) Height in pixels

```lua
local charHeight = picocalc.display.getFontHeight()
```

---

#### `picocalc.display.loadFont(path)`
Loads a `.pfn` bitmap font from an absolute SD path and returns a font id for use with `setFont`. The path is sandbox-checked exactly like image loading. Loaded fonts occupy ids 4..11 (at most 8 loaded at a time, on top of the 4 built-ins) and each may be up to roughly 128 KB in PSRAM. Every font loaded by an app is automatically freed when the app exits, or earlier via `unloadFont`. See [Custom fonts](#custom-fonts) below for the `.pfn` format and the `tools/mkfont.py` build tool.

- **Parameters:**
  - `path` (string): Absolute path to a `.pfn` file
- **Returns:** (number or nil) Font id (4-11) on success, `nil` on sandbox denial or load failure (missing file, bad magic, size mismatch, no free slot). Never raises.

```lua
local id = picocalc.display.loadFont(APP_DIR .. "/fonts/custom.pfn")
if id then
    picocalc.display.setFont(id)
    picocalc.display.drawText(10, 10, "Custom!", picocalc.display.WHITE)
end
```

---

#### `picocalc.display.unloadFont(id)`
Frees a font previously returned by `loadFont`. If `id` is the currently active font, the active font falls back to `FONT_6X8` (id 0) first. No-op for built-in font ids (0-3) or an id that is not currently loaded.

- **Parameters:**
  - `id` (number): Font id returned by `loadFont`
- **Returns:** None

```lua
picocalc.display.unloadFont(id)
```

---

#### `picocalc.display.setScrollArea(top, height, bottom)`
Configure the LCD's hardware vertical scroll area (ST7365P VSCRDEF).

The controller's frame memory is 480 lines; the visible panel shows lines
0..319. The three values must sum to **480**. The standard configuration is
`setScrollArea(0, 320, 160)`, which turns the whole visible panel into a
mod-320 ring: with an offset set, screen row `L` displays frame-memory row
`(offset + L) % 320`.

- **Parameters:**
  - `top` (number): Fixed rows at the top of frame memory
  - `height` (number): Scrolling area height in rows
  - `bottom` (number): Fixed rows at the bottom of frame memory
- **Returns:** None

```lua
-- Ring the whole visible panel over its 320 frame-memory rows
picocalc.display.setScrollArea(0, 320, 160)
```

---

#### `picocalc.display.setScrollOffset(offset)`
Set the hardware vertical scroll offset (ST7365P VSCRSADD): the frame-memory
row displayed at the top of the scroll area. The remap is instant and moves
no pixel data — combined with `flushRows` for the newly revealed strip, this
scrolls full-screen content for the cost of a few rows per frame
(`panels.lua` does exactly this for rigid scroll sequences).

`0` restores the identity mapping. The setter waits out any in-flight flush
DMA before touching the register, so it is safe immediately after
`flush`/`flushRows`.

- **Parameters:**
  - `offset` (number): Frame-memory row shown at the top of the scroll area
- **Returns:** None

```lua
picocalc.display.setScrollOffset(scrollPos % 320)
```

---

#### `picocalc.display.getScrollOffset()`
Return the last offset written with `setScrollOffset`, plus a write counter
(the LCD register itself is write-only).

The OS resets the offset to 0 whenever it takes over the screen (system
menu, app switch) and does **not** restore it. An app driving hardware
scroll must poll this each frame and repaint when either value changes
unexpectedly — the counter catches a foreign write even when the value
matches what the app last set.

- **Returns:**
  - `offset` (number): Last written scroll offset
  - `writeCount` (number): Total register writes since boot

```lua
local off, gen = picocalc.display.getScrollOffset()
if off ~= myOffset or gen ~= myGen then
    -- someone else (system menu) touched the register: repaint
end
```

---

### Font Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `picocalc.display.FONT_6X8` | 0 | Built-in 6x8 pixel bitmap font (default) |
| `picocalc.display.FONT_8X12` | 1 | Larger 8x12 pixel bitmap font |
| `picocalc.display.FONT_SCIENTIFICA` | 2 | Scientifica: monospace 6x12, includes box-drawing glyphs 0x80-0x9F |
| `picocalc.display.FONT_SCIENTIFICA_BOLD` | 3 | Scientifica Bold: monospace 6x12, includes box-drawing glyphs 0x80-0x9F |

Ids 4-11 are reserved for fonts loaded at runtime with `loadFont`/`graphics.font.new` — see [Custom fonts](#custom-fonts).

A byte outside a font's `first..last` range draws as a hollow box rather than a substitute glyph.

---

### Color Constants

Predefined RGB565 color values:

| Constant | Color |
|----------|-------|
| `picocalc.display.BLACK` | Black (0, 0, 0) |
| `picocalc.display.WHITE` | White (255, 255, 255) |
| `picocalc.display.RED` | Red (255, 0, 0) |
| `picocalc.display.GREEN` | Green (0, 255, 0) |
| `picocalc.display.BLUE` | Blue (0, 0, 255) |
| `picocalc.display.YELLOW` | Yellow (255, 255, 0) |
| `picocalc.display.CYAN` | Cyan (0, 255, 255) |
| `picocalc.display.GRAY` | Gray (128, 128, 128) |

---

### Framebuffer Effects

Post-processing effects applied to the entire framebuffer. Draw your scene first, apply effects, then call `flush()`. Effects use the RP2350's hardware interpolators for fast per-pixel blending where applicable.

All effects operate on the back buffer and do not block DMA — they can overlap with the previous frame's transfer for maximum throughput.

#### `picocalc.display.applyEffect("invert")`
Bitwise-inverts all pixels. The fastest effect (~0.3ms).

```lua
picocalc.display.applyEffect("invert")
```

---

#### `picocalc.display.applyEffect("darken", factor)`
Darkens the framebuffer by blending each pixel toward black.

- **Parameters:**
  - `factor` (number, optional): 0 = fully black, 255 = no change. Default: 128.

```lua
picocalc.display.applyEffect("darken", 200)  -- slight darken
picocalc.display.applyEffect("darken", 64)   -- heavy darken
```

---

#### `picocalc.display.applyEffect("brighten", factor)`
Brightens the framebuffer by blending each pixel toward white.

- **Parameters:**
  - `factor` (number, optional): 0 = no change, 255 = fully white. Default: 128.

```lua
picocalc.display.applyEffect("brighten", 80)
```

---

#### `picocalc.display.applyEffect("tint", r, g, b [, strength])`
Blends the framebuffer toward a tint color. Uses hardware interpolator BLEND mode.

- **Parameters:**
  - `r`, `g`, `b` (number): Tint color components (0-255)
  - `strength` (number, optional): Blend strength (0 = no tint, 255 = solid color). Default: 128.

```lua
-- Red tint overlay
picocalc.display.applyEffect("tint", 255, 0, 0, 100)

-- Sepia tone
picocalc.display.applyEffect("tint", 180, 140, 100, 80)
```

---

#### `picocalc.display.applyEffect("fade", r, g, b [, factor])`
Fades the framebuffer toward a target color. Alias for `"tint"` — identical behavior.

- **Parameters:**
  - `r`, `g`, `b` (number): Target color components (0-255)
  - `factor` (number, optional): Fade amount (0 = no change, 255 = solid color). Default: 128.

```lua
-- Fade to black (transition effect)
picocalc.display.applyEffect("fade", 0, 0, 0, 200)

-- Fade to white (flash effect)
picocalc.display.applyEffect("fade", 255, 255, 255, 128)
```

---

#### `picocalc.display.applyEffect("grayscale")`
Desaturates the framebuffer using ITU-R BT.601 luma weights (0.299R + 0.587G + 0.114B).

```lua
picocalc.display.applyEffect("grayscale")
```

---

#### `picocalc.display.applyEffect("blend", image, alpha)`
Alpha-blends an image onto the framebuffer. The image is drawn at (0, 0) and clipped to the screen.

- **Parameters:**
  - `image` (userdata): Image object from `picocalc.graphics.image.load()` or `.new()`
  - `alpha` (number, optional): Opacity (0 = fully transparent, 255 = fully opaque). Default: 128.

```lua
local overlay = picocalc.graphics.image.load(APP_DIR .. "/overlay.png")
picocalc.display.applyEffect("blend", overlay, 100)
```

---

#### `picocalc.display.applyEffect("palette", lut)`
Remaps all framebuffer colors through a lookup table. Each pixel's RGB channels are quantized to an 8-bit index (3 bits red, 3 bits green, 2 bits blue) and replaced with the corresponding LUT entry.

- **Parameters:**
  - `lut` (table): Array of 1-256 RGB565 color values

```lua
-- Create a 256-entry grayscale palette
local lut = {}
for i = 1, 256 do
    local v = math.floor((i - 1) * 255 / 255)
    lut[i] = picocalc.display.rgb(v, v, v)
end
picocalc.display.applyEffect("palette", lut)
```

---

#### `picocalc.display.applyEffect("dither", levels)`
Applies ordered Bayer 4x4 dithering, quantizing colors to a reduced number of levels per channel.

- **Parameters:**
  - `levels` (number, optional): Quantization levels per channel (2-32). Default: 4.

```lua
picocalc.display.applyEffect("dither", 4)   -- retro 4-level dither
picocalc.display.applyEffect("dither", 2)   -- extreme 1-bit style dither
```

---

#### `picocalc.display.applyEffect("scanline", intensity)`
Darkens every other row to create a CRT scanline effect. Uses fast bit-shift operations (no per-pixel channel extraction).

- **Parameters:**
  - `intensity` (number, optional): 1-127 = light scanlines (50% brightness), 128-254 = heavy (25%), 255 = black lines. Default: 128.

```lua
picocalc.display.applyEffect("scanline", 100)  -- subtle CRT effect
picocalc.display.applyEffect("scanline", 255)  -- full black scanlines
```

---

#### `picocalc.display.applyEffect("posterize", levels)`
Reduces color depth by quantizing each channel to a fixed number of levels.

- **Parameters:**
  - `levels` (number, optional): Levels per channel (2-32). Default: 4.

```lua
picocalc.display.applyEffect("posterize", 4)   -- poster-art style
picocalc.display.applyEffect("posterize", 8)   -- subtle reduction
```

---

### Example: Combining Effects

Effects can be chained. Each modifies the framebuffer in sequence.

```lua
while true do
    picocalc.display.clear(picocalc.display.BLACK)

    -- Draw your scene...
    picocalc.display.fillRect(50, 50, 220, 220, picocalc.display.CYAN)
    picocalc.display.drawText(80, 160, "Effects!", picocalc.display.WHITE)

    -- Apply effects (order matters)
    picocalc.display.applyEffect("tint", 255, 100, 0, 60)  -- warm tint
    picocalc.display.applyEffect("scanline", 100)            -- CRT lines
    picocalc.display.applyEffect("dither", 8)                -- subtle dither

    picocalc.display.flush()
end
```

---

### Native C API

Native ELF apps access effects through the `picocalc_display_t` vtable:

```c
void picodeck_main(PicoCalcAPI *api) {
    const picocalc_display_t *d = api->display;

    d->clear(RGB565(0, 0, 0));
    d->drawText(10, 10, "Hello", RGB565(255, 255, 255), RGB565(0, 0, 0));

    // Apply effects
    d->effectTint(255, 0, 0, 128);    // red tint
    d->effectScanline(100);            // CRT scanlines

    d->flush();
}
```

| Function | Signature |
|----------|-----------|
| `effectInvert` | `void (*)(void)` |
| `effectDarken` | `void (*)(uint8_t factor)` |
| `effectBrighten` | `void (*)(uint8_t factor)` |
| `effectTint` | `void (*)(uint8_t r, uint8_t g, uint8_t b, uint8_t strength)` |
| `effectGrayscale` | `void (*)(void)` |
| `effectBlend` | `void (*)(const uint16_t *src, int w, int h, uint8_t alpha)` |
| `effectPalette` | `void (*)(const uint16_t *lut, int lut_size)` |
| `effectDither` | `void (*)(uint8_t levels)` |
| `effectScanline` | `void (*)(uint8_t intensity)` |
| `effectPosterize` | `void (*)(uint8_t levels)` |

API version 4 (`api->version >= 4`) adds the clip rect, mode-7 plane, and the previously Lua-only primitives to the same vtable:

| Function | Signature |
|----------|-----------|
| `setClipRect` | `void (*)(int x, int y, int w, int h)` |
| `getClipRect` | `void (*)(int *x, int *y, int *w, int *h)` |
| `clearClipRect` | `void (*)(void)` |
| `fillHLine` | `void (*)(int y, int x0, int x1, uint16_t color)` |
| `fillTriangle` | `void (*)(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)` |
| `setScrollArea` | `void (*)(int top_fixed, int scroll_height, int bottom_fixed)` |
| `setScrollOffset` | `void (*)(int offset)` |
| `drawPlane` | `void (*)(const uint16_t *tex, int tex_w, int tex_h, float cam_x, float cam_y, float cam_z, float angle, int horizon_y, float scale)` |

API version 6 (`api->version >= 6`) adds the font system to the same vtable. `PC_FONT_6X8`..`PC_FONT_SCIENTIFICA_BOLD` (0-3) name the built-in fonts; `loadFont` returns ids >= 4, up to 8 loaded at a time. Every font a native app loads is freed automatically when the app exits.

| Function | Signature |
|----------|-----------|
| `setFont` | `void (*)(int font_id)` |
| `getFont` | `int (*)(void)` |
| `getFontWidth` | `int (*)(void)` — max advance of the active font |
| `getFontHeight` | `int (*)(void)` |
| `textWidth` | `int (*)(const char *text)` — real width in the active font |
| `loadFont` | `int (*)(const char *path)` — slot id, or -1 on failure |
| `unloadFont` | `void (*)(int font_id)` |
| `drawTextTransparent` | `int (*)(int x, int y, const char *text, uint16_t fg)` |

```c
#define PC_FONT_6X8               0
#define PC_FONT_8X12              1
#define PC_FONT_SCIENTIFICA       2
#define PC_FONT_SCIENTIFICA_BOLD  3
```

---

## picocalc.graphics

Image loading, drawing, and state management. Images are stored in PSRAM and support BMP, JPEG, PNG, and GIF formats.

### State Functions

#### `picocalc.graphics.setColor(color)`
Sets the current drawing color for graphics operations.

- **Parameters:**
  - `color` (number): RGB565 color value
- **Returns:** None

---

#### `picocalc.graphics.setBackgroundColor(color)`
Sets the background color for graphics operations.

- **Parameters:**
  - `color` (number): RGB565 color value
- **Returns:** None

---

#### `picocalc.graphics.setTransparentColor(color)`
Sets the global transparent color for image and sprite drawing. Pixels matching this color will not be drawn.

- **Parameters:**
  - `color` (number or nil): RGB565 color value, or `nil` to disable transparency.
- **Returns:** None

---

#### `picocalc.graphics.getTransparentColor()`
Returns the current global transparent color.

- **Returns:** (number or nil) RGB565 color value, or `nil` if transparency is disabled.

---

#### `picocalc.graphics.setStencilPattern(pattern)`
Sets a global 8-byte stencil pattern applied to subsequent drawing.

- **Parameters:**
  - `pattern` (table or nil): Array of 8 bytes (one per row of the 8×8 pattern), or `nil` to clear the stencil
- **Returns:** None

```lua
picocalc.graphics.setStencilPattern({0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55})
picocalc.graphics.setStencilPattern(nil)  -- clear
```

---

#### `picocalc.graphics.clear([color])`
Clears the screen using the background color (or a specified color).

- **Parameters:**
  - `color` (number, optional): RGB565 color value. Defaults to the current background color.
- **Returns:** None

```lua
picocalc.graphics.setBackgroundColor(picocalc.display.BLACK)
picocalc.graphics.clear()
```

---

### Compound Drawing Functions

#### `picocalc.graphics.drawGrid(x, y, cell_w, cell_h, cols, rows, color)`
Draws a grid of `cols×rows` outlined cells in a single C call.

- **Parameters:**
  - `x`, `y` (number): Top-left corner of the grid
  - `cell_w`, `cell_h` (number): Width and height of each cell in pixels
  - `cols`, `rows` (number): Number of columns and rows
  - `color` (number): RGB565 border color
- **Returns:** None

```lua
picocalc.graphics.drawGrid(10, 10, 14, 14, 10, 20, picocalc.display.GRAY)
```

---

#### `picocalc.graphics.fillBorderedRect(x, y, w, h, fill_color, border_color)`
Fills a rectangle then draws a 1-pixel border over it in a single C call.

- **Parameters:**
  - `x`, `y` (number): Top-left corner
  - `w`, `h` (number): Width and height in pixels
  - `fill_color` (number): RGB565 fill color
  - `border_color` (number): RGB565 border color
- **Returns:** None

```lua
picocalc.graphics.fillBorderedRect(10, 10, 50, 50, picocalc.display.BLUE, picocalc.display.WHITE)
```

---

#### `picocalc.graphics.drawPlayfield(playfield, ox, oy, block_size, cols, rows, grid_color)`
Draws a 2D block grid in a single C call — ideal for falling-block games. `playfield[row][col]` holds an RGB565 color, or `0` for an empty cell. Blocks are filled; the grid lines are drawn in `grid_color`.

- **Parameters:**
  - `playfield` (table): 2D array indexed `playfield[row][col]` (1-based); RGB565 color or `0` = empty
  - `ox`, `oy` (number): Top-left corner of the playfield in pixels
  - `block_size` (number): Width and height of each block in pixels
  - `cols`, `rows` (number): Playfield dimensions in blocks
  - `grid_color` (number): RGB565 grid line color
- **Returns:** None

```lua
local playfield = {}
for r = 1, 20 do
    playfield[r] = {}
    for c = 1, 10 do playfield[r][c] = 0 end
end
playfield[20][5] = picocalc.display.RED

picocalc.graphics.drawPlayfield(playfield, 85, 10, 15, 10, 20, picocalc.display.GRAY)
```

---

#### `picocalc.graphics.updateDrawParticles(flat_array, delta_s)`
Updates, draws, and compacts a flat particle array in a single C call. The array holds 6 values per particle: `x, y, vx, vy, life_ms, color`.

- **Parameters:**
  - `flat_array` (table): Flat sequence with 6 values per particle
  - `delta_s` (number): Elapsed time in seconds since last call
- **Returns:** (number) Count of live particles remaining

```lua
-- particles = {x, y, vx, vy, life_ms, color, ...}
local live = picocalc.graphics.updateDrawParticles(particles, delta / 1000)
```

---

#### `picocalc.graphics.draw3DWireframe(verts, edges, aX, aY, aZ, scx, scy, fov, edgeColor [, vertColor [, vertSize]])`
Rotates, projects, and draws a 3D wireframe model in a single C call. All trigonometry and matrix math runs in C — suitable for real-time use in game loops.

- **Parameters:**
  - `verts` (table): Flat sequence `{x1, y1, z1, x2, y2, z2, ...}` — `n/3` vertices
  - `edges` (table): Flat sequence `{a1, b1, a2, b2, ...}` — 1-based vertex index pairs
  - `aX`, `aY`, `aZ` (number): Rotation angles in radians, applied in X→Y→Z order
  - `scx`, `scy` (number): Screen-space center point (projection origin)
  - `fov` (number): Field-of-view scale factor (larger = more perspective, try 200–400)
  - `edgeColor` (number): RGB565 color for edges
  - `vertColor` (number, optional): RGB565 color for vertex dots. Defaults to `edgeColor`.
  - `vertSize` (number, optional): Dot size in pixels for vertex dots. Defaults to 3.
- **Returns:** None

```lua
local verts = {
    -1,-1,-1,  1,-1,-1,  1,1,-1, -1,1,-1,  -- back face
    -1,-1, 1,  1,-1, 1,  1,1, 1, -1,1, 1,  -- front face
}
local edges = {
    1,2, 2,3, 3,4, 4,1,  -- back
    5,6, 6,7, 7,8, 8,5,  -- front
    1,5, 2,6, 3,7, 4,8,  -- sides
}
local angle = 0
while true do
    angle = angle + 0.02
    picocalc.display.clear(picocalc.display.BLACK)
    picocalc.graphics.draw3DWireframe(verts, edges, angle, angle*0.7, 0,
        160, 160, 300, picocalc.display.WHITE)
    picocalc.display.flush()
end
```

---

#### `draw3DWireframeEx(verts, edges, angleX, angleY, angleZ, scx, scy, fov, edgeColor [, fillColor [, fillMode [, vertSize [, faces]]]])`
Enhanced 3D wireframe with optional filled triangles. **Note:** this is a global function, not under `picocalc.*`.

- **Parameters:**
  - `verts` (table): Flat sequence `{x1, y1, z1, x2, y2, z2, ...}` — `n/3` vertices
  - `edges` (table): Flat sequence `{a1, b1, a2, b2, ...}` — 1-based vertex index pairs
  - `angleX`, `angleY`, `angleZ` (number): Rotation angles in radians
  - `scx`, `scy` (number): Screen-space center point
  - `fov` (number): Field-of-view scale factor
  - `edgeColor` (number): RGB565 color for edges
  - `fillColor` (number, optional): RGB565 fill color. Defaults to `0` (black).
  - `fillMode` (number, optional): 0 = wireframe only, 1 = fill only, 2 = both. Defaults to `0`.
  - `vertSize` (number, optional): Vertex dot size in pixels. Defaults to `3`.
  - `faces` (table, optional): Flat sequence of vertex-index triples `{v1, v2, v3, ...}` defining the triangles to fill
- **Returns:** None

```lua
local faces = {1,2,3, 1,3,4,  5,6,7, 5,7,8}  -- two quads as triangles
draw3DWireframeEx(verts, edges, angle, angle*0.7, 0,
    160, 160, 300, picocalc.display.WHITE, picocalc.display.BLUE, 2, 3, faces)
```
For scenes (a camera, many objects, clipping and depth order), use [picocalc.gfx3d](API-3D.md).

---

### Text Rendering

#### `picocalc.graphics.drawText(text, x, y [, font])`
Draws text using the current graphics color and background color. Optionally specify a font object.

- **Parameters:**
  - `text` (string): Text to draw
  - `x` (number): X coordinate
  - `y` (number): Y coordinate
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** (number) Pixel width of rendered text

```lua
local width = picocalc.graphics.drawText("Hello!", 10, 10)
```

---

#### `picocalc.graphics.drawTextAligned(text, x, y, alignment [, font])`
Draws text with alignment. For center/right, text is positioned relative to `x` using the real measured width of `text` in the given font (proportional-aware, not `font width * length`).

- **Parameters:**
  - `text` (string): Text to draw
  - `x` (number): X coordinate
  - `y` (number): Y coordinate
  - `alignment` (number): 0 = left, 1 = center, 2 = right
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** None

```lua
-- Draw centered text
picocalc.graphics.drawTextAligned("Centered", 160, 10, 1)
```

---

#### `picocalc.graphics.drawTextInRect(text, x, y, w, h [, alignment [, font]])`
Draws word-wrapped text within a bounding rectangle. Wrapping breaks at spaces and uses each glyph's real advance in the active font, so it wraps correctly for both monospace and proportional fonts.

- **Parameters:**
  - `text` (string): Text to draw
  - `x` (number): Left edge of bounding rectangle
  - `y` (number): Top edge of bounding rectangle
  - `w` (number): Width of bounding rectangle
  - `h` (number): Height of bounding rectangle
  - `alignment` (number, optional): 0 = left (default), 1 = center, 2 = right
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** None

```lua
-- Draw a paragraph of word-wrapped text in a box
picocalc.graphics.setColor(picocalc.display.WHITE)
picocalc.graphics.setBackgroundColor(picocalc.display.BLACK)
picocalc.graphics.drawTextInRect(
    "This is a long string that will be word-wrapped to fit within the rectangle.",
    10, 10, 200, 100
)
```

---

#### `picocalc.graphics.getTextSize(text [, font])`
Returns pixel dimensions for a single line of text, using the real per-glyph advances of the given font (proportional-aware).

- **Parameters:**
  - `text` (string): Text to measure
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** (number, number) `width, height`

```lua
local w, h = picocalc.graphics.getTextSize("Hello!")
```

---

#### `picocalc.graphics.getTextSizeForMaxWidth(text, maxWidth [, font])`
Returns pixel dimensions of word-wrapped text within `maxWidth`. Wrapping breaks at spaces using real per-glyph advances, matching `drawTextInRect`.

- **Parameters:**
  - `text` (string): Text to measure
  - `maxWidth` (number): Maximum width in pixels for word wrapping
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** (number, number) `width, height`

```lua
local w, h = picocalc.graphics.getTextSizeForMaxWidth("A long string to measure.", 200)
```

---

#### `picocalc.graphics.imageWithText(text, maxWidth, maxHeight [, bgColor [, font]])`
Renders word-wrapped text into a new image in PSRAM. Uses the current graphics color for the text foreground. The image is capped at 2048 px.

- **Parameters:**
  - `text` (string): Text to render
  - `maxWidth` (number): Maximum image width in pixels
  - `maxHeight` (number): Maximum image height in pixels
  - `bgColor` (number, optional): Background RGB565 color
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** (userdata) Image object, or `nil, errstr` on failure

```lua
picocalc.graphics.setColor(picocalc.display.WHITE)
local img = picocalc.graphics.imageWithText("Hello World", 200, 100, picocalc.display.BLACK)
if img then
    img:draw(10, 10)
end
```

---

### Image Constructor Functions

#### `picocalc.graphics.image.new(width, height)`
Creates a new blank image in PSRAM, initialized to all zeros (black).

- **Parameters:**
  - `width` (number): Image width in pixels
  - `height` (number): Image height in pixels
- **Returns:** (userdata) Image object
- **Errors:** If dimensions are invalid or memory allocation fails

```lua
local canvas = picocalc.graphics.image.new(64, 64)
```

---

#### `picocalc.graphics.image.load(path)`
Loads an image from the SD card. Supports **BMP**, **JPEG**, **PNG**, and **GIF** (first frame only) formats.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (userdata) Image object
- **Errors:** If file not found, format unsupported, or memory allocation fails

```lua
local img = picocalc.graphics.image.load("/apps/myapp/sprite.bmp")
```

---

#### `picocalc.graphics.image.loadFromBuffer(data [, len])`
Decodes an image from an in-memory buffer. The format is auto-detected from the magic bytes — BMP, JPEG, PNG, and GIF are all supported.

`data` is a string or a `sys.qmiPsramAlloc` handle (`len` is checked against its size). Decoded images are capped at 2048×2048.

- **Parameters:**
  - `data` (string or userdata): Image file data
- **Returns:** (userdata) Image object
- **Errors:** If format unsupported or decoding fails

```lua
local raw = picocalc.fs.readFile("/apps/myapp/photo.jpg")
local img = picocalc.graphics.image.loadFromBuffer(raw)
```

---

#### `picocalc.graphics.image.getInfo(path)`
Reads an image's dimensions from its header only — no full decode.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (table) `{width=number, height=number, format=string}`
- **Errors:** If the file is not a recognized image

```lua
local info = picocalc.graphics.image.getInfo("/apps/myapp/photo.jpg")
print(info.width, info.height, info.format)
```

---

#### `picocalc.graphics.image.loadRegion(path, x, y, w, h)`
Loads an image and keeps only the given sub-rectangle. The region is clamped to the image bounds.

- **Parameters:**
  - `path` (string): Absolute file path
  - `x`, `y` (number): Top-left of the region
  - `w`, `h` (number): Region dimensions in pixels
- **Returns:** (userdata) Image object
- **Errors:** If the file fails to load or the region lies outside the image

```lua
-- Load just the top-left 64x64 corner of a large image
local corner = picocalc.graphics.image.loadRegion("/apps/myapp/big.png", 0, 0, 64, 64)
```

---

#### `picocalc.graphics.image.loadScaled(path, w, h)`
Loads an image and resamples it to `w`×`h` (bilinear). Faster and lighter than loading full-size then scaling at draw time.

- **Parameters:**
  - `path` (string): Absolute file path
  - `w`, `h` (number): Target dimensions in pixels
- **Returns:** (userdata) Image object
- **Errors:** If the file fails to load

```lua
local thumb = picocalc.graphics.image.loadScaled("/apps/myapp/photo.jpg", 64, 64)
```

---

#### `picocalc.graphics.image.preload(path)`
Starts an asynchronous decode of an image on Core 1. Only one preload can be in flight at a time. Poll for completion with `pollPreload()`.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (boolean) `true` if the preload was started

```lua
picocalc.graphics.image.preload("/apps/myapp/level2.png")
```

---

#### `picocalc.graphics.image.pollPreload()`
Checks the state of the pending preload.

- **Returns:** (userdata or nil, boolean) `image, ready` — `ready` is `true` once the preload has finished (successfully or not); `image` is the decoded image, or `nil` while still decoding or on failure

```lua
while true do
    local img, ready = picocalc.graphics.image.pollPreload()
    if ready then
        if img then levelArt = img end
        break
    end
    picocalc.sys.sleep(10)
end
```

---

#### `picocalc.graphics.image.cancelPreload()`
Cancels the pending preload.

- **Returns:** None

---

#### `picocalc.graphics.image.getSupportedFormats()`
Returns a table of supported image format names.

- **Returns:** (table) Array of format strings (e.g., `{"BMP", "JPEG", "PNG", "GIF"}`)

---

### Image Methods

All methods are called on image objects with colon syntax.

#### `img:getSize()`
Returns the image dimensions.

- **Returns:** (number, number) `width, height`

```lua
local w, h = img:getSize()
```

---

#### `img:getMetadata()`
Returns image metadata without touching pixel data.

- **Returns:** (table) `{width=number, height=number, transparentColor=number?, storage=string}` — `transparentColor` is only present if one is set; `storage` is `"psram"`

```lua
local meta = img:getMetadata()
print(meta.width, meta.height, meta.storage)
```

---

#### `img:draw(x, y [, flipOpts [, srcRect]])`
Draws the image (or a sub-rectangle of it) to the framebuffer.

- **Parameters:**
  - `x` (number): Destination X coordinate
  - `y` (number): Destination Y coordinate
  - `flipOpts` (boolean or table, optional): If `true`, flips horizontally. If table: `{flipX=bool, flipY=bool}`
  - `srcRect` (table, optional): Source sub-rectangle `{x=int, y=int, w=int, h=int}`
- **Returns:** None

```lua
-- Draw full image
img:draw(10, 20)

-- Draw horizontally flipped
img:draw(10, 20, true)

-- Draw with flip options
img:draw(10, 20, {flipX = true, flipY = false})

-- Draw a sub-region
img:draw(10, 20, false, {x = 0, y = 0, w = 32, h = 32})
```

---

#### `img:drawAnchored(x, y, anchorX, anchorY)`
Draws the image positioned relative to an anchor point.

- **Parameters:**
  - `x` (number): Anchor X coordinate
  - `y` (number): Anchor Y coordinate
  - `anchorX` (number): Horizontal anchor (0.0 = left, 0.5 = center, 1.0 = right)
  - `anchorY` (number): Vertical anchor (0.0 = top, 0.5 = center, 1.0 = bottom)
- **Returns:** None

```lua
-- Draw centered on screen
img:drawAnchored(160, 160, 0.5, 0.5)
```

---

#### `img:drawTiled(x, y, width, height)`
Tiles the image to fill a rectangular area.

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Fill area width
  - `height` (number): Fill area height
- **Returns:** None

```lua
-- Tile a pattern across a 200x100 area
img:drawTiled(0, 30, 200, 100)
```

---

#### `img:drawScaled(x, y, scale [, angle])`
Draws the image scaled and optionally rotated.

- **Parameters:**
  - `x` (number): Destination X coordinate
  - `y` (number): Destination Y coordinate
  - `scale` (number): Scale factor (1.0 = original size, 2.0 = double)
  - `angle` (number, optional): Rotation angle in radians. Defaults to 0.
- **Returns:** None

```lua
img:drawScaled(160, 160, 2.0)        -- 2x zoom
img:drawScaled(160, 160, 1.0, 0.785) -- Rotate 45°
```

---

#### `img:drawScaledNN(x, y, scale)`
Draws the image scaled using nearest-neighbor interpolation. Faster and sharper for integer scaling (pixel art).

Samples source pixels exactly for any scale (older firmware sampled the wrong column at non-power-of-two scales, e.g. every third column at scale 3).

- **Parameters:**
  - `x` (number): Destination X coordinate
  - `y` (number): Destination Y coordinate
  - `scale` (number): Integer scale factor (e.g., 2 for 2x size)
- **Returns:** None

```lua
img:drawScaledNN(10, 10, 3)  -- 3x zoom (pixel art style)
```

---

#### `image:drawStretched(x, y, w, h [, srcRect])`
Draws the image (or the `srcRect` part of it) stretched to exactly `w` × `h` pixels, nearest-neighbour, honouring the image's transparent colour. Unlike `drawScaledNN`, the size need not be an integer multiple, and the result stays pixel-sharp (no filtering).

- **Parameters:**
  - `x`, `y` (number): Top-left corner on screen
  - `w`, `h` (number): Destination size; `0` or less draws nothing
  - `srcRect` (table, optional): `{x=, y=, w=, h=}` source rectangle; clamped to the image
- **Returns:** None

```lua
sheet:drawStretched(100, 80, 48, 24, { x = 16, y = 0, w = 16, h = 8 })  -- one frame, 3x
```

---

#### `img:copy()`
Creates a deep copy of the image.

- **Returns:** (userdata) New image object with identical pixel data

```lua
local backup = img:copy()
```

---

## picocalc.graphics.sprite

Sprite system for game and graphics applications. Sprites are 2D objects that can be positioned, scaled, rotated, and managed through a global sprite manager.

**Object lifetimes:** a sprite added with `add()` stays alive while displayed (no need to keep a reference); `remove()`/`removeSprites()`/`removeAll()` release it. `add()` is idempotent. A sprite keeps its image, stencil and tilemap alive; a spritesheet its image; a tilemap its tileset; an animation loop its frames. Sprite `width`/`height` are bounds only; drawing uses the image's own size.

### Constructor Functions

#### `picocalc.graphics.sprite.new([image])`
Creates a new sprite, optionally with an image.

- **Parameters:**
  - `image` (userdata, optional): Image object from `graphics.image.new()` or `graphics.image.load()`
- **Returns:** (userdata) Sprite object

```lua
local sprite = picocalc.graphics.sprite.new(myImage)
local emptySprite = picocalc.graphics.sprite.new()
```

---

#### `picocalc.graphics.sprite.update()`
Updates and draws all sprites in the manager. Call once per frame.

- **Returns:** None

```lua
while true do
    -- Update sprite positions
    sprite1:moveBy(1, 0)
    picocalc.graphics.sprite.update()
end
```

---

#### `picocalc.graphics.sprite.spriteCount()`
Returns the number of sprites in the manager.

- **Returns:** (number) Count of sprites

```lua
local count = picocalc.graphics.sprite.spriteCount()
```

---

#### `picocalc.graphics.sprite.getAllSprites()`
Returns a table containing all sprites in the manager.

- **Returns:** (table) Array of sprite objects

```lua
local all = picocalc.graphics.sprite.getAllSprites()
for i, s in ipairs(all) do
    print(i, s.x, s.y)
end
```

---

#### `picocalc.graphics.sprite.removeAll()`
Removes all sprites from the manager.

- **Returns:** None

```lua
picocalc.graphics.sprite.removeAll()
```

---

#### `picocalc.graphics.sprite.removeSprites(spriteArray)`
Removes multiple sprites from the manager.

- **Parameters:**
  - `spriteArray` (table): Array of sprite objects to remove
- **Returns:** None

```lua
picocalc.graphics.sprite.removeSprites({sprite1, sprite2, sprite3})
```

---

#### `picocalc.graphics.sprite.performOnAllSprites(callback)`
Calls a function on each sprite in the manager. It iterates a snapshot of the display list and propagates errors from the callback.

- **Parameters:**
  - `callback` (function): Function to call with each sprite as argument
- **Returns:** None

```lua
picocalc.graphics.sprite.performOnAllSprites(function(s)
    s:setVisible(false)
end)
```

---

#### `picocalc.graphics.sprite.querySpritesAtPoint(x, y)`  
#### `picocalc.graphics.sprite.querySpritesAtPoint(point)`
Queries all sprites at a specific point.

- **Parameters:**
  - `x`, `y` (number): Coordinates, OR
  - `point` (table): `{x=number, y=number}`
- **Returns:** (table) Array of sprites at that point

```lua
local hits = picocalc.graphics.sprite.querySpritesAtPoint(160, 100)
```

---

#### `picocalc.graphics.sprite.querySpritesInRect(x, y, w, h)`
#### `picocalc.graphics.sprite.querySpritesInRect(rect)`
Queries all sprites within a rectangular area.

- **Parameters:**
  - `x`, `y`, `w`, `h` (number), OR
  - `rect` (table): `{x=number, y=number, w=number, h=number}`
- **Returns:** (table) Array of sprites in the rect

```lua
local hits = picocalc.graphics.sprite.querySpritesInRect(0, 0, 100, 100)
```

---

#### `picocalc.graphics.sprite.querySpritesAlongLine(x1, y1, x2, y2)`
Queries all sprites that intersect a line segment.

- **Parameters:**
  - `x1`, `y1` (number): Start point
  - `x2`, `y2` (number): End point
- **Returns:** (table) Array of sprite objects

```lua
local hits = picocalc.graphics.sprite.querySpritesAlongLine(0, 0, 320, 320)
```

---

#### `picocalc.graphics.sprite.querySpriteInfoAlongLine(x1, y1, x2, y2)`
Queries all sprites that intersect a line segment, returning detailed intersection info.

- **Parameters:**
  - `x1`, `y1` (number): Start point
  - `x2`, `y2` (number): End point
- **Returns:** (table) Array of intersection info tables: `{sprite, x, y}`


All methods are called on sprite objects with colon syntax.

#### `sprite:add()` / `sprite:addSprite()`
Adds the sprite to the global sprite manager.

- **Returns:** None

```lua
mySprite:add()
```

---

#### `sprite:remove()` / `sprite:removeSprite()`
Removes the sprite from the global sprite manager.

- **Returns:** None

```lua
mySprite:remove()
```

---

#### `sprite:draw([x, y])`
Draws the sprite to the framebuffer immediately (not via the manager).

- **Parameters:**
  - `x`, `y` (number, optional): Position to draw. Defaults to sprite's stored position.
- **Returns:** None

```lua
mySprite:draw()  -- Draw at sprite.x, sprite.y
mySprite:draw(50, 100)  -- Draw at custom position
```

---

#### `sprite:update()`
Updates and draws a single sprite (alternative to using the manager).

- **Returns:** None

```lua
mySprite:update()
```

---

#### `sprite:setImage(image [, flip [, scale [, yscale]]])`
Sets the sprite's image.

- **Parameters:**
  - `image` (userdata): Image object
  - `flip` (boolean, optional): Enable horizontal flip
  - `scale` (number, optional): Scale factor
  - `yscale` (number, optional): Y scale factor (defaults to scale)
- **Returns:** None

```lua
sprite:setImage(myImage, false, 1.5)
```

---

#### `sprite:getImage()`
Gets the sprite's image (the image object itself).

- **Returns:** (userdata or nil) Image object

---

#### `sprite:moveTo(x, y)`
Moves the sprite to absolute coordinates.

- **Parameters:**
  - `x`, `y` (number): New position
- **Returns:** None

```lua
sprite:moveTo(100, 50)
```

---

#### `sprite:moveBy(dx, dy)`
Moves the sprite by a relative offset.

- **Parameters:**
  - `dx`, `dy` (number): Offset to add to current position
- **Returns:** None

```lua
sprite:moveBy(5, -3)
```

---

#### `sprite:getPosition()`
Gets the sprite's position.

- **Returns:** (number, number) `x, y`

```lua
local x, y = sprite:getPosition()
```

---

#### `sprite:setZIndex(z)`
Sets the sprite's Z-index for draw ordering.

- **Parameters:**
  - `z` (number): Z-order value
- **Returns:** None

```lua
sprite:setZIndex(10)
```

---

#### `sprite:getZIndex()`
Gets the sprite's Z-index.

- **Returns:** (number) Z-index

---

#### `sprite:setVisible(flag)`
Shows or hides the sprite.

- **Parameters:**
  - `flag` (boolean): `true` to show, `false` to hide
- **Returns:** None

```lua
sprite:setVisible(false)
```

---

#### `sprite:isVisible()`
Checks if the sprite is visible.

- **Returns:** (boolean)

---

#### `sprite:setCenter(x, y)`
Sets the sprite's rotation/scale center point.

- **Parameters:**
  - `x`, `y` (number): Center point relative to sprite origin
- **Returns:** None

```lua
sprite:setCenter(16, 16)  -- Center of a 32x32 sprite
```

---

#### `sprite:getCenter()`
Gets the sprite's center point.

- **Returns:** (number, number) `centerX, centerY`

---

#### `sprite:getCenterPoint()`
Gets the sprite's center point as a table.

- **Returns:** (table) `{x, y}`

---

#### `sprite:setSize(width, height)`
Sets the sprite's dimensions.

- **Parameters:**
  - `width`, `height` (number): New dimensions
- **Returns:** None

```lua
sprite:setSize(64, 64)
```

---

#### `sprite:getSize()`
Gets the sprite's dimensions.

- **Returns:** (number, number) `width, height`

---

#### `sprite:setScale(scale [, yScale])`
Sets the sprite's scale factor(s).

- **Parameters:**
  - `scale` (number): Scale factor
  - `yScale` (number, optional): Y scale (defaults to scale)
- **Returns:** None

```lua
sprite:setScale(2.0)     -- Uniform 2x
sprite:setScale(2.0, 1.5)  -- Non-uniform
```

---

#### `sprite:getScale()`
Gets the sprite's scale factors.

- **Returns:** (number, number) `scaleX, scaleY`

---

#### `sprite:setScaleNN(scale)`
Sets an integer scale factor using nearest-neighbor interpolation. Sharp for pixel art.

- **Parameters:**
  - `scale` (number): Positive integer scale (1, 2, 3...)
- **Returns:** None

---

#### `sprite:setTransparentColor(color)`
Sets a per-sprite transparent color, overriding the global transparent color.

- **Parameters:**
  - `color` (number or nil): RGB565 color value, or `nil` to use the global setting.
- **Returns:** None

---

#### `sprite:setRotation(angle [, scale [, yScale]])`
Sets the sprite's rotation angle in radians.

- **Parameters:**
  - `angle` (number): Rotation in radians
  - `scale` (number, optional): Scale X
  - `yScale` (number, optional): Scale Y
- **Returns:** None

```lua
sprite:setRotation(math.pi / 4)  -- 45 degrees
```

---

#### `sprite:getRotation()`
Gets the sprite's rotation angle.

- **Returns:** (number) Rotation in radians

---

#### `sprite:copy()`
Creates a copy of the sprite.

- **Returns:** (userdata) New sprite object

```lua
local clone = sprite:copy()
```

---

#### `sprite:setSourceRect(x, y, w, h)`
Extracts a sub-region of the sprite's image as its new frame. Subsequent `draw()` or `update()` calls will only render this region. This effectively creates an internal copy of the frame data.

- **Parameters:**
  - `x`, `y` (number): Top-left coordinate in source image
  - `w`, `h` (number): Dimensions of the frame to extract
- **Returns:** None

```lua
-- Select a 32x32 frame from a larger sheet
sprite:setSourceRect(32, 0, 32, 32)
```

---

#### `sprite:clearSourceRect()`
Resets the sprite to use its full source image.

- **Returns:** None

---

#### `sprite:setUpdatesEnabled(flag)`
Enables or disables automatic updates when using `graphics.sprite.update()`.

- **Parameters:**
  - `flag` (boolean): Enable/disable updates
- **Returns:** None

---

#### `sprite:updatesEnabled()`
Checks if updates are enabled.

- **Returns:** (boolean)

---

#### `sprite:setAlwaysRedraw(flag)`
Enables or disables forced redraw for this sprite every frame, even if it hasn't moved.

- **Parameters:**
  - `flag` (boolean)
- **Returns:** None

---

#### `sprite:getAlwaysRedraw()`
- **Returns:** (boolean)

---

#### `sprite:markDirty()`
Explicitly marks the sprite as needing to be redrawn in the next update. Accepted but not applied (no effect yet).

- **Returns:** None

---

#### `sprite:addDirtyRect(x, y, width, height)`
Adds a dirty rectangle for partial redrawing. Accepted but not applied (no effect yet).

---

#### `sprite:setRedrawsOnImageChange(flag)`
Sets whether the sprite automatically redraws when its image is changed.

- **Parameters:**
  - `flag` (boolean)
- **Returns:** None

---

#### `sprite:setTag(tag)`
Sets a user-defined tag value.

- **Parameters:**
  - `tag` (number): Tag value
- **Returns:** None

```lua
sprite:setTag(123)
```

---

#### `sprite:getTag()`
Gets the sprite's tag.

- **Returns:** (number) Tag value

---

#### `sprite:setImageFlip(flip)`
Sets horizontal flip.

- **Parameters:**
  - `flip` (boolean): Flip enabled
- **Returns:** None

---

#### `sprite:getImageFlip()`
Gets horizontal flip state.

- **Returns:** (boolean)

---

#### `sprite:setIgnoresDrawOffset(flag)`
Sets whether the sprite ignores global draw offsets. Accepted but not applied (no effect yet).

- **Parameters:**
  - `flag` (boolean): Ignore offset
- **Returns:** None

---

#### `sprite:setBounds(x, y, w, h)` / `sprite:setBounds(rect)`
Sets the sprite's bounding box for culling.

- **Parameters:**
  - `x`, `y`, `w`, `h` (number), OR
  - `rect` (table): `{x, y, w, h}`
- **Returns:** None

---

#### `sprite:getBounds()`
Gets the sprite's bounding box.

- **Returns:** (number, number, number, number) `x, y, w, h`

---

#### `sprite:getBoundsRect()`
Gets the sprite's bounding box as a table.

- **Returns:** (table) `{x, y, w, h}`

---

#### `sprite:setOpaque(flag)`
Sets whether the sprite is opaque (affects collision detection).

- **Parameters:**
  - `flag` (boolean): Opaque state
- **Returns:** None

---

#### `sprite:isOpaque()`
Gets the sprite's opaque state.

- **Returns:** (boolean)

---

#### `sprite:setCollisionsEnabled(flag)`
Enables collision detection for this sprite.

- **Parameters:**
  - `flag` (boolean): Enable collisions
- **Returns:** None

---

#### `sprite:collisionsEnabled()`
Checks if collisions are enabled.

- **Returns:** (boolean)

---

#### `sprite:setCollideRect(x, y, w, h)` / `sprite:setCollideRect(rect)`
Sets the sprite's collision rectangle.

- **Parameters:**
  - `x`, `y`, `w`, `h` (number), OR
  - `rect` (table): `{x, y, w, h}`
- **Returns:** None

---

#### `sprite:getCollideRect()`
Gets the sprite's collision rectangle.

- **Returns:** (number, number, number, number) `x, y, w, h`

---

#### `sprite:getCollideBounds()`
Gets the absolute collision bounds (sprite position + collide rect).

- **Returns:** (number, number, number, number) `x, y, w, h`

---

#### `sprite:clearCollideRect()`
Resets the collision rectangle to the full sprite size.

- **Returns:** None

---

#### `sprite:setClipRect(x, y, w, h)` / `sprite:setClipRect(rect)`
Sets a clipping rectangle for the sprite, relative to the screen.

- **Parameters:**
  - `x`, `y`, `w`, `h` (number), OR
  - `rect` (table): `{x, y, w, h}`
- **Returns:** None

---

#### `sprite:clearClipRect()`
Clears the clipping rectangle.

- **Returns:** None

---

#### `sprite:overlappingSprites()`
Gets all sprites that overlap with this sprite.

- **Returns:** (table) Array of overlapping sprites

```lua
local hits = mySprite:overlappingSprites()
```

---

#### `sprite:allOverlappingSprites()`
Gets all sprite pairs that overlap each other.

- **Returns:** (table) Array of `{sprite1, sprite2}` pairs

---

#### `sprite:setGroups(groups)`
Sets collision group membership.

- **Parameters:**
  - `groups` (number): Bitmask of groups
- **Returns:** None

---

#### `sprite:setCollidesWithGroups(groups)`
Sets which collision groups this sprite collides with.

- **Parameters:**
  - `groups` (number): Bitmask
- **Returns:** None

---

#### `sprite:setGroupMask(mask)` / `sprite:getGroupMask()`
Sets/gets the group mask.

---

#### `sprite:setCollidesWithGroupsMask(mask)` / `sprite:getCollidesWithGroupsMask()`
Sets/gets the collision-with-groups mask.

---

#### `sprite:resetGroupMask()` / `sprite:resetCollidesWithGroupsMask()`
Resets the group masks to 0.

---

#### `sprite:checkCollisions(x, y)` / `sprite:checkCollisions(point)`
Checks if a point collides with the sprite's collision rect.

- **Parameters:**
  - `x`, `y` (number), OR
  - `point` (table): `{x, y}`
- **Returns:** (boolean) True if collision

---

#### `sprite:moveWithCollisions(goalX, goalY)`
Moves the sprite toward a goal position, sliding along the collision rects of other sprites. Requires collisions to be enabled (`setCollisionsEnabled(true)`).

- **Parameters:**
  - `goalX`, `goalY` (number): Desired position
- **Returns:** (number, number, table) `actualX, actualY, collisions` — the position reached, plus a table of collision records `{sprite, other, type, x, y, normal = {x, y}, touch}`

```lua
sprite:setCollisionsEnabled(true)
local x, y, hits = sprite:moveWithCollisions(goalX, goalY)
for i, c in ipairs(hits) do
    print("bumped", c.type, c.normal.x, c.normal.y)
end
```

---

#### `sprite:collisionResponse()`
Returns the sprite's collision response type.

- **Returns:** (string) Response type (default `"slide"`)

---

#### `sprite:setStencilImage(image)`
Sets a stencil image for the sprite. Accepted but not applied (no effect yet).

- **Parameters:**
  - `image` (userdata): Image object to use as the stencil
- **Returns:** None

---

#### `sprite:setTilemap(tilemap)`
Assigns a tilemap to this sprite. When set, the sprite renders the tilemap instead of a single image. The sprite's position acts as the tilemap scroll offset. Pass `nil` to clear.

- **Parameters:**
  - `tilemap` (userdata or nil): Tilemap object, or `nil` to clear
- **Returns:** None

```lua
local tm = picocalc.graphics.tilemap.new(tilesetImage, 16, 16)
tm:setSize(20, 20)
local bgSprite = picocalc.graphics.sprite.new()
bgSprite:setTilemap(tm)
bgSprite:moveTo(0, 0)
bgSprite:add()
```

---

#### `picocalc.graphics.sprite.addWallSprites(tilemap, wallIDs [, xOffset [, yOffset]])`
Creates invisible collision sprites for each tile whose index appears in the `wallIDs` table. Useful for tile-based collision detection with the sprite system.

- **Parameters:**
  - `tilemap` (userdata): Tilemap object
  - `wallIDs` (table): Array of tile indices that are solid/collidable
  - `xOffset` (number, optional): X offset applied to all wall sprite positions
  - `yOffset` (number, optional): Y offset applied to all wall sprite positions
- **Returns:** (number) Count of wall sprites created

```lua
-- Create wall collision sprites for tile indices 1, 2, and 5
local wallCount = picocalc.graphics.sprite.addWallSprites(tilemap, {1, 2, 5})
print("Created " .. wallCount .. " wall sprites")
```

---

#### `picocalc.graphics.sprite.spriteWithText(text, maxWidth, maxHeight [, bgColor [, font]])`
Creates a sprite with text rendered into it. Uses the current graphics color for the text foreground. The image is capped at 2048 px.

- **Parameters:**
  - `text` (string): Text to render
  - `maxWidth` (number): Maximum image width in pixels
  - `maxHeight` (number): Maximum image height in pixels
  - `bgColor` (number, optional): Background RGB565 color
  - `font` (userdata, optional): Font object from `graphics.font.new()`
- **Returns:** (userdata) Sprite object with text image

```lua
picocalc.graphics.setColor(picocalc.display.WHITE)
local label = picocalc.graphics.sprite.spriteWithText("Score: 0", 120, 16, picocalc.display.BLACK)
label:moveTo(10, 10)
label:add()
```

---

### Sprite Properties

Sprites support direct property access via Lua:

```lua
sprite.x = 100      -- Set X position
sprite.y = 50      -- Set Y position
sprite.width = 64  -- Set width
sprite.height = 64 -- Set height
sprite.z = 10      -- Set Z-index
sprite.visible = true   -- Show/hide
sprite.scale = 2.0      -- Set uniform scale
sprite.scale_nn = 1     -- Get/set integer NN scale
sprite.rotation = 0.5   -- Set rotation (radians)
sprite.tag = 123        -- Set tag
sprite.image            -- Get image (userdata or nil)
```

---

## picocalc.graphics.spritesheet

Spritesheet support for sprite animations. A spritesheet is a single image containing multiple animation frames.

### Constructor Functions

#### `picocalc.graphics.spritesheet.new([image])`
Creates a new spritesheet, optionally with a base image.

- **Parameters:**
  - `image` (userdata, optional): Image object containing the spritesheet
- **Returns:** (userdata) Spritesheet object

```lua
local ss = picocalc.graphics.spritesheet.new(myImage)
```

---

#### `picocalc.graphics.spritesheet.newGrid(image, cols, rows, frameWidth, frameHeight)`
Creates a spritesheet from a grid layout. Automatically calculates frame positions.

- **Parameters:**
  - `image` (userdata): Image object containing the spritesheet
  - `cols` (number): Number of columns
  - `rows` (number): Number of rows
  - `frameWidth` (number): Width of each frame in pixels
  - `frameHeight` (number): Height of each frame in pixels
- **Returns:** (userdata) Spritesheet object

```lua
-- 4x4 grid of 32x32 pixel frames
local ss = picocalc.graphics.spritesheet.newGrid(spritesheetImg, 4, 4, 32, 32)
```

---

### Spritesheet Methods

#### `spritesheet:addFrame(x, y, width, height)`
Manually adds a frame to the spritesheet.

- **Parameters:**
  - `x`, `y` (number): Top-left position of frame in the image
  - `width`, `height` (number): Dimensions of the frame
- **Returns:** (number) Frame index (0-based)

```lua
ss:addFrame(0, 0, 32, 32)   -- Frame 0
ss:addFrame(32, 0, 32, 32)  -- Frame 1
```

---

#### `spritesheet:getFrameCount()`
Returns the total number of frames.

- **Returns:** (number) Frame count

```lua
local count = ss:getFrameCount()
```

---

#### `spritesheet:getFrame(index)`
Returns the bounds of a specific frame.

- **Parameters:**
  - `index` (number): Frame index (0-based)
- **Returns:** (table) `{x, y, w, h}` or nil if invalid

```lua
local frame = ss:getFrame(0)
print(frame.x, frame.y, frame.w, frame.h)
```

---

#### `spritesheet:getImage()`
Returns the base image.

- **Returns:** (userdata or nil) Image object

---

#### `spritesheet:drawFrame(frameIndex, x, y [, flip])`
Draws a specific frame to the screen.

- **Parameters:**
  - `frameIndex` (number): Which frame to draw
  - `x`, `y` (number): Screen position
  - `flip` (boolean, optional): Horizontal flip
- **Returns:** None

```lua
ss:drawFrame(0, 100, 100)  -- Draw frame 0 at (100,100)
ss:drawFrame(1, 100, 100, true)  -- Flipped
```

---

### Example: Simple Animation

```lua
local spritesheet = picocalc.graphics.image.load("/apps/myapp/character.png")
local ss = picocalc.graphics.spritesheet.newGrid(spritesheet, 4, 4, 32, 32)

local frame = 0
local timer = 0

while true do
    picocalc.display.clear(picocalc.display.BLACK)
    
    timer = timer + 1
    if timer > 5 then  -- Change frame every 5 frames
        frame = (frame + 1) % ss:getFrameCount()
        timer = 0
    end
    
    ss:drawFrame(frame, 144, 144)
    picocalc.display.flush()
end
```

---

## picocalc.graphics.font

Custom font loading and text rendering. Font objects can be passed to text rendering functions throughout the graphics API.

### Constructor Functions

#### `picocalc.graphics.font.new(nameOrPath)`
Creates a new font object, either from one of the four built-in fonts or by loading a custom `.pfn` file from the SD card.

- **Parameters:**
  - `nameOrPath` (string): One of the built-in names `"6x8"`, `"8x12"`, `"scientifica"`, `"scientifica-bold"`, or an absolute path to a `.pfn` file inside the app's sandbox
- **Returns:** (userdata) Font object. **Raises a Lua error** (never returns `nil`) if the path is outside the sandbox or the file fails to load (missing, bad magic, size mismatch, no free slot).

A font object created from a path owns a loaded font slot (see [Custom fonts](#custom-fonts)) and frees it automatically when the object is garbage-collected. Every font an app has loaded, whether via `font.new(path)` or `picocalc.display.loadFont`, is also freed when the app exits, so leaving objects to the garbage collector is safe.

```lua
local font = picocalc.graphics.font.new("8x12")
local custom = picocalc.graphics.font.new(APP_DIR .. "/fonts/custom.pfn")
```

---

### Font Methods

#### `font:drawText(x, y, text, fg [, bg])`
Draws text at the specified position using this font.

- **Parameters:**
  - `x` (number): X coordinate
  - `y` (number): Y coordinate
  - `text` (string): Text to draw
  - `fg` (number): Foreground RGB565 color
  - `bg` (number, optional): Background RGB565 color
- **Returns:** None

```lua
font:drawText(10, 10, "Hello!", picocalc.display.WHITE)
```

---

#### `font:drawTextAligned(x, y, text, alignment, fg [, bg])`
Draws text with alignment using this font.

- **Parameters:**
  - `x` (number): X coordinate
  - `y` (number): Y coordinate
  - `text` (string): Text to draw
  - `alignment` (number): 0 = left, 1 = center, 2 = right
  - `fg` (number): Foreground RGB565 color
  - `bg` (number, optional): Background RGB565 color
- **Returns:** None

```lua
font:drawTextAligned(160, 10, "Centered", 1, picocalc.display.WHITE)
```

---

#### `font:drawTextInRect(x, y, w, h, text [, alignment [, fg [, bg]]])`
Draws word-wrapped text within a bounding rectangle using this font. Wrapping breaks at spaces and uses each glyph's real advance, so it works correctly for both monospace and proportional fonts.

- **Parameters:**
  - `x` (number): Left edge of bounding rectangle
  - `y` (number): Top edge of bounding rectangle
  - `w` (number): Width of bounding rectangle
  - `h` (number): Height of bounding rectangle
  - `text` (string): Text to draw
  - `alignment` (number, optional): 0 = left (default), 1 = center, 2 = right
  - `fg` (number, optional): Foreground RGB565 color
  - `bg` (number, optional): Background RGB565 color
- **Returns:** None

```lua
local font = picocalc.graphics.font.new("scientifica")
font:drawTextInRect(10, 10, 200, 100, "This text will wrap within the rectangle.", 0,
    picocalc.display.WHITE, picocalc.display.BLACK)
```

---

#### `font:getHeight()`
Returns the font's glyph height in pixels.

- **Returns:** (number) Height in pixels

---

#### `font:getWidth()`
Returns the font's maximum glyph advance in pixels. For a proportional font this is the widest glyph, not every glyph's width — use `font:getTextWidth` to measure a specific string.

- **Returns:** (number) Max advance in pixels

---

#### `font:getTextWidth(text)`
Returns the pixel width of text rendered in this font.

- **Parameters:**
  - `text` (string): Text to measure
- **Returns:** (number) Width in pixels

```lua
local w = font:getTextWidth("Hello")
```

---

#### `font:getName()`
Returns the string the font was created with: one of the built-in names, or the `.pfn` path for a loaded font.

- **Returns:** (string) Font name or path

---

### Custom fonts

`picocalc.graphics.font.new(path)` and `picocalc.display.loadFont(path)` both load fonts from a `.pfn` file — a small, little-endian, proportional-capable bitmap font container built by `tools/mkfont.py`.

#### `.pfn` file format

12-byte header followed by two arrays:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"PFNT"` |
| 4 | 1 | version, must be 1 |
| 5 | 1 | flags, bit0 = proportional (widths array is meaningful) |
| 6 | 1 | first |
| 7 | 1 | last (>= first) |
| 8 | 1 | height, 1..64 |
| 9 | 1 | max_width, 1..64 |
| 10 | 1 | stride, must equal `(max_width + 7) / 8` |
| 11 | 1 | reserved, 0 |
| 12 | count | widths, u8 each, `count = last - first + 1` |
| 12 + count | count * height * stride | bitmaps |

The widths array is always present so the loader never branches on `flags`; a monospace file simply repeats `max_width` for every glyph. Total file length must equal `12 + count + count*height*stride` exactly. The largest legal file is `12 + 256 + 256*64*8 = 131,340` bytes (~128 KB), which is also the effective per-font PSRAM budget for a loaded font.

A byte outside `first..last` draws as a hollow box rather than a substitute glyph.

#### `tools/mkfont.py`

Builds a `.pfn` from a BDF font, a TTF/OTF font, or a fixed-grid PNG sheet:

```
python3 tools/mkfont.py SRC OUT.pfn [--size N] [--cell WxH] [--range FIRST-LAST]
                                     [--proportional] [--spacing N] [--dump]
```

- `SRC` ending `.bdf`: parsed by a built-in BDF reader; each glyph's `DWIDTH` gives its advance, and `--proportional` is implied automatically when advances vary.
- `SRC` ending `.ttf` / `.otf`: rasterised with Pillow's `ImageFont` at `--size` pixels, thresholded at 50%; advance comes from the font's own metrics. (Requires Pillow; BDF and PNG sources need only the standard library.)
- `SRC` ending `.png`: a fixed grid sheet, cell size given by `--cell WxH`, glyphs read left to right then top to bottom starting at `first`; a pixel is set when its luminance is above 50%.
- `--proportional` on a monospace source trims each glyph's advance down to its ink extent plus `--spacing` (default 1) and packs it to the left of its cell. Space keeps half the cell width, minimum 2 px.
- `--range` selects the codepoint range to emit, default `0x20-0x7E`.
- `--dump` prints every glyph as ASCII art with its advance, for eyeballing before shipping the file.

The tool and the on-device renderer both support glyphs up to 64 px wide and 64 px tall (`stride = ceil(max_width / 8)` bytes per row).

```lua
-- Loaded on the app's SD card at fonts/custom.pfn, built with:
--   python3 tools/mkfont.py fonts/custom.bdf apps/myapp/fonts/custom.pfn --proportional
local font = picocalc.graphics.font.new(APP_DIR .. "/fonts/custom.pfn")
font:drawText(10, 10, "Loaded from SD!", picocalc.display.WHITE)
```

---

## picocalc.graphics.tilemap

Tilemap system for tile-based game worlds. Tilemaps use an image atlas as a tileset and render visible tiles to the screen with scroll offset support.

### Constructor Functions

#### `picocalc.graphics.tilemap.new(image, tileWidth, tileHeight)`
Creates a new tilemap using an image atlas as the tileset. Tiles are indexed 1-based (0 = empty/transparent). Tiles are extracted from the tileset image left-to-right, top-to-bottom.

- **Parameters:**
  - `image` (userdata): Tileset image (image atlas containing all tile graphics)
  - `tileWidth` (number): Width of each tile in pixels
  - `tileHeight` (number): Height of each tile in pixels
- **Returns:** (userdata) Tilemap object

```lua
local tileset = picocalc.graphics.image.load(APP_DIR .. "/tileset.png")
local tilemap = picocalc.graphics.tilemap.new(tileset, 16, 16)
```

---

### Tilemap Methods

#### `tilemap:setSize(width, height)`
Allocates the tile grid. Maximum 128x128 tiles. Tile data is stored in PSRAM.

- **Parameters:**
  - `width` (number): Grid width in tiles
  - `height` (number): Grid height in tiles
- **Returns:** None

```lua
tilemap:setSize(40, 30)
```

---

#### `tilemap:setTileAtPosition(x, y, tileIndex)`
Sets the tile at a grid position. Tile index is 1-based; 0 = empty/transparent.

- **Parameters:**
  - `x` (number): Grid X position
  - `y` (number): Grid Y position
  - `tileIndex` (number): Tile index (1-based, 0 = empty)
- **Returns:** None

```lua
tilemap:setTileAtPosition(5, 3, 1)  -- Place tile 1 at grid (5,3)
tilemap:setTileAtPosition(5, 4, 0)  -- Clear tile at grid (5,4)
```

---

#### `tilemap:getTileAtPosition(x, y)`
Returns the tile index at a grid position. Returns 0 for empty or out-of-bounds positions.

- **Parameters:**
  - `x` (number): Grid X position
  - `y` (number): Grid Y position
- **Returns:** (number) Tile index (0 = empty)

```lua
local tile = tilemap:getTileAtPosition(5, 3)
```

---

#### `tilemap:getSize()`
Returns the tilemap dimensions in tiles.

- **Returns:** (number, number) `width, height`

```lua
local w, h = tilemap:getSize()
```

---

#### `tilemap:getTileSize()`
Returns the tile dimensions in pixels.

- **Returns:** (number, number) `tileWidth, tileHeight`

```lua
local tw, th = tilemap:getTileSize()
```

---

#### `tilemap:getPixelSize()`
Returns the total tilemap dimensions in pixels.

- **Returns:** (number, number) `pixelWidth, pixelHeight`

```lua
local pw, ph = tilemap:getPixelSize()
```

---

#### `tilemap:draw(scrollX, scrollY)`
Draws visible tiles to the framebuffer with scroll offset. Only draws tiles visible on the 320x320 screen.

- **Parameters:**
  - `scrollX` (number): Horizontal scroll offset in pixels
  - `scrollY` (number): Vertical scroll offset in pixels
- **Returns:** None

```lua
tilemap:draw(cameraX, cameraY)
```

---

### Example: Tile-Based Game World

```lua
-- Load tileset and create tilemap
local tileset = picocalc.graphics.image.load(APP_DIR .. "/tileset.png")
local tilemap = picocalc.graphics.tilemap.new(tileset, 16, 16)
tilemap:setSize(40, 30)

-- Fill with grass (tile 1), add some walls (tile 2)
for y = 0, 29 do
    for x = 0, 39 do
        tilemap:setTileAtPosition(x, y, 1)  -- grass
    end
end
-- Add border walls
for x = 0, 39 do
    tilemap:setTileAtPosition(x, 0, 2)   -- top wall
    tilemap:setTileAtPosition(x, 29, 2)  -- bottom wall
end

-- Create wall collision sprites for tile index 2
local wallCount = picocalc.graphics.sprite.addWallSprites(tilemap, {2})

-- Scroll camera
local scrollX, scrollY = 0, 0
while true do
    picocalc.display.clear(picocalc.display.BLACK)
    tilemap:draw(scrollX, scrollY)
    picocalc.graphics.sprite.update()
    picocalc.display.flush()
end
```

---

## Game development APIs

Building a game? These pages cover APIs that pair well with the graphics functions above:

- **API Game** — camera, scene management, and save helpers
- **API TCP** — raw TCP/TLS sockets
- **API Zip** — ZIP archive extraction
- **API JSON** — JSON encoding/decoding
