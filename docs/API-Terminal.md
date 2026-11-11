---
title: "API Terminal"
---

`picocalc.terminal` provides a full-featured terminal emulator widget. Each terminal has its own cell grid, scrollback buffer, cursor, and colour state. Multiple terminals can be created but only one should render to the display at a time.

A cell is 6x12 px, so a terminal has its own font and metrics: it does not use the display's current font, and it ignores the display's clip rect.

## Creating a terminal

```
local term = picocalc.terminal.new([cols [, rows [, scrollback]]])
```

- `cols` (number, default **53**): grid width in characters, 1-53
- `rows` (number, default **26**): grid height in characters, 1-26
- `scrollback` (number, default **1000**): number of lines kept in scrollback history, at most 5000. Zero or less means the default.

Out-of-range sizes raise an error. Returns a terminal object. Raises an error if allocation fails (PSRAM OOM).

**Where it draws.** A terminal of up to 22 rows starts at y = 28 with its text at x = 4, which leaves room for a header above it and a footer below. A taller one moves up just enough to fit (a full 26 rows starts at y = 8 and ends at the bottom of the screen), and one of 53 columns starts at x = 2. Use [`setRenderBounds`](#render-bounds) to place it yourself, for example under `ui.drawHeader`.

```lua
-- Full-screen terminal with default size
local term = picocalc.terminal.new()

-- Smaller terminal, 500-line scrollback
local term = picocalc.terminal.new(40, 20, 500)
```

---

## Writing and clearing

#### `term:write(str)`
Write a string to the terminal at the current cursor position. Supports ANSI escape sequences (cursor movement, colour codes, erase sequences, etc.). The cursor advances automatically; lines wrap and the display scrolls when the bottom row is reached. The whole string is written; a NUL byte in it does nothing, as on a real terminal.

```lua
term:write("Hello, world!\n")
term:write("\27[31mRed text\27[0m\n")   -- ANSI colour
```

#### `term:clear()`
Clear all cells and reset the cursor to (0, 0).

---

## Cursor

#### `term:setCursor(x, y)`
Move the cursor to column `x`, row `y` (0-based).

#### `term:getCursor()` → `x, y`
Return the current cursor column and row (0-based).

#### `term:setCursorVisible(visible)`
Show or hide the cursor.

#### `term:setCursorBlink(blink)`
Enable or disable cursor blinking.

---

## Colours

#### `term:setColors(fg, bg)`
Set the foreground and background colours (RGB565) for what is written next. An escape sequence's reset (`ESC[0m`, `ESC[39m`, `ESC[49m`) goes back to white on black, not to these.

#### `term:getColors()` → `fg, bg`
Return the current foreground and background colours, which escape sequences change too.

#### ANSI colour codes
`term:write()` reads these codes. A colour number N means the same colour as palette index N of a new `picocalc.chargrid` grid ([API Chargrid](API-Chargrid.md)).

| Code | Sets |
|---|---|
| `\27[30m`-`\27[37m`, `\27[40m`-`\27[47m` | Foreground, background: colours 0-7 |
| `\27[90m`-`\27[97m`, `\27[100m`-`\27[107m` | Foreground, background: colours 8-15 |
| `\27[38;5;Nm`, `\27[48;5;Nm` | Foreground, background: colour N, 0-255 (above that the code is ignored) |
| `\27[38;2;R;G;Bm`, `\27[48;2;R;G;Bm` | Foreground, background: any colour, reduced to RGB565 |
| `\27[39m`, `\27[49m` | Foreground, background: back to white, black |

Colours 0-15 are the VGA colours:

| N | Colour | Codes | RGB | RGB565 |
|---|---|---|---|---|
| 0 | Black | 30, 40 | 0, 0, 0 | `0x0000` |
| 1 | Red | 31, 41 | 170, 0, 0 | `0xA800` |
| 2 | Green | 32, 42 | 0, 170, 0 | `0x0540` |
| 3 | Yellow | 33, 43 | 170, 85, 0 | `0xAAA0` |
| 4 | Blue | 34, 44 | 0, 0, 170 | `0x0015` |
| 5 | Magenta | 35, 45 | 170, 0, 170 | `0xA815` |
| 6 | Cyan | 36, 46 | 0, 170, 170 | `0x0555` |
| 7 | White | 37, 47 | 170, 170, 170 | `0xAD55` |
| 8 | Grey | 90, 100 | 85, 85, 85 | `0x52AA` |
| 9 | Bright red | 91, 101 | 255, 85, 85 | `0xFAAA` |
| 10 | Bright green | 92, 102 | 85, 255, 85 | `0x57EA` |
| 11 | Bright yellow | 93, 103 | 255, 255, 85 | `0xFFEA` |
| 12 | Bright blue | 94, 104 | 85, 85, 255 | `0x52BF` |
| 13 | Bright magenta | 95, 105 | 255, 85, 255 | `0xFABF` |
| 14 | Bright cyan | 96, 106 | 85, 255, 255 | `0x57FF` |
| 15 | Bright white | 97, 107 | 255, 255, 255 | `0xFFFF` |

Colours 16-231 are a 6×6×6 cube (`16 + 36r + 6g + b`, where 0-5 stand for the levels 0, 95, 135, 175, 215, 255), and 232-255 are greys (8, 18, ... 238). These 240 follow xterm's 256-colour layout; 0-15 do not.

---

## Scrolling

Two different things scroll. **The grid** holds the live text, and `term:scroll` shifts its contents. **The view** looks back through the scrollback without changing anything: use `setScrollbackOffset` for that.

#### `term:scroll(lines)`
Shift the grid's contents, destructively, and leave the cursor where it is. Positive moves them up: each top row goes into the scrollback and a blank row comes in at the bottom, as when output scrolls. Negative moves them down: a blank row comes in at the top and the bottom row is lost. Colours go with the rows.

#### `term:getScrollbackCount()` → `n`
Return the number of lines currently in the scrollback buffer.

#### `term:getScrollbackLine(n)` → `str`
Return scrollback line `n` (1-based) as a plain string. Raises an error if `n` is out of range.

#### `term:getScrollbackOffset()` → `offset`
Return the current scrollback view offset (0 = showing live output).

#### `term:setScrollbackOffset(offset)`
View the text `offset` lines back from the live grid, or set 0 to return to it. An offset past the oldest line shows the oldest line first. Nothing is clamped, so `getScrollbackOffset` returns what you set: keep it between 0 and `getScrollbackCount()`. Writing does not change the offset; `clear` sets it back to 0.

While the offset is above 0, `render` and `renderDirty` draw the lines across the full width, with no gutter, scrollbar or cursor. A line keeps its colours.

---

## Rendering

Call one of these once per frame after writing content. They draw into the back buffer, so a present shows the result.

#### `term:render()`
Redraw every cell to the framebuffer. Use when the whole screen may be stale (e.g. first frame, font change), or simply every frame, followed by `picocalc.display.flush()`.

#### `term:renderDirty()`
Redraw only rows marked dirty since the last render, and the cursor. If a `flush()` has swapped the framebuffers since the last render, the back buffer holds an older frame, so it redraws everything instead: with `flush()` it costs what `render()` costs. To save the work, present with `picocalc.display.flushRows()`, which does not swap, and send only the rows that changed:

```lua
local D = picocalc.display
local term = picocalc.terminal.new(53, 10)
term:setRenderBounds(0, 120)     -- so a row's y is row * 12
term:write("Status:\n")
term:render()
D.flushRows(0, 119)

for i = 1, 8 do
    term:write("step " .. i .. "\n")
    local first, last = term:getDirtyRange()
    term:renderDirty()
    if first >= 0 then
        D.flushRows(first * 12, last * 12 + 11)
    end
    picocalc.sys.sleep(50)
end
```

With no dirty rows and no swap it draws nothing at all, the cursor included, so a blinking cursor needs `render()`.

#### `term:markAllDirty()`
Mark every row as dirty so the next `renderDirty()` redraws everything.

#### `term:isFullDirty()` → `bool`
Returns `true` if all rows are marked dirty.

#### `term:getDirtyRange()` → `first, last`
Return the first and last dirty row indices (inclusive), or `-1, -1` when no row is dirty. Useful for partial redraws.

---

## Font

#### `term:setFont(name)`
Switch the terminal font. Valid values (note the underscore: `graphics.font.new` spells it `"scientifica-bold"`):

| Name | Description |
|------|-------------|
| `"scientifica"` | Default bitmap font |
| `"scientifica_bold"` | Bold variant |

#### `term:getFont()` → `name`
Return the current font name string.

---

## Line numbers

Draw a gutter on the left side with line numbers.

#### `term:setLineNumbers(enabled)`
Enable or disable the line number gutter (default: off).

#### `term:setLineNumberStart(n)`
Set the first line number displayed (default: 1).

#### `term:setLineNumberCols(cols)`
Set the gutter width in columns, 2-10 (default: 5).

#### `term:setLineNumberColors(fg, bg)`
Set gutter foreground and background colours (RGB565).

#### `term:getContentCols()` → `n`
Return the number of columns available for content: the terminal's columns, less the gutter's, less one for the scrollbar when it is on. Use this when calculating text layout so content does not overlap the gutter.

```lua
term:setLineNumbers(true)
term:setLineNumberCols(4)
term:setLineNumberColors(0x7BEF, 0x2945)  -- grey on dark
local usable = term:getContentCols()      -- e.g. 49 when gutter is 4
```

---

## Scrollbar

Draw a vertical scrollbar on the right side.

#### `term:setScrollbar(enabled)`
Enable or disable the scrollbar (default: off).

#### `term:setScrollbarColors(bg, thumb)`
Set the scrollbar track background and thumb colours (RGB565).

#### `term:setScrollbarWidth(width)`
Set the scrollbar width in pixels (default: 4).

#### `term:setScrollInfo(totalLines, scrollPosition)`
Tell the scrollbar how many total logical lines exist and where the current viewport starts. Call this each frame when the document length or scroll position changes.

```lua
term:setScrollbar(true)
term:setScrollbarWidth(6)
term:setScrollInfo(#lines, scroll_offset)
```

---

## Grid dimensions

#### `term:getCols()` → `n`
Return the total number of columns in the terminal grid.

#### `term:getRows()` → `n`
Return the total number of rows in the terminal grid.

---

## Render bounds

#### `term:setRenderBounds(y_start, y_end)`
Place the terminal between two pixel rows of the screen. Useful for embedding a terminal in a portion of the screen alongside other UI elements.

Row 0 of the grid is drawn at `y_start`. A row that does not fully fit above `y_end` is not drawn, by any call: text, gutter, cursor and scrollback view alike. So size the grid to fit, `(y_end - y_start) / 12` rows: the rows below the end still exist, and a cursor there does not show. Text that scrolls still scrolls at the grid's last row, not the last one showing.

- `y_start` (number): First pixel row to draw on (0-based; below 0 reads as 0)
- `y_end` (number): The pixel row below the last one to draw on (above 320 reads as 320). If it is not below `y_start`, the call changes nothing.

With no call to this, a Lua terminal stays where [`new`](#creating-a-terminal) placed it, which fits it on the screen at any size: a full 26 rows of 53 columns start at y = 8 and x = 2. A [native](#differences-from-the-native-terminal-api) terminal renders from y 28 to y 302 at x 4 however many rows it has, so one that uses the whole screen must set its bounds.

```lua
-- Under ui.drawHeader's 21 rows, above ui.drawFooter's 18: 23 text rows.
local term = picocalc.terminal.new(53, 23)
term:setRenderBounds(21, 302)
```

---

## Cell access

Direct cell manipulation for syntax highlighting and custom rendering.

#### `term:setCell(x, y, ch)`
Set the character at column `x`, row `y` (0-based). Out-of-bounds coordinates are silently ignored.

- `x` (number): Column
- `y` (number): Row
- `ch` (string): The character to place in the cell (only its first byte is used)

#### `term:getCell(x, y)` → `ch`
Return the character at column `x`, row `y` as a single-character string. Returns `" "` for out-of-bounds coordinates or empty cells.

#### `term:setCellColors(x, y, fg, bg)`
Set the foreground and background colours (RGB565) of a single cell. Marks the row dirty.

- `x` (number): Column
- `y` (number): Row
- `fg` (number): Foreground colour (RGB565)
- `bg` (number): Background colour (RGB565)

#### `term:setRowColors(y, fgTable, bgTable [, startX [, count]])`
Batch-set foreground and background colours for a range of cells in row `y`. Both `fgTable` and `bgTable` are arrays of RGB565 values. Marks the row dirty.

- `y` (number): Row
- `fgTable` (table): Array of foreground colours
- `bgTable` (table): Array of background colours
- `startX` (number, optional): Starting column (default 0)
- `count` (number, optional): Number of cells to set (default: all columns)

```lua
-- Highlight columns 0-9 of row 3
local fg = {}; local bg = {}
for i = 1, 10 do fg[i] = 0xFFE0; bg[i] = 0x0000 end  -- yellow on black
term:setRowColors(3, fg, bg, 0, 10)
```

---

## Word wrap

#### `term:setWordWrap(enabled)`
Enable or disable word wrapping (default: off).

#### `term:setWordWrapColumn(col)`
Set the column at which wrapping occurs.

#### `term:setWrapIndicator(enabled)`
Show a dim `.` in the last content column of a row that wraps (default: on).

#### `term:getWordWrap()` → `bool`
Return the current word-wrap state.

#### `term:getVisualRowCount()` → `n`
Return the number of visual (wrapped) rows the current content occupies.

---

## Input helpers

The `wait` methods block the calling Lua coroutine while keeping the system menu, HTTP callbacks, screenshots, and the watchdog all responsive: they run the full OS service pass (menu, callbacks, dev commands, exit request). They poll the keyboard themselves. `waitForAnyKey` and `waitForKey` return once every button is up again. All of these read the keyboard only: a game controller does not reach them. To let a controller drive a terminal app, read [`picocalc.gamepad.getNavPressed()`](API-Gamepad.md#picocalcgamepadgetnavpressed) in your own loop, as [`apps/terminal_example`](https://github.com/PicoDeck/picodeck/tree/master/apps/terminal_example) does.

The `read` methods do not poll: they read the result of the last `picocalc.input.update()`, so without one they return `nil` for ever.

#### `term:waitForAnyKey()`
Block until any button is pressed, then return nothing. A typed letter is not a button: use `waitForChar` for those.

#### `term:waitForKey(keyName)` → `mask`
Block until the named key is pressed, ignoring every other key. Returns the button mask integer. An unknown name raises an error.

Valid key names: `"enter"`, `"left"`, `"right"`, `"up"`, `"down"`, `"esc"`, `"f1"`–`"f5"`, `"tab"`, `"backspace"`.

#### `term:readKey()` → `mask | nil`
Non-blocking. Returns the mask of the buttons that went down at the last `input.update()` (as `input.getButtonsPressed()`), or `nil` if none did. A key that is merely held reads as `nil`.

#### `term:readChar()` → `str | nil`
Non-blocking. Returns the character of the last `input.update()` (as `input.getChar()`), or `nil`. Enter and Backspace arrive as `"\n"` and `"\b"`. Read it once per `update()`, which gets you one character per frame.

For typed text in a slow frame loop, `input.pollEvent()` is the better read: it hands over every key that has arrived, in order, where `readChar` gets one per frame and 4 more can wait (16 events wait for `pollEvent`, and a keystroke is 3 of them: down, char and up).

```lua
picocalc.input.update()
for ev in picocalc.input.pollEvent do
    if ev.type == "char" then term:write(ev.char) end
end
```

#### `term:waitForChar()` → `str`
Block until a character is typed. Returns the character as a string (Enter and Backspace as above).

---

## Differences from the native terminal API

`picocalc.terminal` covers almost all of the native `picocalc_terminal_t` ([`sdk/native/terminal.h`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/terminal.h), 42 entries). Every native call has a Lua method of the same name except these:

| Native call | In Lua |
|-------------|--------|
| `create` / `free` | `picocalc.terminal.new(...)`; the collector frees the object (there is no `free` method) |
| `putChar(term, c)` | none: use `term:write(c)` |
| `getScrollbackLineColors(term, line, fg, bg)` | none: scrollback colours are not readable |
| `logicalToVisual`, `visualToLogical` | none: no logical/visual row mapping |
| `calculateLineWraps` | none: only `getVisualRowCount()` |

Same name, different shape:

- `create`: a native terminal always starts at y = 28, x = 4, whatever its size, so a native app sizes it to fit or calls `setRenderBounds`. Only Lua's `new` moves a tall terminal to fit.
- `write`: only Lua reads ANSI escape sequences. The native call puts each byte in a cell as it is, so native code sets colours with `setColors`.
- `getScrollbackLine`: native fills a `uint16_t` cell array for a 0-based `line`; Lua takes a 1-based line and returns a plain string (characters only, no colours).
- `setCursorVisible` and `setCursorBlink` are global: one setting shared by every terminal. The native entries take no `terminal_t*`; the Lua methods are called on an object as usual but the setting still applies to all terminals.

Lua-only methods: `setFont`/`getFont`, `setCell`/`getCell`, `setCellColors`, `setRowColors`, and the input helpers `waitForAnyKey`, `waitForKey`, `readKey`, `readChar`, `waitForChar`.

---

## Complete example

Writes more than fits, then scrolls back through it with the D-pad's Up and Down (the arrow keys, or a controller), repeating while one is held. B or Esc exits.

```lua
local D = picocalc.display
local input = picocalc.input
local gp = picocalc.gamepad

local term = picocalc.terminal.new()
term:setCursorVisible(false)

for i = 1, 50 do
    term:write(string.format("Line %d: the quick brown fox\n", i))
end

local function redraw()
    term:render()
    D.flush()
end

redraw()

while true do
    input.update()
    local nav = gp.getNavPressed()
    local offset = term:getScrollbackOffset()
    if nav & gp.PAD_B ~= 0 then
        return
    elseif nav & gp.PAD_UP ~= 0 then
        if offset < term:getScrollbackCount() then
            term:setScrollbackOffset(offset + 1)
            redraw()
        end
    elseif nav & gp.PAD_DOWN ~= 0 then
        if offset > 0 then
            term:setScrollbackOffset(offset - 1)
            redraw()
        end
    end
    picocalc.sys.sleep(16)
end
```
