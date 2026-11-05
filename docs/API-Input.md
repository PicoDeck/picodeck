---
title: "API Input"
---

Keyboard and button input functions. For games, the [gamepad](API-Gamepad.md) (`picocalc.gamepad`) gives the same edges on buttons players can rebind; the keys behind it still report here.

## picocalc.input

### Functions

#### `picocalc.input.update()`
Takes the keyboard events that arrived since the last call. **Call once per frame** before reading button or character state. The keyboard controller is read in the background (every 10 ms, or back to back while keys are coming), so `update()` does not wait on it and a key reaches it within ~22 ms. The exception: after a failed controller transaction, `update()` runs the ~12 ms bus recovery (at most every 100 ms once failures persist).

If neither `update()` nor `picocalc.sys.sleep()` has run for a second, the controller is left alone until the next call. Keys pressed meanwhile wait in the controller and arrive, in order, from the call after that one.

`update()` also services HTTP/TCP/sound callbacks, the system menu and dev commands.

**After the system menu, a dialog or a launch.** A key that is down when the system menu opens or closes, when a dialog (`ui.confirm`, `ui.textInput`, `fs.browse`) returns, or when your app starts, reads as not held from then on: no `getButtons()` bit, no edges, no `pollEvent` events, no chars, no `isKeyDown`, and no [gamepad](API-Gamepad.md) button, until it is released. Pressed again it reads normally; holding it on does not count, however long the keyboard repeats it. So a game that compares `getButtons()` from one frame to the next never reads the menu's Esc, or the Enter that answered a dialog, as a press, and a direction held on through the menu needs a new press. What was typed at a dialog does not reach `pollEvent` either. The same goes the other way: the key your app quits on does not reach the launcher.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.input.update()
```

---

#### `picocalc.input.getButtons()`
Returns the current bitmask of **held** buttons.

- **Parameters:** None
- **Returns:** (number) Bitmask of currently pressed buttons

```lua
local buttons = picocalc.input.getButtons()
if buttons & picocalc.input.BTN_UP ~= 0 then
    -- Up button is held
end
```

---

#### `picocalc.input.getButtonsPressed()`
Returns the bitmask of buttons that were **pressed this frame** (edge detection, not held).

- **Parameters:** None
- **Returns:** (number) Bitmask of buttons pressed this frame

```lua
local pressed = picocalc.input.getButtonsPressed()
if pressed & picocalc.input.BTN_ENTER ~= 0 then
    -- Enter was just pressed
end
```

---

#### `picocalc.input.getButtonsReleased()`
Returns the bitmask of buttons that were **released this frame**.

- **Parameters:** None
- **Returns:** (number) Bitmask of buttons released this frame

---

#### `picocalc.input.getButtonsRepeated()`
Like `getButtonsPressed()`, but buttons held past the repeat delay also produce synthetic repeat edges (see `setRepeat()`). Useful for menus that should scroll while a button is held.

- **Parameters:** None
- **Returns:** (number) Bitmask of pressed edges plus synthetic repeat edges

```lua
picocalc.input.setRepeat(400, 80)  -- start repeating after 400ms, then every 80ms
local pressed = picocalc.input.getButtonsRepeated()
if pressed & picocalc.input.BTN_DOWN ~= 0 then
    selection = selection + 1  -- scrolls while held
end
```

---

#### `picocalc.input.setRepeat(delay_ms [, rate_ms])`
Configures key auto-repeat for `getButtonsRepeated()`.

- **Parameters:**
  - `delay_ms` (number): Milliseconds a button must be held before repeating starts. `0` disables auto-repeat.
  - `rate_ms` (number, optional): Milliseconds between repeat edges (default `80`, minimum `1`).
- **Returns:** None

```lua
picocalc.input.setRepeat(400)      -- repeat every 80ms after a 400ms hold
picocalc.input.setRepeat(0)        -- disable auto-repeat
```

---

#### `picocalc.input.getChar()`
Returns the character the last `update()` took, if any: one per `update()`, oldest first. Several keys typed in one frame arrive on the frames that follow, in order; up to 4 wait, and older ones are dropped. A held key repeats. Use `pollEvent()` to see every key, however many arrive.

The value stays until the next `update()`: reading it twice in a frame gives the same character twice, so **never loop on it**. `while c do ... c = getChar() end` does not end. Call it once per `update()`, or drain `pollEvent()` instead.

Enter returns `"\n"`, Backspace `"\b"`, and Ctrl+letter a control code (Ctrl+C is `"\3"`). Tab, Delete, arrows and function keys return `nil`.

- **Parameters:** None
- **Returns:** (string or nil) Single-character string, or `nil` if no character arrived with this `update()`

```lua
picocalc.input.update()
local ch = picocalc.input.getChar()
if ch then
    text = text .. ch
