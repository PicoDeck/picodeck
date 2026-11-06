---
title: "API REPL"
---

Primitives for building console-style applications: a scrolling output area of 37 rows of 53 characters, with a one-line input bar under it. The module reads lines and shows text. Evaluating what was typed is up to your app, and there is no command history.

## picocalc.repl

The screen belongs to the REPL while you use it: each call that changes something redraws the whole screen in the built-in 6x8 font and presents it.

### Functions

#### `picocalc.repl.readline()`
Non-blocking read of a single input event. Call this in a loop to build an interactive prompt. It polls the keyboard itself, so don't call `picocalc.input.update()` in the same loop.

| Key | What `readline` does |
|---|---|
| Enter | Returns the typed line (`""` if nothing was typed), adds it to the output (unless echo is off) and empties the bar |
| Up, Down | Scrolls the output a line, and returns `nil`. The next `print` jumps back to the newest line |
| Backspace | Removes the last character, and returns `nil` |
| Esc | Only returns `nil`: the typed text stays in the bar, and the call looks the same as no key at all |
| A printable character | Adds it to the bar (up to 255 of them; the bar shows the end of a longer line), and returns `nil` |

To act on Esc, read `picocalc.input.getButtonsPressed()` straight after `readline`, as the example does.

`readline` draws only when one of these changes something, so an app that starts by reading shows no input bar until the first key. Call `picocalc.repl.print()` with no arguments first: it draws the screen and adds no line.

- **Parameters:** None
- **Returns:** (string or nil) The input line when Enter is pressed, otherwise `nil`

```lua
local repl = picocalc.repl
local input = picocalc.input

repl.print("Type a Lua expression. Esc quits.")
while true do
    local line = repl.readline()
    if line then
        local chunk, err = load("return " .. line)
        if chunk then
            repl.print(select(2, pcall(chunk)))
        else
            repl.print(err)
        end
    end
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then
        return
    end
    picocalc.sys.sleep(10)
end
```

---

#### `picocalc.repl.print(...)`
Print values to the REPL output area. Accepts multiple arguments which are converted to strings and separated by tabs. Supports nil, boolean, number, string, table, function, and userdata types.

Values go through `tostring()` (so `__tostring` metamethods are honoured). Tabs move to the next stop of 8 columns, `\n` starts a new line, and a line longer than 53 characters continues on the next row. The output keeps a 256-line scrollback, and printing shows the newest line.

Each call redraws and presents the whole screen, so print many lines as one string with `\n` rather than one call per line.

- **Parameters:**
  - `...` (any): Values to print. With none, the screen is drawn and no line is added.
- **Returns:** None

```lua
picocalc.repl.print("Hello", 42, true)
-- Output: Hello   42      true
```

---

#### `picocalc.repl.clear()`
Clear the screen and forget the output, its scroll position and any text typed so far. The input bar comes back with the next `print` or key.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.repl.clear()
picocalc.repl.print("Screen cleared.")
```

---

#### `picocalc.repl.echo(enabled)`
Enable or disable input echo. While it is off the input bar shows one `*` per typed character, and a submitted line is returned but not added to the output. Useful for password prompts or hidden input. Echo is on when an app starts.

- **Parameters:**
  - `enabled` (boolean): `true` to show typed characters, `false` to hide them
- **Returns:** None

```lua
local repl = picocalc.repl
repl.echo(false)
repl.print("Enter password:")
local password = nil
while not password do
    password = repl.readline()
    picocalc.sys.sleep(10)
end
repl.echo(true)
repl.print("Got " .. #password .. " characters.")
```
