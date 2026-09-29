---
title: "API Terminal"
---

`picocalc.terminal` provides a full-featured terminal emulator widget. Each terminal has its own cell grid, scrollback buffer, cursor, and colour state. Multiple terminals can be created but only one should render to the display at a time.

## Creating a terminal

```lua
local term = picocalc.terminal.new([cols [, rows [, scrollback]]])
```

- `cols` (number, default **53**): grid width in characters, 1-53
- `rows` (number, default **26**): grid height in characters, 1-26

Out-of-range sizes raise an error.
- `scrollback` (number, default **1000**): number of lines kept in scrollback history

Returns a terminal object. Raises an error if allocation fails (PSRAM OOM).

```lua
-- Full-screen terminal with default size
local term = picocalc.terminal.new()

-- Smaller terminal, 500-line scrollback
local term = picocalc.terminal.new(40, 20, 500)
```

---

## Writing and clearing

#### `term:write(str)`
Write a string to the terminal at the current cursor position. Supports ANSI escape sequences (cursor movement, colour codes, erase sequences, etc.). The cursor advances automatically; lines wrap and the display scrolls when the bottom row is reached.

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
Set the default foreground and background colours (RGB565).

#### `term:getColors()` → `fg, bg`
Return the current default foreground and background colours.

---

## Scrolling

#### `term:scroll(lines)`
Scroll the viewport up by `lines` rows (positive = up, negative = down).

#### `term:getScrollbackCount()` → `n`
Return the number of lines currently in the scrollback buffer.

#### `term:getScrollbackLine(n)` → `str`
Return scrollback line `n` (1-based) as a plain string. Raises an error if `n` is out of range.

#### `term:getScrollbackOffset()` → `offset`
Return the current scrollback view offset (0 = showing live output).

#### `term:setScrollbackOffset(offset)`
Jump the viewport to a specific scrollback position.

---

## Rendering

Call one of these once per frame after writing content.

#### `term:render()`
Redraw every cell to the framebuffer. Use when the whole screen may be stale (e.g. first frame, font change).

#### `term:renderDirty()`
Redraw only rows marked dirty since the last render. Faster than `render()` for incremental updates.

#### `term:markAllDirty()`
Mark every row as dirty so the next `renderDirty()` redraws everything.

#### `term:isFullDirty()` → `bool`
Returns `true` if all rows are marked dirty.

#### `term:getDirtyRange()` → `first, last`
Return the first and last dirty row indices (inclusive). Useful for partial redraws.

---

## Font

#### `term:setFont(name)`
Switch the terminal font. Valid values:

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
Set the gutter width in columns (default: 5).

#### `term:setLineNumberColors(fg, bg)`
Set gutter foreground and background colours (RGB565).

#### `term:getContentCols()` → `n`
Return the number of columns available for content (`terminal cols − gutter width`). Use this when calculating text layout so content does not overlap the gutter.

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
Restrict rendering to rows between `y_start` and `y_end` (inclusive). Useful for embedding a terminal in a portion of the screen alongside other UI elements.

- `y_start` (number): First row to render (0-based pixel row)
- `y_end` (number): Last row to render

```lua
term:setRenderBounds(16, 300)  -- leave room for a header
```

---

## Cell access

Direct cell manipulation for syntax highlighting and custom rendering.

#### `term:setCell(x, y, ch)`
Set the character at column `x`, row `y` (0-based). Out-of-bounds coordinates are silently ignored.

- `x` (number): Column
- `y` (number): Row
- `ch` (string): Single character to place in the cell

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
Show a visual indicator (e.g. `↩`) at wrapped line ends.

#### `term:getWordWrap()` → `bool`
Return the current word-wrap state.

#### `term:getVisualRowCount()` → `n`
Return the number of visual (wrapped) rows the current content occupies.

---

## Input helpers

These methods block the calling Lua coroutine while keeping the system menu, HTTP callbacks, screenshots, and the watchdog all responsive: they run the full OS service pass (menu, callbacks, dev commands, exit request).

#### `term:waitForAnyKey()`
Block until any button is pressed, then return `nil`.

#### `term:waitForKey(keyName)` → `mask`
Block until the named key is pressed. Returns the button mask integer.

Valid key names: `"enter"`, `"left"`, `"right"`, `"up"`, `"down"`, `"esc"`, `"f1"`–`"f5"`, `"tab"`, `"backspace"`.

#### `term:readKey()` → `mask | nil`
Non-blocking. Returns the button mask if any key is currently pressed, or `nil`.

#### `term:readChar()` → `str | nil`
Non-blocking. Returns a single-character string if a printable key was typed, or `nil`.

#### `term:waitForChar()` → `str`
Block until a printable character is typed. Returns the character as a string.

---

## Differences from the native terminal API

`picocalc.terminal` covers almost all of the native `picocalc_terminal_t` (`sdk/native/terminal.h`, 42 entries). Every native call has a Lua method of the same name except these:

| Native call | In Lua |
|-------------|--------|
| `create` / `free` | `picocalc.terminal.new(...)`; the collector frees the object (there is no `free` method) |
| `putChar(term, c)` | none: use `term:write(c)` |
| `getScrollbackLineColors(term, line, fg, bg)` | none: scrollback colours are not readable |
| `logicalToVisual`, `visualToLogical` | none: no logical/visual row mapping |
| `calculateLineWraps` | none: only `getVisualRowCount()` |

Same name, different shape:

- `getScrollbackLine`: native fills a `uint16_t` cell array for a 0-based `line`; Lua takes a 1-based line and returns a plain string (characters only, no colours).
- `setCursorVisible` and `setCursorBlink` are global: one setting shared by every terminal. The native entries take no `terminal_t*`; the Lua methods are called on an object as usual but the setting still applies to all terminals.

Lua-only methods: `setFont`/`getFont`, `setCell`/`getCell`, `setCellColors`, `setRowColors`, and the input helpers `waitForAnyKey`, `waitForKey`, `readKey`, `readChar`, `waitForChar`.

---

## Complete example

```lua
local D = picocalc.display

local term = picocalc.terminal.new()
term:setLineNumbers(true)
term:setScrollbar(true)
term:setScrollbarWidth(6)

local lines = {}

local function redraw()
    term:renderDirty()
    D.flush()
end

-- Write some content
for i = 1, 50 do
    local line = string.format("Line %d: the quick brown fox\n", i)
    table.insert(lines, line)
    term:write(line)
end

term:setScrollInfo(#lines, 0)
redraw()

-- Scroll with arrow keys
while true do
    local key = term:waitForKey("up") or term:readKey()
    if key then
        if key & picocalc.input.BTN_UP ~= 0 then
            term:scroll(-1)
        elseif key & picocalc.input.BTN_DOWN ~= 0 then
            term:scroll(1)
        elseif key & picocalc.input.BTN_ESC ~= 0 then
            return
        end
        redraw()
    end
end
```
