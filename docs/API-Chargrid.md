---
title: "API Chargrid"
---

## picocalc.chargrid

A grid of character cells, drawn in one call. Each cell holds a glyph code and a foreground and background palette index. Use it for text screens (an RPG's map window, status panel and dialogue), ASCII art, and animated scenes built from character ramps.

The grid keeps its cells in memory, and `grid:draw()` renders its window in C: a full 320×320 screen of 6×8 cells takes about 9 ms on the device. Your Lua code writes cells (one at a time, a row at a time from strings or tables, or as text) and draws once per frame.

### Cells, glyphs and colours

- **Coordinates** are 0-based cells: column `x`, row `y`. Writes outside the grid are dropped; reads outside return `0, 0, 0`.
- **Colours** are indices into the grid's own 256-entry palette of RGB565 values. The default is the xterm-256 layout: 0-15 the classic colours, 16-231 a 6×6×6 colour cube (`16 + 36r + 6g + b`), 232-255 greys. New cells are glyph 0 (empty), fg 15 (white), bg 0 (black). Change an entry with `grid:setColor()` and every cell using it changes at the next draw (palette cycling).
- **Glyph codes** are one byte per cell:

| Codes | Constants | Glyph |
|---|---|---|
| `0x00` | `EMPTY` | Nothing: the cell's background only |
| `0x01-0x04` | `DOT1`-`DOT4` | Dots of growing size: `·` `•` `●` `⬤` |
| `0x05-0x08` | `SHADE1`-`SHADE4` | Shades `░` `▒` `▓` `█` |
| `0x09-0x0C` | `HALF_TOP`, `HALF_BOTTOM`, `HALF_LEFT`, `HALF_RIGHT` | Half blocks `▀` `▄` `▌` `▐` |
| `0x0D-0x17` | `BOX_H`, `BOX_V`, `BOX_TL`, `BOX_TR`, `BOX_BL`, `BOX_BR`, `BOX_LT`, `BOX_RT`, `BOX_TT`, `BOX_BT`, `BOX_X` | Box drawing `─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼` |
| `0x18-0x1B` | `TRI_UP`, `TRI_RIGHT`, `TRI_DOWN`, `TRI_LEFT` | Triangles `▲ ▶ ▼ ◀` |
| `0x1C-0x1F` | | Reserved (empty) |
| `0x20-0xFF` | | The grid's font; a code the font lacks draws its hollow box |

Codes `0x01-0x1B` are drawn at the grid's own cell size, so every grid has them, with or without a font. `grid:defineGlyph()` replaces any code from `0x01` up with your own bitmap (a tree, a door, a potion).

A grid shows at most 256 different glyphs; it is not general Unicode (no CJK or emoji).

### Text

`print`, `printWrapped`, `stamp`, `setRamp` (string form) and a one-character string glyph argument read **UTF-8** and map each character to a code:

1. `·•●⬤░▒▓█▀▄▌▐▲▶▼◀` and the box-drawing characters go to the codes above. Double, heavy and rounded box lines become the single-line shapes.
2. ASCII is itself.
3. U+00A0-U+00FF become their Latin-1 byte, for fonts that cover it.
4. Anything else becomes `?`.

A byte that does not begin a valid UTF-8 sequence is used as a raw code, so a font's own `0x80-0xFF` glyphs stay reachable. In `print`, `printWrapped` and `stamp`, `\n` starts a new line and other control bytes (and 0x7F) are skipped. `setRow`, `setRect` and integer glyph arguments take raw codes.

### Cell size and fonts

A grid's cell size comes from its font (`max width × height`), or is given explicitly: 1-16 pixels wide, 1-32 tall. A glyph smaller than the cell is centred in it; a larger one is cropped. The grid copies the glyphs when the font is set, so the font may be unloaded afterwards.

| Font | Cell | Full-screen grid |
|---|---|---|
| `picocalc.display.FONT_6X8` (0) | 6×8 | 53×40 |
| `picocalc.display.FONT_8X12` (1) | 8×12 | 40×26 |
| `picocalc.display.FONT_SCIENTIFICA` (2) | 6×12 | 53×26 |
| No font, 4×4 cells | 4×4 | 80×80 (shapes and defined glyphs only) |

### Drawing and performance

`grid:draw()` redraws every cell of its window on every call, so it is always correct whatever else drew on the screen. It is clipped to the display clip rect. To keep a mostly static screen idle, skip both `draw` and `flush` while `grid:isDirty()` is false. Whenever you `flush()`, draw the grid first: the back buffer holds the frame from two flushes ago.

Measured on the device (`-O2`, one call), a full-window draw takes:

| Grid | Time |
|---|---|
| 53×40 at 6×8 | ~9 ms |
| 40×26 at 8×12 | ~7.7 ms |
| 80×80 at 4×4 | ~13.4 ms |

Cells with an even width at an even x take a two-pixels-per-store fast path; odd widths and odd positions draw pixel by pixel and cost more.

For per-cell maths in Lua, compute a row of levels into a reused table and hand it to `grid:shadeRow()`, one call per row (about 0.9 µs per cell).

### Functions

#### `picocalc.chargrid.new(cols, rows [, fontId [, cellW, cellH]])`
Creates a grid.

- **Parameters:**
  - `cols`, `rows` (number): grid size in cells, each 1-1024, `cols * rows` at most 65,536
  - `fontId` (integer, optional): a built-in font id or one from `picocalc.display.loadFont`; nil for no font
  - `cellW`, `cellH` (number, optional): the cell size, 1-16 × 1-32. Required without a font; defaults to the font's size
- **Returns:** (`PicoDeckCharGrid`) the grid
- **Errors:** sizes out of range; a font id that is not loaded; out of memory

```lua
local g = picocalc.chargrid.new(53, 40, picocalc.display.FONT_6X8)  -- the whole screen
local dots = picocalc.chargrid.new(80, 40, nil, 4, 4)               -- shapes only
```

### Grid methods

All arguments named `x`, `y`, `w`, `h`, `px`, `py`, `sx`, `sy`, `dx`, `dy` are numbers (floats round). Colours and codes must be integers.

#### `grid:getSize()`
- **Returns:** `cols, rows`

#### `grid:getCellSize()`
- **Returns:** `cellW, cellH` in pixels

#### `grid:setFont(fontId)`
Rebuilds the glyphs `0x20-0xFF` from the font, keeping the cell size and any defined glyphs. Errors if the font id is not loaded.

#### `grid:defineGlyph(code, rows)`
Replaces glyph `code` (1-255) with a bitmap: `rows` holds `cellH` integers, and in each row bit `cellW - 1` is the leftmost pixel. A defined glyph survives `setFont`.

```lua
-- a 6x8 tree on code 0x80
g:defineGlyph(0x80, { 0x08, 0x1C, 0x3E, 0x1C, 0x3E, 0x08, 0x08, 0x00 })
```

#### `grid:setPalette(colors [, first])`
Sets palette entries `first` (default 0) onward from a table of RGB565 integers (0-65535). `first + #colors` may be at most 256.

#### `grid:setColor(index, color)` / `grid:getColor(index)`
Sets or returns one palette entry (`index` 0-255).

#### `grid:setRamp(glyphs [, dither])`
Sets the ramp `shadeRow` maps levels through: a UTF-8 string (`" .:-=+*#%@"`, `"·•●"`, or raw codes such as `"\0\1\2\3\4"`) or a table of codes, 1-256 entries. With `dither`, a 4×4 ordered dither spreads levels between entries.

#### `grid:setColorRamp(indices [, dither])`
Sets the colour ramp `shadeRow` maps tones (or levels) through: a table of 1-256 palette indices.

#### `grid:clear([glyph [, fg [, bg]]])`
Fills every cell; defaults glyph 0, fg 15, bg 0.

#### `grid:set(x, y, glyph [, fg [, bg]])` / `grid:get(x, y)`
Sets one cell (glyph: a code or a one-character string; nil colours are kept) or returns `glyph, fg, bg`.

#### `grid:fill(x, y, w, h [, glyph [, fg [, bg]]])`
Fills a rectangle; nil leaves that plane alone (`fill(x, y, w, h, nil, 9)` recolours).

#### `grid:setRow(y, glyphs [, fgs [, bgs [, x]]])`
Writes cells from `x` (default 0) along row `y`. Each plane argument is a string (one byte per cell), a table of integers, one integer (fills), or nil (left alone). The sequences must have equal lengths; with none, the integers fill to the end of the row.

```lua
g:setRow(3, "HP", { 9, 9 })        -- two red cells
g:setRow(4, nil, nil, 17)          -- the row's background, navy
```

#### `grid:getRow(y [, x [, n]])`
Returns three strings `glyphs, fgs, bgs` for `n` cells from `x` (default: to the end of the row). Cells outside the grid are left out.

#### `grid:setRect(x, y, w, h, glyphs [, fgs [, bgs]])`
Like `setRow` for a rectangle: sequences hold `w * h` entries, row by row.

#### `grid:shadeRow(y, levels [, tones [, x]])`
Shades cells from `x` (default 0) along row `y`.

- `levels` (table of numbers 0-1, or nil) pick glyphs through the ramp.
- `tones` (table, or nil) pick foreground colours through the colour ramp.
- Without tones, levels pick the colours too (when a colour ramp is set).
- Out-of-range values clamp, and NaN is 0.

Errors when a needed ramp is not set.

```lua
g:setRamp(" .:-=+*#%@")
g:setColorRamp({ 16, 52, 88, 124, 160, 196, 202, 208, 214, 220, 226, 231 })
local row = {}
for y = 0, 39 do
    for x = 1, 53 do row[x] = heat[y][x] / 36 end
    g:shadeRow(y, row)
end
```

#### `grid:print(x, y, text [, fg [, bg]])`
Writes UTF-8 text; `\n` continues at `x` on the next row. Returns the x after the text. Nil colours are kept.

#### `grid:printWrapped(x, y, w, text [, fg [, bg]])`
Word-wraps text into rows `w` cells wide. Returns the number of rows used.

#### `grid:box(x, y, w, h [, style [, fg [, bg]]])`
Draws a frame (nothing when `w` or `h` is below 2), leaving the inside untouched. The style is `picocalc.chargrid.BOX_SINGLE` (the default), `BOX_BLOCK` or `BOX_ASCII`.

#### `grid:stamp(x, y, art [, fg [, bg [, transparent]]])`
Writes multi-line UTF-8 art. Cells whose character is `transparent` (a one-character string or a code; default `" "`) are left alone.

#### `grid:blit(src, sx, sy, w, h, dx, dy [, transparentGlyph])`
Copies a rectangle of cells (all three planes) from grid `src`, which may be this grid, to `(dx, dy)`. Overlapping copies are safe; source cells holding `transparentGlyph` are skipped. Use it for a camera window onto a large map.

#### `grid:scroll(dx, dy [, glyph [, fg [, bg]]])`
Shifts the contents and fills the cells that come into view (defaults glyph 0, fg 15, bg 0).

#### `grid:draw(px, py [, sx, sy, w, h])`
Draws cells `sx..sx+w-1` × `sy..sy+h-1` (default: the whole grid) with cell `(sx, sy)` at pixel `(px, py)`, inside the display clip rect.

#### `grid:isDirty()`
- **Returns:** (boolean) whether any write happened since the last `draw`

#### `grid:getStats()`
- **Returns:** a table `{ cells = n, us = t }` for the last `draw`: the cells it touched and its time in microseconds

### Example: an RPG screen

```lua
local CG = picocalc.chargrid
local map, view = CG.new(200, 120, 0), CG.new(53, 40, 0)
-- … fill map once …
local function frame(px, py, message)
    local cx, cy = math.max(0, px - 26), math.max(0, py - 16)
    view:clear()
    view:blit(map, cx, cy, 53, 33, 0, 0)                 -- the camera window
    view:set(px - cx, py - cy, "@", 226)
    view:box(0, 33, 53, 7)
    view:printWrapped(1, 34, 51, message, 252)
    view:draw(1, 0)
    picocalc.display.flush()
end
```
