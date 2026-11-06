---
title: "API UI"
---

Standard UI components for consistent app appearance. The `draw*` functions draw into the back buffer and keep no state: call them every frame you redraw, and `picocalc.display.flush()` shows the result. `confirm`, `textInput` and `splash` present their own frames.

These are the primitives: they draw a widget and leave the keys, the text, the scrolling and the focus to your app. For widgets that handle their own input, see [Library Widgets](Library-Widgets.md), built on these.

## picocalc.ui

### Functions

#### `picocalc.ui.drawHeader(title)`
Draws the standard header bar at the top of the screen: a 20px bar and a 1px border line below it. The title is on the left (in the current font); on the right are the clock (once it is set), a WiFi icon and a battery icon. The status on the right always uses the built-in 6x8 font, whatever font your app has selected.

- WiFi icon: bright when online; dimmed while connecting or before internet access is confirmed; dimmed with a red slash when the connection failed; absent when WiFi is disconnected or the device has no WiFi.
- Battery icon: shows the charge as a fill bar, or as a number inside the icon when the **Battery %** setting is on (system menu → Settings; the `battery_pct` key in [API Sysconfig](API-Sysconfig.md)). It turns red at 15% or below, and a lightning bolt appears beside it while charging.

- **Parameters:**
  - `title` (string): Title text to display
- **Returns:** (number) the rows the header covers (currently 21). Start your content at that y. Firmware before this return value was added returns nothing and draws a 29-row header, so `picocalc.ui.drawHeader(title) or 29` works on both.

```lua
local top = picocalc.ui.drawHeader("My App")
picocalc.display.drawText(8, top + 4, "Content", picocalc.display.WHITE)
```

---

#### `picocalc.ui.drawFooter([leftText [, rightText]])`
Draws a standard footer bar across the bottom of the screen, from y = 302 down, with optional left and right text.

- **Parameters:**
  - `leftText` (string, optional): Text for the left side
  - `rightText` (string, optional): Text for the right side
- **Returns:** (number) the rows the footer covers (18). Your content ends above `320 - height`.

```lua
picocalc.ui.drawFooter("Press Esc to exit", "Bat: 85%")
```

---

#### `picocalc.ui.drawTabs(y, tabs, activeIndex [, prevKey [, nextKey]])`
Draws a horizontal tab bar and optionally handles navigation key presses.

- **Parameters:**
  - `y` (number): Y coordinate of the top of the tab bar
  - `tabs` (table): Array of tab label strings (e.g., `{"Files", "Settings", "About"}`)
  - `activeIndex` (number): 1-based index of the currently active tab (one out of range draws as the nearest tab)
  - `prevKey` (number, optional): Button bitmask to switch to the previous tab (e.g., `picocalc.input.BTN_LEFT`)
  - `nextKey` (number, optional): Button bitmask to switch to the next tab (e.g., `picocalc.input.BTN_RIGHT`)
- **Returns:** (number, number) `newActiveIndex, heightConsumed` — the (possibly updated) active tab index and the pixel height consumed by the tab bar (21)

```lua
local tabs = {"Files", "Settings", "About"}
local active = 1

while true do
    picocalc.input.update()
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then
        return
    end
    picocalc.display.clear(picocalc.display.BLACK)
    local top = picocalc.ui.drawHeader("My App")
    local tab_h
    active, tab_h = picocalc.ui.drawTabs(top, tabs, active,
        picocalc.input.BTN_LEFT, picocalc.input.BTN_RIGHT)
    -- draw content for tabs[active] starting at y=top+tab_h
    picocalc.display.flush()
end
```

---

### Dialogs

> Dialogs are system UI: they draw in the built-in 6x8 font and across the whole screen, whatever font and clip rect your app has set, and give both back when they return. They leave their last frame in your framebuffers, so redraw your whole screen after one. The `draw*` helpers above and below (`drawFooter`, `drawTabs`, `drawPanel`, `splash`, and the other app-positioned widgets) and the `drawHeader` title draw in the app's current font instead.

#### `picocalc.ui.confirm(message)`
Show a yes/no confirmation dialog. Blocks until user responds: Enter or `y` for yes, Esc or `n` for no. Input in the first 400 ms is ignored, so a key your app was already reading cannot answer it. The system menu key is ignored while it is up (the dialog stays), so it never answers No for you.

- **Parameters:**
  - `message` (string): Question text. It wraps at 43 characters, at spaces and `/` where it can, and `\n` starts a new line; the box grows to fit, up to 20 lines, and anything past those is not shown.
- **Returns:** (boolean) true if confirmed, false if cancelled

```lua
if picocalc.ui.confirm("Delete this file?") then
    -- user pressed Yes
end
```

---

#### `picocalc.ui.textInput([prompt], [default])`
Show a modal text input dialog. Blocks until user submits (Enter) or cancels (Esc, or the system menu key).

- **Parameters:**
  - `prompt` (string, optional): Prompt text, on one line (43 characters fit in the box)
  - `default` (string, optional): Pre-filled text
- **Returns:** (string) entered text, or nil if cancelled