end
```

---

#### `picocalc.input.getRawKey()`
Returns the code of the last key to go down or repeat during the last `update()`: the same code `pollEvent()` reports as `key` (see [Key codes](#key-codes)). It stays until the next `update()`.

- **Parameters:** None
- **Returns:** (number) Key code, or `0` if no key went down with this `update()`

```lua
picocalc.input.update()
if picocalc.input.getRawKey() == 0xD4 then  -- Delete
    delete_selection()
end
```

---

#### `picocalc.input.clearState()`
Clear all button and key input state. Resets pressed, released, and held button buffers, queued `pollEvent` events and `isKeyDown` state. Useful when returning from modal dialogs or menus to prevent stale input.

- **Parameters:** None
- **Returns:** None

```lua
-- After closing a dialog, clear stale input
picocalc.input.clearState()
```

---

#### `picocalc.input.pollEvent()`
Pops the oldest keyboard event, or `nil` when none is queued. Events are
filled by `update()` in the order keys were pressed and released, so taps
shorter than a frame and several characters in one frame are all reported.

- **Returns:** (table or nil) `{type, key, char, mods, button, repeat}`:
  - `type`: `"down"`, `"up"` or `"char"`
  - `key`: ASCII for printable keys, else the keyboard code (as `getRawKey()`)
  - `char`: the character (`"char"` events only)
  - `mods`: `BTN_SHIFT`/`BTN_CTRL`/`BTN_ALT`/`BTN_FN` held at the event
  - `button`: the `BTN_*` constant for keys that have one
  - `repeat`: `true` on the `down` and `char` events a held key repeats with, absent on a press (read as `ev["repeat"]`: `repeat` is a Lua keyword). The keyboard repeats a held key about every 100 ms after the first 300 ms.

Independent of `getChar()`/`getButtons*()`. 16 events are kept (the oldest is
dropped); a new app starts with an empty queue.

```lua
picocalc.input.update()
for ev in picocalc.input.pollEvent do
    if ev.type == "char" then text = text .. ev.char end
end
```

---

#### `picocalc.input.isKeyDown(k)`
True while a key is held. `k` is a one-character string (`"w"`; letters
ignore case) or an integer keycode as `pollEvent` reports it. Updated by
`update()`. Reliable for buttons (arrows, Enter, Esc, F-keys, modifiers);
letters and shifted symbols depend on the keyboard reporting their release
(letters confirmed on the device: Z and X held and released in a game; shifted symbols still pending hardware confirmation). `clearState()` clears a key that sticks.

---

### Button Constants

Bitmask values for button states:

| Constant | Description |
|----------|-------------|
| `picocalc.input.BTN_UP` | D-pad Up |
| `picocalc.input.BTN_DOWN` | D-pad Down |
| `picocalc.input.BTN_LEFT` | D-pad Left |
| `picocalc.input.BTN_RIGHT` | D-pad Right |
| `picocalc.input.BTN_ENTER` | Enter key |
| `picocalc.input.BTN_ESC` | Escape key |
| `picocalc.input.BTN_MENU` | Menu key (system overlay, auto-handled; opens the menu and, pressed again while it is open, closes it) |
| `picocalc.input.BTN_F1` | F1 key |
| `picocalc.input.BTN_F2` | F2 key |
| `picocalc.input.BTN_F3` | F3 key |
| `picocalc.input.BTN_F4` | F4 key |
| `picocalc.input.BTN_F5` | F5 key |
| `picocalc.input.BTN_F6` | F6 key |
| `picocalc.input.BTN_F7` | F7 key |
| `picocalc.input.BTN_F8` | F8 key |
| `picocalc.input.BTN_F9` | F9 key |
| `picocalc.input.BTN_BACKSPACE` | Backspace key |
| `picocalc.input.BTN_TAB` | Tab key |
| `picocalc.input.BTN_DEL` | Delete key |
| `picocalc.input.BTN_SHIFT` | Shift modifier (left or right) |
| `picocalc.input.BTN_CTRL` | Ctrl modifier |
| `picocalc.input.BTN_ALT` | Alt modifier |
| `picocalc.input.BTN_FN` | Fn/Symbol modifier |
