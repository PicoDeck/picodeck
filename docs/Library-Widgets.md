---
title: "Library Widgets"
---

A toolkit of ready-made form widgets for Lua apps: labels, buttons, checkboxes, radio groups, text fields, text areas, dropdowns and scrolling lists, plus a focus group that moves between them. Each widget is a Lua object that holds its own state, reacts to keys and draws itself, through [`picocalc.ui`](API-UI.md).

The library's source is `system/lib/widgets.lua` in the repo; it must be on the SD card at `/system/lib/widgets.lua`. Load it with:

```lua
local W = picocalc.sys.loadlib("widgets")
```

## Which one to use

| | [`picocalc.ui`](API-UI.md) | Widgets (this page) | [Panels](Library-Panels.md) |
|---|---|---|---|
| **What it is** | Draw calls and two blocking dialogs, built in | Stateful objects in Lua, on top of `picocalc.ui` | A comic player that takes over the screen |
| **State and keys** | Yours: it draws what you pass and reads nothing | The widget's: `handleInput` edits its text, selection and cursor | The library's |
| **Your frame loop** | Yes | Yes | Not needed (`Panels.start` runs its own) |
| **Use it for** | Headers and footers, a quick `confirm` or `textInput`, a widget that behaves your way | Settings screens and forms: several fields that keys move between | Stories told in scrolling panels of art and text |

A game usually needs only `picocalc.ui`. Reach for this library when you would otherwise be writing a text field, a focus order or a scrolling list yourself. The toolkit has no layout: you give every widget its position in pixels.

## Quick start

A complete form. Tab moves between the widgets, and the focused one takes the keys.

```lua
local W = picocalc.sys.loadlib("widgets")
local input, display, ui = picocalc.input, picocalc.display, picocalc.ui

local quit = false
local name = W.TextField({ x = 90, y = 34, w = 200, placeholder = "your name" })
local sound = W.Checkbox({ x = 90, y = 54, label = "Sound on", checked = true })
local level = W.RadioGroup({ x = 90, y = 72, options = { "Easy", "Normal", "Hard" }, selected = 2 })
local theme = W.Select({ x = 90, y = 118, w = 200, options = { "Red", "Green", "Blue", "Black", "White" } })
local list = W.ListView({ x = 10, y = 140, w = 300, h = 60, items = { "Alpha", "Beta", "Gamma", "Delta", "Echo", "Foxtrot" } })
local notes = W.TextArea({ x = 10, y = 204, w = 300, h = 44, text = "Notes wrap at the edge of the box, and Enter starts a new line in them." })
local save = W.Button({ x = 10, y = 254, w = 90, label = "Save", onPress = function()
    ui.toast("Saved " .. name:getText())
end })
local leave = W.Button({ x = 110, y = 254, w = 90, label = "Quit", onPress = function() quit = true end })

local group = W.FocusGroup({ name, sound, level, theme, list, notes, save, leave })
local labels = {
    W.Label({ x = 10, y = 36, text = "Name" }),
    W.Label({ x = 10, y = 56, text = "Options" }),
    W.Label({ x = 10, y = 74, text = "Level" }),
    W.Label({ x = 10, y = 120, text = "Theme" }),
}
local line = W.Divider({ x = 10, y = 112, w = 300 })
local bar = W.ProgressBar({ x = 210, y = 256, w = 100, h = 10, progress = 0.4 })

while not quit do
    input.update()
    -- Press edges, plus Ctrl and Shift while held: Ctrl+arrow and Shift+Tab read them.
    local btns = input.getButtonsPressed() | (input.getButtons() & (input.BTN_CTRL | input.BTN_SHIFT))
    group:handleInput(btns, input.getChar())

    display.clear(display.BLACK)
    ui.drawHeader("Settings")
    for _, l in ipairs(labels) do l:draw() end
    line:draw()
    for _, w in ipairs({ name, sound, level, theme, list, notes, save, leave, bar }) do w:draw() end
    theme:drawDropdown()      -- last, so the open list covers what is under it
    display.flush()
    picocalc.sys.sleep(16)
end
```

## How widgets work

**Creating one.** Each constructor takes one options table, which must be there even if empty (`W.Divider({})`). All options are optional, and positions default to 0.

**Drawing.** `widget:draw()` draws it where it is, into the back buffer. Call it every frame you redraw, in the order you want them stacked. No widget clears the screen except `Loading`, and presenting the frame is up to you.