> **Note:** At most 127 characters: a longer `default` is cut, and typing stops there.

```lua
local name = picocalc.ui.textInput("Enter name:", "Player 1")
if name then
    print("Hello, " .. name)
end
```

---

#### `picocalc.ui.splash([status], [subtext])`
Draw a splash/loading screen with the PicoDeck logo: the final frame of the boot animation, with `status` and `subtext` centred under the logo.

- **Parameters:**
  - `status` (string, optional): Status text
  - `subtext` (string, optional): Smaller text below
- **Returns:** None

```lua
picocalc.ui.splash("Loading...", "Please wait")
```

---

### Widget Drawing Primitives

Each one returns the rows it covers, for laying out what comes next, except where its entry says otherwise. These are the fixed sizes:

| Widget | Height | Width |
|---|---|---|
| `drawHeader` | 21 (a 20 px bar and its border) | the screen |
| `drawFooter` | 18 | the screen |
| `drawTabs` | 21 | the screen |
| `drawButton` | 15 | `w` |
| `drawTextField`, `drawListItem` | 13 | `w` |
| `drawCheckbox` | 10 | 10 |
| `drawRadio` | 11 | 11 |
| `drawDivider` | 1 | `w` |
| `drawToast` | 14 | the text + 16 |

The widgets are laid out for the built-in 6x8 font, with 4 px of padding: a field or list item shows `(w - 8) / 6` characters. They draw in your current font but keep those metrics, so with a larger font the text overflows its box and text-area rows overlap. Select font 0 (`picocalc.display.setFont(0)`) to draw them.

A size too small for its text is not an error: the widget draws what fits, and text it has no room for is left out.

#### `picocalc.ui.drawPanel(x, y, w, h, [title])`
Draw a panel/card container with optional title bar. The title is centred in a 16 px bar.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `title` (string, optional): Panel title
- **Returns:** (number) the y where the panel's content starts: `y + 18` with a title, `y + 1` without

```lua
local top = picocalc.ui.drawPanel(10, 30, 300, 200, "Settings")
picocalc.ui.drawCheckbox(18, top + 6, true, false)
```

---

#### `picocalc.ui.drawButton(x, y, w, label, focused, [pressed])`
Draw a button widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `label` (string): Button label text
  - `focused` (boolean): Whether the button has focus
  - `pressed` (boolean, optional): Whether the button is pressed
- **Returns:** (number) the height, 15

```lua
picocalc.ui.drawButton(10, 100, 80, "OK", true, false)
```

---

#### `picocalc.ui.drawTextField(x, y, w, text, cursorPos, scrollOffset, focused, showCursor)`
Draw a single-line text input field. This is a rendering primitive — it does NOT handle input: keep the text and cursor yourself, reading typed characters with [`picocalc.input.pollEvent()`](API-Input.md), or use `W.TextField` from [Library Widgets](Library-Widgets.md), which does all that.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `text` (string): Current text content
  - `cursorPos` (number): Cursor character position, 0-based
  - `scrollOffset` (number): How many characters to skip from the start of `text`. A negative one reads as 0.
  - `focused` (boolean): Whether the field has focus (its border is brighter)
  - `showCursor` (boolean): Whether to display the cursor. It shows only while `focused` is true too, so blink it by passing false on alternate half-seconds.
- **Returns:** (number) the height, 13

```lua
picocalc.ui.drawTextField(10, 50, 200, "Hello", 5, 0, true, true)
```

---

#### `picocalc.ui.drawTextArea(x, y, w, h, text, segments, scrollY, cursorRow, cursorCol, focused, showCursor)`
Draw a multi-line text area widget. This is a rendering primitive: it shows `segments` as they are, and control characters in a row as spaces. Rows are 8 px apart and hold `(w - 8) / 6` characters, so wrap for that width, less a column or two if the scrollbar may show (it takes the last 4 px when there are more rows than fit).

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `text` (string): Full text content
  - `segments` (table): Array of line-start byte offsets, 0-based (from `wrapText`). Offsets outside `text` are clamped to it.
  - `scrollY` (number): Vertical scroll offset in rows. A negative one reads as 0.
  - `cursorRow` (number): Cursor row, 0-based, counted from the first segment and not from the scroll position
  - `cursorCol` (number): Cursor column, 0-based
  - `focused` (boolean): Whether the area has focus
  - `showCursor` (boolean): Whether to display the cursor. It shows only while `focused` is true too.
- **Returns:** (number) how many rows the box shows: `(h - 6) / 8` rows, at least 1, or 0 if `h` is under 12

```lua
local segments, rows = picocalc.ui.wrapText(myText, 40)
picocalc.ui.drawTextArea(10, 30, 250, 200, myText, segments, 0, 0, 0, true, true)
```

---

#### `picocalc.ui.drawListItem(x, y, w, text, selected, focused)`
Draw a selectable list item.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `text` (string): Item text
  - `selected` (boolean): Whether the item is selected (it fills with the highlight colour)
  - `focused` (boolean): Whether the item has focus (a selected item gets a brighter border)
- **Returns:** (number) the height, 13

