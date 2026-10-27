---
title: "API Chargrid"
---

## picocalc.chargrid

A grid of character cells, drawn in one call. Each cell holds a glyph code and a foreground and background palette index. Use it for text screens (an RPG's map window, status panel and dialogue), ASCII art, and animated scenes built from character ramps.

The grid keeps its cells in memory, and `grid:draw()` renders its window in C: a full 320×320 screen of 6×8 cells takes about 3.1 ms on the device. Your Lua code writes cells (one at a time, a row at a time from strings or tables, or as text) and draws once per frame.

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
| `/system/lib/fonts/spleen-5x8.pfn` via `picocalc.display.loadFont` | 5×8 | 64×40 |
| No font, 4×4 cells | 4×4 | 80×80 (shapes and defined glyphs only) |

Spleen 5×8 (BSD 2-Clause, Frederic Cambus; source and licence in the repo's `sdk/fonts/spleen/`) gives 64-column grids. Release packaging does not install `/system/lib` (see [API System and Config](API-System-and-Config.md)): copy `system/lib/fonts/spleen-5x8.pfn` to the SD card, or ship a copy in your app directory and load it from `APP_DIR`. The font is BSD 2-Clause: copies distributed on SD cards or in app directories must ship `sdk/fonts/spleen/LICENSE` alongside the `.pfn`.

```lua
local id = picocalc.display.loadFont("/system/lib/fonts/spleen-5x8.pfn")
local g = picocalc.chargrid.new(64, 40, id)
```

### Drawing and performance

`grid:draw()` redraws every cell of its window on every call, so it is always correct whatever else drew on the screen. It is clipped to the display clip rect. To keep a mostly static screen idle, skip both `draw` and `flush` while `grid:isDirty()` is false. Whenever you `flush()`, draw the grid first: the back buffer holds the frame from two flushes ago.

Measured on the device (`-O2`, one call), a full-window draw takes:

| Grid | Time |
|---|---|
| 53×40 at 6×8 | ~3.1 ms |
| 40×26 at 8×12 | ~2.5 ms |
| 64×40 at 5×8 | ~3.4 ms |
| 80×80 at 4×4 | ~5 ms |

Cells of every width take a two-pixels-per-store fast path at any x (odd widths store their last column on its own; pair stores may be unaligned: 53×40 at 6×8 drawn at x = 1 takes ~3.3 ms against ~3.1 ms at x = 0). Only cells cut by the clip rect draw pixel by pixel.

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
Sets the colour ramp `shadeRow` maps tones (or levels) through: a table of 1-256 palette indices. With `dither`, the same 4×4 ordered dither spreads tones between colour-ramp entries.

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

- `levels` (table of numbers 0-1, or nil) pick glyphs through the ramp. Non-number entries and holes count as 0.
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
Draws cells `sx..sx+w-1` × `sy..sy+h-1` (`w` and `h` default to the grid's `cols` and `rows`, i.e. to the end of the grid from `sx`, `sy`; cells past the edge are clipped) with cell `(sx, sy)` at pixel `(px, py)`, inside the display clip rect.

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

## Shaders

A shader is a short program that runs once per cell, in C, over a rectangle of a grid. It is how to animate a whole scene (sky, water, fire, lighting) without a Lua loop over cells. Lua does the per-frame work that grows with columns, rows and objects: a curtain's edge per column, the player's position. The shader does the per-cell maths.

```lua
local sh, err = picocalc.chargrid.shader([[
  uniform t
  level = noise(x * 0.1 + t, y * 0.2)    -- through the grid's ramp
  tone  = level                          -- through the colour ramp
]])
assert(sh, err)
function frame(t)
  sh:set("t", t)
  sh:run(grid)
  grid:draw(0, 0)
end
```

### The language

- **Statements:** one per line, or separated by `;`. A statement is `name = expression`, or a declaration: `uniform a, b`, `array a, b`, `layer a, b`. `--` starts a comment, and brackets may span lines.
- **No control flow:** every statement runs for every cell, in order. Use `select(c, a, b)` and the comparisons instead of `if`.
- **Names** must be declared or assigned before they are read, so a typo is a compile error, not a silent zero.
- **Numbers:** `12`, `0.5`, `.5`, `1e-3`, `0x1F`. Arithmetic is single precision.
- **Operators**, loosest first:
  - `or`;
  - `and`;
  - `< <= > >= == ~=`;
  - `+ -`;
  - `* / %`;
  - unary `-` and `not`;
  - `^`, right-associative, so `-x^2` is `-(x^2)`.

  Comparisons, `and`, `or` and `not` give 1 or 0, and a value is true when it is not 0. `a % b` is `a - floor(a / b) * b` in single precision (GLSL's `mod`): like Lua's `%` for ordinary values (the result takes the sign of `b`), but not at the edges: `x % inf` is NaN (Lua: `x` for `x ≥ 0`), and `6 % 0.1` is 0, as `6 / 0.1` rounds to 60 (Lua: about 0.1).
- Expressions nest at most 16 deep.

**Inputs:**

| Name | Value |
|---|---|
| `x`, `y` | The cell's column and row in the grid |
| `u`, `v` | `x / cols`, `y / rows` |
| `cols`, `rows` | The grid's size |
| `glyph`, `fg`, `bg` | The cell's glyph code and palette indices before this run changes them. Assigning one makes later reads see the new value |
| uniforms | `uniform t`: one number for every cell, set with `sh:set("t", value)` (0 until set) |
| arrays | `array edge`: `edge[i]` reads entry `floor(i)`, clamped to the array (0 if it was never set). Set with `sh:setArray` |
| layers | `layer land`: `land.glyph`, `land.fg`, `land.bg` read another grid at the same `(x, y)`, and 0 outside it. Pass the grids to `sh:run` in declaration order |

`x`, `y`, `u`, `v`, `cols`, `rows`, uniforms, arrays and layers are read-only.

**Outputs:** assign these names, and the run writes each cell in this order:
1. `keep`: if ≥ 0.5, the cell is left untouched.
2. **Glyph:** `glyph`, a code rounded and clamped to 0-255. Otherwise `level` (0..1) through the grid's ramp (`grid:setRamp`, dithered if set). Otherwise unchanged.
3. **fg:** `fg`, an index. Otherwise `r`, `g`, `b` (each 0..1; any unassigned one is 0), matched to the nearest of the grid's 256 palette colours. Otherwise `tone` through the colour ramp. Otherwise `level` through the colour ramp. Otherwise unchanged.
4. **bg:** `bg`, an index. Otherwise `bgtone` through the colour ramp. Otherwise unchanged.

NaN, infinities and out-of-range values never reach the grid: an index output that is NaN gives 0, and the ramps treat NaN as 0.

These ten names (`keep`, `glyph`, `level`, `fg`, `r`, `g`, `b`, `tone`, `bg`, `bgtone`) are outputs wherever they are assigned, so do not use one as a temporary: `r = dist(x, y, px, py)` makes `r` an output, and the run then sets fg from the nearest palette colour. Give temporaries other names (`d = dist(...)`).

**Built-ins:**
- `sin cos abs floor ceil fract sqrt exp log pow min max`;
- `clamp(x, lo, hi)`, `mix(a, b, t)`, `smooth(e0, e1, x)` (smoothstep), `step(edge, x)`, `select(c, a, b)`, `dist(x0, y0, x1, y1)`, `atan2(y, x)`;
- `hash(x, y [, seed])`, `noise(x, y [, seed])` and `fbm(x, y, octaves [, seed])`: for the arguments `picocalc.noise` accepts, exactly its results. Where it raises, the shader computes instead: NaN and infinite coordinates give a value in [0, 1), and a seed is floored (a seed beyond the 32-bit range saturates, where Lua wraps). `octaves` must be a constant 1-8.

The maths functions are fast polynomial approximations: sin and cos within 4e-6 for |x| ≤ 10, exp within 1e-6 relative. They give the same results in the simulator and on the device.

**Limits:**
- source: 4096 bytes;
- 512 operations as written, counted before unused statements are dropped and `a * b + c` is fused (constant expressions are folded and do not count): beyond that, "program is too long";
- 255 instructions after optimisation, and 32 values live at once;
- 237 distinct constants after optimisation;
- 119 names of the program's own (variables, assigned outputs, uniforms, arrays and layers; the inputs do not count), each at most 31 characters;
- 16 uniforms, 8 arrays of 1024 entries, 4 layers.

Constant expressions are computed once at compile time, unused statements cost nothing, and `a * b + c` is one instruction. Division by a constant is a multiplication by its reciprocal.

**The nearest colour** for `r, g, b`: colours are bucketed 16 levels per channel, and each bucket's answer is cached on the grid (4.5 KB, allocated on the first such run). `setPalette` and `setColor` reset the cache. If there is no memory for it, each cell searches the palette (slower but the same colours), and the next `setPalette` or `setColor` lets a run try again. To keep a scene to its own colours, fill all 256 palette entries with them; the lowest index wins a tie.

### Shader functions

#### `picocalc.chargrid.shader(source)`
Compiles a shader. Returns it, or `nil` and an error message such as `"line 3:12: unknown name 'hme'"`. It never raises for a bad program.

#### `shader:set(name, value)`
Sets a declared uniform to a finite number. Raises for an unknown name.

#### `shader:setArray(name, values [, n])`
Copies `values[1..n]` (default `#values`, at most 1024) into a declared array. Shader index 0 is `values[1]`, and entries past the end of the table read 0. Raises for an unknown name.

#### `shader:run(grid [, x, y, w, h] [, layer1, ...])`
Runs over the cell rect (default: the whole grid), clipped to the grid, and marks the grid dirty. The layers follow in declaration order, and their count must match. `shader:run(grid, land)` runs over the whole grid with one layer.

#### `shader:getStats()`
Returns `{ cells, us, instructions, registers }`: the last run's cells and time, and the compiled program's size.

### Performance

A run costs roughly a fixed time per cell per instruction, plus each built-in's own time (`fbm` costs as much as its octaves of `noise`). `shader:getStats()` shows both. Keep work that is the same for a whole column or row in Lua (an `array`), and work that is the same for every cell in a `uniform`.

Measured on the device (one `run`, whole grid):

| Program | Grid | Time |
|---|---|---|
| Aurora-class sky: 3 `fbm(…, 3)`, 2 `noise`, 3 `exp`, `sin`, 2 `smooth`, `r,g,b` (61 instructions) | 80×40 | ~27 ms |
| The same | 80×80 | ~51 ms |
| Torch: `dist`, `clamp`, `tone` (8 instructions) | 53×40 | ~1.9 ms |

Built-ins, per call: `sin` ~290 ns, `exp` ~280 ns, `log` ~410 ns, `sqrt` ~130 ns, `pow` ~840 ns, `atan2` ~500 ns, `noise` ~290 ns, `fbm(…, 3)` ~1,030 ns. At 15 fps (66 ms per frame), about 6,600 cells of an aurora-class program fit alongside drawing and Lua.

A run works each row in strips of up to 80 cells, and each strip has a fixed cost of about ten cells' work, as does each `run` call. So cover a shape with a few whole-row rects (with `keep` sparing the cells between) rather than many small ones.

These times hold while no video player is open: a run then keeps its working values in on-chip SRAM that the video decoder lends. While one is open they stay in PSRAM, in strips of 32 cells, and the same runs take longer: the aurora-class program above about a quarter, a 140-instruction sky nearly twice as long. The results are the same either way.

`noise` and `fbm` cost less when neighbouring cells share a lattice cell (the per-call times above step about a tenth of a lattice cell per grid cell): at a frequency of one or more per cell each call works out new corner hashes. Run times vary by a few percent with where the program and the grids land in memory (the device's XIP cache holds both code and PSRAM data).

### Example: a torch-lit map window

```lua
local view = CG.new(53, 40, 0)
view:setColorRamp({ 236, 238, 240, 242, 244, 246, 248, 250 })  -- dim greys
local torch = CG.shader([[
  uniform px, py
  d = dist(x, y, px, py)
  keep = d < 4                          -- full colour near the player
  tone = clamp(1 - (d - 4) / 9, 0, 1)   -- fading to dark grey beyond
]])
-- each frame: blit the map window, then
torch:set("px", px - camX); torch:set("py", py - camY)
torch:run(view, 0, 0, 53, 33)
```

## picocalc.noise

Deterministic 2-D noise for scenes and procedural maps: the same inputs give the same results in the simulator and on the device. The hash is the one [ascii.rest](https://ascii.rest) pieces use, so with seed 0 a ported piece looks the same; a seed gives independent noise, and the same seed always gives the same map. Every result is in [0, 1). Coordinates are computed in single precision (about 7 significant digits).

Errors: coordinates must be finite numbers (NaN or an infinity raises), a seed an integer (`2.5` raises; integer-valued floats such as `3.0` are fine), and `octaves` an integer 1-8.

#### `picocalc.noise.hash(x, y [, seed])`
The lattice hash of the integer point (`floor(x)`, `floor(y)`): `h = x·374761393 + y·668265263 + seed·1442695041` (32-bit wrapping), `h = (h ⊕ (h >> 13)) · 1274126177`, `h = h ⊕ (h >> 16)`, result `(h >> 8) / 2²⁴`. `seed` is an integer (default 0, wrapped to 32 bits).

#### `picocalc.noise.value(x, y [, seed])`
Value noise: the hashes of the four surrounding lattice points, blended with smoothstep weights. Equal to `hash` at lattice points.

#### `picocalc.noise.fbm(x, y, octaves [, seed])`
Fractal noise: `octaves` (an integer 1-8) layers of `value` at doubling frequency (lacunarity 2) and halving weight (gain 0.5), divided by the sum of the weights. Every octave uses the same seed, as ascii.rest does.

```lua
local N = picocalc.noise
-- A reproducible cave: wall where 2-octave noise is high.
for y = 0, 39 do
    for x = 0, 52 do
        if N.fbm(x * 0.15, y * 0.15, 2, worldSeed) > 0.55 then map:set(x, y, "#", 244) end
    end
end
```

The chargrid shader has the same three as built-ins (`hash`, `noise`, `fbm`), with identical results for the arguments these functions accept (the shader does not raise: see its built-ins).