**Input.** Focusable widgets have `widget:handleInput(btns, ch)`, which returns `true` if the widget used the input. Read it before your own keys: a Select that is open returns `true` for the Esc that closes it, so don't quit on that one.

- `btns` is a `BTN_*` mask of keys pressed this frame. `input.getButtonsPressed()` gives that, and the quick start ORs in Ctrl and Shift while held, since a modifier is pressed some frames before the key it goes with.
- `ch` is one typed character, or `nil`. Only printable ASCII is taken.

`input.getChar()` gives one character per `input.update()`, which is plenty at a normal frame rate, and 4 more can wait for the next updates. If your frames are slow, a fast typist can get ahead of that, so read the event queue instead, which hands over every key that has arrived (16 events wait, and a keystroke is 3 of them: down, char and up):

```lua
group:handleInput(input.getButtonsPressed(), nil)
for ev in input.pollEvent do
    if ev.type == "char" then group:handleInput(0, ev.char) end
end
```

**Focus.** A widget draws differently when focused and takes keys only if you pass them to it. `widget:setFocused(bool)` sets it, and a [`FocusGroup`](#wfocusgroupwidgets) does it for you.

**Fonts.** Like `picocalc.ui`, the widgets are laid out for the built-in 6x8 font. Keep the display on font 0 while drawing them.

**Sizes.** A one-line field, list row or `W.theme.row_h` is 13 px, a button 15, a checkbox 10 and a radio 11. See [the sizes table](API-UI.md#widget-drawing-primitives).

## Display widgets

These take no input, and a `FocusGroup` skips them.

### `W.Label(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position of the text, or of its box with `w` |
| `text` | `""` | The text |
| `w` | none | Box width, which `align` needs |
| `align` | `"left"` | `"left"`, `"center"` or `"right"` inside `w` |
| `color` | `W.theme.text` | Text colour |

Methods: `setText(str)`, `draw()`. The text is one line, with no background of its own.

### `W.Divider(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Left end |
| `w` | 300 | Length |
| `color` | `W.theme.border` | Colour |

Methods: `draw()`.

### `W.ProgressBar(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w`, `h` | 200, 12 | Size |
| `progress` | 0 | 0.0 to 1.0 |
| `color` | `W.theme.success` | Fill colour |

Methods: `setProgress(p)`, `draw()`.

### `W.Panel(opts)`

A bordered card with an optional title bar, to draw first and put other widgets on. It holds nothing: you still position and draw what goes in it, starting 18 px below its top with a title.

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w`, `h` | 310, 260 | Size |
| `title` | none | Title bar text |

Methods: `draw()`.

### `W.Loading(opts)`

A full-screen wait screen. `draw()` clears the screen to black and shows a spinner, or a progress bar if `progress` is a number, with the text centred under it. Each `draw()` advances the spinner.

| Option | Default | Meaning |
|---|---|---|
| `text` | `"Loading..."` | Main text |
| `subtext` | none | A second line below it, dimmer |
| `progress` | none | 0.0 to 1.0; none shows the spinner |

Methods: `setText(str)`, `setProgress(p)`, `draw()`.

### `W.toast(msg [, seconds])` and `W.drawToasts()`

The toolkit's own toasts: `W.toast` adds one for `seconds` (default 2), and `W.drawToasts()`, which you call each frame, draws those still live, stacked upward from y = 280, newest lowest. For most apps [`picocalc.ui.toast`](API-UI.md#picocalcuitoasttext-style) is simpler: the OS shows it at the next present with no draw call, and it can be coloured.

## Input widgets

All of these have `setFocused(bool)`, `handleInput(btns, ch)` and `draw()`.

### `W.Button(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w` | 80 | Width (the height is 15) |
| `label` | `"Button"` | Text, centred |
| `onPress` | none | `function()`, called on Enter |

Methods: `setLabel(str)`. It shows as pressed for one frame.

### `W.Checkbox(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position of the box |
| `label` | `""` | Text to the right of the box |
| `checked` | false | Starting state |
| `onChange` | none | `function(checked)` |

Methods: `isChecked()`, `setChecked(bool)`. Enter toggles it and calls `onChange`; `setChecked` does not.

### `W.RadioGroup(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position of the first radio |
| `options` | `{}` | Array of label strings |
| `selected` | 1 | Selected index |
| `spacing` | 13 | Pixels between rows |
| `onChange` | none | `function(index, text)` |

Methods: `getSelected()` returns `index, text`; `setSelected(i)`. Up and Down move the highlight (wrapping), and Enter selects it.

### `W.TextField(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w` | 200 | Width (the height is 13) |
| `text` | `""` | Starting text |
| `placeholder` | `""` | Shown dimmed while empty and unfocused |
| `maxlen` | 256 | Most characters typing can reach |
| `onChange` | none | `function(text)`, when typing or deleting changes it |
| `onSubmit` | none | `function(text)`, on Enter |

Methods: `getText()`, `setText(str)`, `setCursor(pos)` (0-based; none of these call `onChange`).

Keys: Left, Right; Ctrl+Left and Ctrl+Right by word; Ctrl+F1 to the start and Ctrl+F2 to the end; Backspace; Del; Enter; printable characters. It scrolls sideways to keep the cursor in view, and the cursor blinks.

### `W.TextArea(opts)`

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w`, `h` | 300, 100 | Size |
| `text` | `""` | Starting text |
| `placeholder` | `""` | Shown while empty and unfocused |
| `wordWrap` | true | Wrap long lines at word breaks. Off, a long line is cut at the edge of the box |
| `multiline` | true | Enter adds a newline. Off, Enter calls `onSubmit` |
| `maxlen` | 4096 | Most characters typing can reach |
| `onChange` | none | `function(text)` |
| `onSubmit` | none | `function(text)`, on Enter when `multiline` is off |

Methods: `getText()`, `setText(str)`.

Keys: the arrows; Ctrl+Up and Ctrl+Down by a page of rows (this moves the view, not the cursor); Ctrl+Left and Ctrl+Right by word; Backspace; Del; Enter; printable characters. It shows `(h - 6) / 8` rows of `(w - 12) / 6` characters, and a scrollbar when there are more rows.

### `W.Select(opts)`

A one-line field that opens a dropdown, which you filter by typing.

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w` | 200 | Width |
| `options` | `{}` | Array of strings |
| `selected` | 1 | Selected index |
| `maxVisible` | 6 | Rows of the open list |
| `onChange` | none | `function(index, text)` |

Methods: `getSelected()` returns `index, text`; `setSelected(i)`; `setOptions(t)` (selects the first); `isExpanded()`; `drawDropdown()`.

`draw()` draws the closed field. Call `drawDropdown()` as well, after everything it may cover, every frame: it draws only while the list is open, below the field, or above it near the bottom of the screen.

Keys: Enter opens the list. While open: type to filter (case-insensitive), Backspace to undo that, Up and Down to move, Enter to choose, Esc to close without choosing. Losing focus also closes it.

### `W.ListView(opts)`

A scrolling list in a bordered box.

| Option | Default | Meaning |
|---|---|---|
| `x`, `y` | 0 | Position |
| `w`, `h` | 300, 200 | Size. It shows `(h - 2) / 13` rows |
| `items` | `{}` | Array of strings |
| `onSelect` | none | `function(index, item)`, when the highlight moves |
| `onActivate` | none | `function(index, item)`, on Enter |

Methods: `getSelected()` returns `index, item`; `setItems(t)` (back to the first row). Up and Down move and wrap, and the box scrolls to follow.

## `W.FocusGroup(widgets)`

Moves focus between widgets and hands keys to the focused one. `widgets` is an array; those that take no input stay in it harmlessly. The first focusable one starts focused. A group draws nothing: keep drawing the widgets yourself.

- `group:handleInput(btns, ch)`: Tab moves to the next focusable widget and Shift+Tab to the previous, wrapping; anything else goes to the focused widget. Returns `true` if it was used.
- `group:getFocused()`: the focused widget, or `nil`.
- `group:getFocusIndex()`: its index in the array, 0 if none.
- `group:setFocus(index)`: move focus to that widget.

## `W.theme`

The colours and metrics the toolkit's Lua code reads, as a table you can change before creating widgets. Constructor defaults read it when the widget is made; the rest read it at each draw.

| Field | Default | Used for |
|---|---|---|
| `text`, `text_dim` | white, grey | Labels; dim when not focused |
| `bg` | dark blue | `ListView` and the `Select` dropdown |
| `bg_input` | near black | The closed `Select`, and behind a `TextField`'s placeholder |
| `border`, `focus` | blue-greys | `ListView`, `Select` and `Divider` |
| `success` | green | `ProgressBar`, `Loading` |
| `accent`, `error` | | Not read by any widget |
| `font_w`, `font_h`, `row_h` | 6, 8, 13 | Layout: leave these |

What `picocalc.ui` draws (buttons, fields, list rows, checkboxes, radios, panels) keeps its own colours and does not read this table.