```lua
for i, item in ipairs(items) do
    picocalc.ui.drawListItem(10, 30 + (i-1) * 20, 300, item, i == selectedIdx, true)
end
```

---

#### `picocalc.ui.drawProgress(x, y, w, h, progress, [fillColor])`
Draw a progress bar.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `progress` (number): Progress value from 0.0 to 1.0
  - `fillColor` (number, optional): RGB565 color (defaults to green)
- **Returns:** None (its height is your `h`)

```lua
picocalc.ui.drawProgress(10, 100, 200, 16, 0.75)
```

---

#### `picocalc.ui.drawCheckbox(x, y, checked, focused)`
Draw a checkbox widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `checked` (boolean): Whether the checkbox is checked
  - `focused` (boolean): Whether the checkbox has focus
- **Returns:** (number) the height, 10

```lua
picocalc.ui.drawCheckbox(10, 50, true, false)
```

---

#### `picocalc.ui.drawRadio(x, y, selected, focused)`
Draw a radio button widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `selected` (boolean): Whether the radio button is selected
  - `focused` (boolean): Whether the radio button has focus
- **Returns:** (number) the height, 11

```lua
picocalc.ui.drawRadio(10, 50, true, false)
```

---

#### `picocalc.ui.drawDivider(x, y, w, [color])`
Draw a horizontal divider line.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `color` (number, optional): RGB565 color
- **Returns:** (number) the height, 1

```lua
picocalc.ui.drawDivider(10, 80, 300)
```

---

#### `picocalc.ui.drawSpinner(cx, cy, [radius], [frame])`
Draw an animated loading spinner — a 2×2 pixel block chasing itself around the
corners of a small glyph, with a dim trail pixel. `radius` sets the glyph's scale: the
square it covers is about `2 * radius + 4` px wide. It does not clear behind itself.

- **Parameters:**
  - `cx` (number): Center X position
  - `cy` (number): Center Y position
  - `radius` (number, optional): Spinner radius (default 8)
  - `frame` (number, optional): Animation frame counter (default 0)
- **Returns:** None

```lua
local frame = 0
-- in render loop:
frame = frame + 1
picocalc.ui.drawSpinner(160, 160, 12, frame)
```

---

#### `picocalc.ui.drawToast(y, text, [bgColor])`
Draw a toast notification bar at the given Y position.

- **Parameters:**
  - `y` (number): Y position
  - `text` (string): Toast message text. The bar is centred and as wide as the text.
  - `bgColor` (number, optional): RGB565 background color (dark grey by default, and also when 0)
- **Returns:** (number) the height, 14

```lua
picocalc.ui.drawToast(280, "File saved!")
```

---

#### `picocalc.ui.toast(text, [style])`
Push a system toast notification that auto-displays and auto-dismisses. The OS draws the toast over your frame (centred at y 280, always in the built-in 6x8 font, whatever font your app has selected) at every present — `flush()`, `flushRows()` or `flushRegion()` — for about 3 s. With a partial flush that does not send its rows, the OS puts it on the panel directly and leaves your draw buffer as it was; when it expires, the OS pushes your buffer's pixels back over it.

- **Parameters:**
  - `text` (string): Toast message text. Up to 63 bytes are kept, and about 50 characters fit on the screen. An empty text is ignored.
  - `style` (number, optional): The background colour, one of the constants below (default `TOAST_INFO`)
- **Returns:** None

| Constant | Value | Colour |
|---|---|---|
| `picocalc.ui.TOAST_INFO` | 0 | Grey |
| `picocalc.ui.TOAST_SUCCESS` | 1 | Green |
| `picocalc.ui.TOAST_WARNING` | 2 | Amber |
| `picocalc.ui.TOAST_ERROR` | 3 | Red |

Toasts show one at a time, in the order pushed. Three can wait behind the one showing: pushing a fourth drops the oldest of those. The toast's 3 s starts at the first present after it reaches the front, so nothing shows while your app is not presenting frames.

```lua
picocalc.ui.toast("Download complete", picocalc.ui.TOAST_SUCCESS)
```

---

#### `picocalc.ui.wrapText(text, maxCols)`
Word-wrap text to fit within a column width. Returns segment offsets for use with `drawTextArea`.

A row breaks after the last space or punctuation mark that fits, looking back at most 20 columns, and is cut at `maxCols` if there is none. `\n` always starts a new row, and the newline counts as the last byte of the row it ends, so a text ending in `\n` has an empty row after it.

- **Parameters:**
  - `text` (string): Text to wrap
  - `maxCols` (number): Maximum columns per line. Zero or less makes the whole text one row.
- **Returns:** (table, number) segments array of 0-based byte offsets where the rows start, number of wrapped rows. Empty text is one row at offset 0.

Row `i` (1-based) of the text is `text:sub(segments[i] + 1, segments[i + 1] or #text)`, including any newline or space at its end.

```lua
local longText = "One line that is long enough to need wrapping.\nA second line."
local segments, rows = picocalc.ui.wrapText(longText, 40)
picocalc.ui.drawTextArea(10, 30, 250, 200, longText, segments, 0, 0, 0, true, true)
```
