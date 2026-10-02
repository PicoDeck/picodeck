---
title: "API Gamepad"
---

A logical gamepad with 12 buttons. Read it instead of raw keys and your game gets rebinding without writing any code for it: each button is an alias for keys, and players choose those keys in the system menu ([Controls](#controls-settings), below).

## How it works

- **Aliasing, not remapping.** A bound key still reports as itself: `BTN_F4`, `getChar()`, `pollEvent()` and `isKeyDown()` are unchanged. Apps that read keys keep working, and one app can read both.
- **Two slots per button.** Each button has a **primary** and an **alternate** key. The button is held while either key is down.
- **One button per key.** Within one map a key drives at most one button.
- **Same polling as the input masks.** `picocalc.input.update()` updates the gamepad too. A tap shorter than one frame still gives one press edge and, on the next frame, one release edge.
- **Esc is not a gamepad button.** Keep Esc as back/quit.
- **A connected controller drives the same buttons.** A game controller and the keyboard work together: a button is held while either holds it, with one press edge and one release edge, and your game cannot tell which one pressed it, so it needs no code for controllers. `getLabel()` still names the keyboard key. A controller's Home button opens the system menu (and closes it again when pressed while it is open, like the menu key), and the D-pad and A/B move through it and the launcher, so a player with only a controller can leave a game. The header shows a gamepad icon while one is connected. On the device that is a [Bluetooth controller](#bluetooth-controllers); in the simulator, a controller plugged into the computer.

### Default bindings

Only the primary slots are bound by default.

| Button | Constant | Default key |
|--------|----------|-------------|
| Up / Down / Left / Right | `PAD_UP` / `PAD_DOWN` / `PAD_LEFT` / `PAD_RIGHT` | arrow keys |
| A | `PAD_A` | F4 |
| B | `PAD_B` | F5 |
| X | `PAD_X` | Delete |
| Y | `PAD_Y` | Backspace |
| L | `PAD_L` | F2 |
| R | `PAD_R` | F3 |
| Start | `PAD_START` | F1 |
| Select | `PAD_SELECT` | Tab |

### Keys that can be bound

Arrows, Enter, Esc, Tab, Backspace, Delete, Space, F1–F5, the letters A–Z, and Ctrl.

The keyboard reports some keys differently while Shift or Alt is held, and the gamepad allows for it:

- **Shift** turns F1–F5 into F6–F10, Delete into End, Tab into Home, Enter into Insert, Esc into Brk and Up/Down into PgUp/PgDn. The gamepad reads these as the original key, so a button held while Shift goes down still releases. Shift+F5 (F10) and Shift+Esc (Brk) keep their system meaning (system menu, screenshot) and never press a button.
- **Shift** silences Left, Right, Backspace and Space, and **Alt** silences B and Space and turns I into Insert. If Shift or Alt goes down while one of these is held, its button is released at once, and while Alt is held B, I and Space do not press their button; keep holding the key and the button comes back at the key's next repeat after the modifier is up.
- Enter pressed while Shift is held gives a short press, then reads as held from its first repeat.
- A key pressed within a few milliseconds after Shift comes up may miss its press (Left, Right, Backspace, Space, Enter).

A button can stay held in two cases, until the system menu opens, the app exits or it calls `picocalc.input.clearState()`:

- Enter released at the same moment as Shift while Alt is still held.
- B, I or Space held while the keyboard's num lock is on. Pressing Left Shift and Alt together turns num lock on; it then acts as if Alt were held, and it stays on until Shift is pressed on its own. The gamepad cannot see it.

Not bindable: the system menu key (F10) and Brk, which belong to the OS; F6–F9, which are Shift+F1–F4; digits and symbols, which Shift turns into other symbols; Shift, Alt and Sym, which change the other keys.

## Controls (Settings)

Players rebind the gamepad in the system menu: **Settings → Controls**. Your game needs no code for it.

- **At the launcher** the page edits the bindings of all games (`/system/gamepad.json`).
- **Inside a running game** its top row picks **This game** (the default) or **All games**. This game edits that game's override (`/data/<APP_ID>/gamepad.json`); the bindings it inherits from All games are drawn dimmed.

Each button has a row with a **Primary** and an **Alt** cell. Each cell shows its key's name, or `-` when unbound.

| Key | Action |
|-----|--------|
| Arrows | Move between cells; on the top row, Left/Right switch This game / All games |
| Enter | Bind the cell: the next key pressed becomes its key (on the top row, switch) |
| Menu key | Cancel the capture; otherwise saves and closes the menu |
| C | Clear the cell |
| R, twice | Reset: All games to the defaults; This game to All games (its override file is deleted) |
| Esc | Save and go back |

- While the cell reads `press a key...`, every key that can be bound is taken, Esc and Backspace included. A key that cannot be bound is refused with a notice ("1 can't be bound") and the capture goes on; Shift, Alt and Sym are passed over without one, since they start chords. Brk is not bound either: it takes its screenshot once the menu has closed, of the game's screen.
- The menu key is Shift+F5, so press Shift first. If Shift and F5 go down in the same keyboard scan, the keyboard can report F5 before Shift, and F5 is bound instead of the capture being cancelled.
- A key already on another button moves: its old slot becomes unbound and the notice says so ("F5 moved from B to A"). In This game, the button it left becomes part of the override too, as does any button whose slot you bind or clear.
- Leaving the page with Esc saves, and the running game has the new bindings as soon as the menu closes. The key that closes the menu never reaches the game as a press. If the save fails the page says so, and the previous file and bindings are kept.
- A bindings file that is there but cannot be read (a read error, or too little free memory) keeps the page shut with "Could not read the bindings", so its bindings are never replaced by what the page could not see. A corrupt file, which launches ignore too, opens as the defaults ("Ignored a corrupt bindings file"), and saving replaces it.
- The global file lists only the buttons that differ from the defaults (with none it is deleted); the override lists the buttons it overrides.

## Bluetooth controllers

A Bluetooth game controller drives the same 12 buttons, so every game that reads `picocalc.gamepad` or `api->gamepad` works with one and needs no code for it. One controller at a time; the keyboard keeps working alongside it.

### Turning it on

Bluetooth is **off** until you turn it on: system menu → Settings → **Bluetooth**, then Enter on **Bluetooth: Off**. While it is off nothing of it runs. The setting is kept (the `bt_enabled` key in [API Sysconfig](API-Sysconfig.md)), so it is back on after a restart.

### Pairing

1. Put the controller in pairing mode:
   - DualShock 4: hold **Share** and **PS** until the light bar flashes quickly.
   - DualSense: hold **Create** and **PS** until the lights around the touchpad flash.
   - Switch Pro Controller: hold the small **sync** button on the top edge.
   - 8BitDo: switch it to its Android/D-input or Switch mode (usually **Start**+**B** or **Start**+**Y** to power on), then hold its **Pair** button for 3 seconds.
2. On the Bluetooth page choose **Search for controllers**. The search takes about 10 seconds; controllers are listed first, other devices dimmed below them.
3. Choose your controller. The page says **Connected** and the header shows the gamepad icon.

The controller is remembered (up to 4; pairing a fifth forgets the oldest). Next time, switch it on (the PS or Home button) while Bluetooth is on and it reconnects by itself, in the launcher or in a game. On the Bluetooth page, Enter on a paired controller connects or disconnects it, and **Del** twice forgets it. A paired controller connects with the key it was paired with; one that was paired with another console or computer since has lost that key, so forget it and pair it again. Only the controller you choose from the search, while it pairs, may pair: nothing else nearby can pair with your PicoDeck or pose as your controller.

### Buttons

| Gamepad | DualShock 4 / DualSense | Switch Pro / 8BitDo | Xbox-style Android pad |
|---|---|---|---|
| A (bottom) | Cross | B | A |
| B (right) | Circle | A | B |
| X (left) | Square | Y | X |
| Y (top) | Triangle | X | Y |
| L / R | L1 / R1 | L / R | LB / RB |
| Start / Select | Options / Share (Create) | + / − (Start / Select) | Start / Back |
| Up/Down/Left/Right | D-pad, left stick | D-pad, left stick | D-pad, left stick |
| System menu | PS | Home | Home / Guide |

- **By position, not by label.** A is always the bottom face button and B the right one, as on an Xbox pad. On a Nintendo-style pad (Switch Pro, 8BitDo) that means A is the button labelled **B**.
- The left stick works as the D-pad once pushed past half way. Triggers, the right stick and the stick clicks are not used.
- **Home (or PS) opens the system menu**, where the D-pad and A/B move: a player with only a controller can always leave a game.
- A controller that drops (switched off, out of range, flat battery) releases every button it held: nothing stays stuck.

Supported: Bluetooth Classic HID controllers. PicoDeck has a button layout for the DualShock 4, DualSense, Switch Pro Controller (in its simple mode) and 8BitDo pads; any other gamepad is read with the common Android/Xbox numbering, which most Bluetooth "Android" pads use. Not supported: Bluetooth LE-only controllers (the Xbox Wireless Controller with current firmware, Stadia, Steam), a single Joy-Con, and Wii remotes. If a controller pairs but its buttons come out wrong, [tell us](https://github.com/PicoDeck/picodeck/issues/26) which one.

## picocalc.gamepad

`nil` on firmware older than API version 9. Check before use:

```lua
local gp = picocalc.gamepad
if not gp then
    -- older firmware: read picocalc.input instead
end
```

### Functions

#### `picocalc.gamepad.getButtons()`
Returns the bitmask of **held** gamepad buttons.

- **Returns:** (number) Bitmask of `PAD_*` constants

---

#### `picocalc.gamepad.getButtonsPressed()`
Returns the bitmask of gamepad buttons **pressed this frame**.

- **Returns:** (number) Bitmask of `PAD_*` constants

---

#### `picocalc.gamepad.getButtonsReleased()`
Returns the bitmask of gamepad buttons **released this frame**.

- **Returns:** (number) Bitmask of `PAD_*` constants

```lua
local gp = picocalc.gamepad
while true do
    picocalc.input.update()
    local held, pressed = gp.getButtons(), gp.getButtonsPressed()
    if held & gp.PAD_LEFT ~= 0 then x = x - 2 end
    if held & gp.PAD_RIGHT ~= 0 then x = x + 2 end
    if pressed & gp.PAD_A ~= 0 then jump() end
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then return end
    draw()
    picocalc.display.flush()
end
```

---

#### `picocalc.gamepad.getLabel(btn [, slot])`
The name of the key bound to a button, for on-screen hints (`"F4"`, `"Del"`, `"W"`). Use it instead of writing key names into your game: the player may have rebound them.

- **Parameters:**
  - `btn` (number): One `PAD_*` constant
  - `slot` (number, optional): `0` for the primary key (default), `1` for the alternate
- **Returns:** (string or nil) The key's name, or `nil` when that slot is unbound

Raises an error when `btn` is not a single `PAD_*` constant or `slot` is not `0` or `1`.

```lua
local key = picocalc.gamepad.getLabel(picocalc.gamepad.PAD_A) or "?"
picocalc.display.drawText(10, 300, "Press " .. key .. " to jump", picocalc.display.WHITE)
```

Key names: `Up`, `Down`, `Left`, `Right`, `Enter`, `Esc`, `Tab`, `Bksp`, `Del`, `Space`, `F1`–`F5`, `A`–`Z`, `Ctrl`.

### Button Constants

| Constant | Button |
|----------|--------|
| `picocalc.gamepad.PAD_UP` | Up |
| `picocalc.gamepad.PAD_DOWN` | Down |
| `picocalc.gamepad.PAD_LEFT` | Left |
| `picocalc.gamepad.PAD_RIGHT` | Right |
| `picocalc.gamepad.PAD_A` | A |
| `picocalc.gamepad.PAD_B` | B |
| `picocalc.gamepad.PAD_X` | X |
| `picocalc.gamepad.PAD_Y` | Y |
| `picocalc.gamepad.PAD_L` | L |
| `picocalc.gamepad.PAD_R` | R |
| `picocalc.gamepad.PAD_START` | Start |
| `picocalc.gamepad.PAD_SELECT` | Select |

## Bindings files

The bindings are read when an app starts, and again when the Controls page saves:

- **`/system/gamepad.json`**: the global map, applied over the defaults.
- **`/data/<APP_ID>/gamepad.json`**: one game's override. Only the buttons it lists change; every other button keeps the global binding. A key the override uses is taken off the button it had in the global map.

Both are plain JSON you can edit by hand. Each button lists its primary key, then its alternate; `[]`, `""` or `null` leaves a slot unbound:

```json
{
  "a": ["F4", "Z"],
  "up": ["Up", "W"],
  "y": ["", "X"],
  "select": []
}
```

Button names are `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `l`, `r`, `start`, `select`; key names are those above. Both ignore case. A missing or corrupt file means the defaults (or no override), never a failed launch. Unknown button and key names are ignored and logged on the serial console as `[GAMEPAD]` lines.

## Native API (C)

`api->gamepad` (`picocalc_gamepad_t` in `sdk/native/os.h`), API version 9. The pointer sits after `version` in `PicoCalcAPI`, so on older firmware the field does not exist: check the version first.

```c
if (api->version >= 9) {
    const picocalc_gamepad_t *gp = api->gamepad;
    api->sys->poll();
    uint32_t pressed = gp->getButtonsPressed();
    if (pressed & PAD_A) jump();
    const char *key = gp->getLabel(PAD_A, 0);   // "F4", or NULL when unbound
}
```

| Function | Returns |
|----------|---------|
| `uint32_t getButtons(void)` | `PAD_*` held |
| `uint32_t getButtonsPressed(void)` | `PAD_*` pressed this frame |
| `uint32_t getButtonsReleased(void)` | `PAD_*` released this frame |
| `const char *getLabel(uint32_t pad_button, int slot)` | The key's name; `NULL` when unbound, or when `pad_button` is not one `PAD_*` bit or `slot` is not 0 or 1 |

The masks update when `api->sys->poll()` runs, like `api->input`'s.
