---
title: "API Gamepad"
---

A logical gamepad with 12 buttons, two analog sticks and two analog triggers. Read it instead of raw keys and your game gets rebinding without writing any code for it: each button is an alias for keys, and players choose those keys in the system menu ([Controls](#controls-settings), below). A driving game can [read the sticks and triggers](#analog-sticks-and-triggers) alone and still work on the keyboard.

## How it works

- **Aliasing, not remapping.** A bound key still reports as itself: `BTN_F4`, `getChar()`, `pollEvent()` and `isKeyDown()` are unchanged. Apps that read keys keep working, and one app can read both.
- **Two slots per button.** Each button has a **primary** and an **alternate** key. The button is held while either key is down.
- **One button per key.** Within one map a key drives at most one button.
- **Same polling as the input masks.** `picocalc.input.update()` updates the gamepad too. A tap shorter than one frame still gives one press edge and, on the next frame, one release edge.
- **Esc is not a gamepad button.** Keep Esc as back/quit. `getNavPressed()` reads it as B, so in a menu B and Esc both go back.
- **What is down at the system menu stays hidden.** A button down when the menu opens or closes (B closing it, a direction held on through it), when an OS dialog returns, or when your app starts, from a key or a controller, reads as not held, with no edges, until it is released; pressed again it reads normally. Other buttons are not affected. See [`update()`](API-Input.md).
- **A connected controller drives the same buttons.** A game controller and the keyboard work together: a button is held while either holds it, with one press edge and one release edge, and your game cannot tell which one pressed it, so it needs no code for controllers. `getLabel()` still names the keyboard key. A controller's Home button opens the system menu (and closes it again when pressed while it is open, like the menu key), and the D-pad and A/B move through it and the launcher, and A and B also answer the OS's confirm and text-input dialogs, so a player with only a controller can leave a game. The header shows a gamepad icon while one is connected. On the device that is a [Bluetooth controller](#bluetooth-controllers); in the simulator, a controller plugged into the computer.

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

The browser demo at [picodeck.net/try](https://picodeck.net/try/) also binds Z to A and X to B, with F4 and F5 as their alternate keys, since many laptops reach F4 and F5 only with Fn. A controller works there too, once the browser has seen one of its buttons pressed.

### Keys that can be bound

Arrows, Enter, Esc, Tab, Backspace, Delete, Space, F1–F5, the letters A–Z, and Ctrl.

The keyboard reports some keys differently while Shift or Alt is held, and the gamepad allows for it:

- **Shift** turns F1–F5 into F6–F10, Delete into End, Tab into Home, Enter into Insert, Esc into Brk and Up/Down into PgUp/PgDn. The gamepad reads these as the original key, so a button held while Shift goes down still releases. Shift+F5 (reported as F10) and Shift+Esc (Brk) keep their system meaning (system menu, screenshot) and never press a button.
- **Shift** silences Left, Right, Backspace and Space, and **Alt** silences B and Space and turns I into Insert. If Shift or Alt goes down while one of these is held, its button is released at once, and while Alt is held B, I and Space do not press their button; keep holding the key and the button comes back at the key's next repeat after the modifier is up.
- Enter pressed while Shift is held gives a short press, then reads as held from its first repeat.
- A key pressed within a few milliseconds after Shift comes up may miss its press (Left, Right, Backspace, Space, Enter).

A button can stay held in two cases, until the system menu opens, the app exits or it calls `picocalc.input.clearState()`:

- Enter released at the same moment as Shift while Alt is still held.
- B, I or Space held while the keyboard's num lock is on. Pressing Left Shift and Alt together turns num lock on; it then acts as if Alt were held, and it stays on until Shift is pressed on its own. The gamepad cannot see it.

Not bindable: the system menu key (Shift+F5, reported as F10) and Brk, which belong to the OS; F6–F9, which are Shift+F1–F4; digits and symbols, which Shift turns into other symbols; Shift, Alt and Sym, which change the other keys.

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

## Analog sticks and triggers

`getAxis` reads a stick as `-1.0` to `1.0`, right and down positive like screen coordinates, and a trigger as `0.0` to `1.0`. They are separate from the `PAD_*` buttons, which behave as they always have. API version 10.

```lua
local gp = picocalc.gamepad
picocalc.input.update()
local steer = gp.getAxis(gp.AXIS_LX)
local throttle = gp.getAxis(gp.AXIS_RT)
if gp.getButtons() & gp.PAD_A ~= 0 then throttle = 1 end -- for the keyboard
```

- **It works without a controller.** With no stick in use, `AXIS_LX` and `AXIS_LY` read the D-pad, `-1`, `0` or `+1`, whether that is the keyboard's bound keys or the controller's own D-pad. So a game can read only the axes for movement and still work on the keyboard. The right stick and the triggers have no such fallback: they read `0`, so a game with a trigger for the throttle needs a button as well, as above.
- **A stick in use wins over the D-pad.** A stick past half way also holds `PAD_LEFT` and the rest, so a game reading buttons can use it as a D-pad, but `getAxis` still reads how far it is pushed, not `-1`. While the left stick is in use both of its axes read the stick, whatever the D-pad holds.
- **The dead zone is done for you.** A stick within about 12% of its centre reads `0` on both axes (the distance counts, not each axis on its own), and the rest of the travel is rescaled to start at `0`, so there is no jump as the stick leaves the middle. From about 95% it reads full, so a worn stick still reaches `1`. Past the dead zone the direction is kept, so a square-gated stick pushed into a corner reads about `0.7` on each axis. A trigger reads `0` under about 3% and full from about 97%.
- **Several controllers.** Each axis reads the largest value among the controllers connected.
- **Same polling as the buttons.** `picocalc.input.update()` updates the axes too, and every read until the next one gives the same value. An axis is a level, not an event: nothing latches a short movement. Moving a stick or trigger keeps the screen from dimming, as a key does, but a stick or trigger held dead still does not.
- **Resolution.** 127 steps each side of a stick's centre and 255 along a trigger, whatever the controller sends.
- **The keyboard has no analog input.** There the left stick's axes are only ever `-1`, `0` or `+1`.

Which controllers have what is under [Bluetooth controllers](#bluetooth-controllers). In the simulator a controller plugged into the computer gives all six axes. In the browser demo at [picodeck.net/try](https://picodeck.net/try/) the sticks are analog, but the browser's triggers reach the game as `0` or `1`.

## Bluetooth controllers

A Bluetooth game controller drives the same 12 buttons and the analog axes, so every game that reads `picocalc.gamepad` or `api->gamepad` works with one and needs no code for it. One controller at a time; the keyboard keeps working alongside it.

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

The controller is remembered (up to 4; pairing a fifth forgets the oldest). Next time, switch it on (the PS or Home button) while Bluetooth is on and it reconnects by itself, in the launcher or in a game. On the Bluetooth page, Enter on a paired controller connects or disconnects it, and **Del** twice forgets it. A paired controller connects with the key it was paired with; one that was paired with another console or computer since has lost that key, so forget it and pair it again. Only the controller you pick from the search can pair, and only while you are pairing it: at any other time a device that asks to pair is refused, including one that copies a paired controller's address. (Bluetooth's "Just Works" pairing, which game controllers use, cannot tell a device posing as your controller from the real one while you pair it, so pair it where you can see it.)

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
| `AXIS_LX` / `AXIS_LY` | Left stick | Left stick | Left stick |
| `AXIS_RX` / `AXIS_RY` | Right stick | Right stick | Right stick |
| `AXIS_LT` / `AXIS_RT` | L2 / R2 | 8BitDo: its triggers. Switch Pro: none, read `0` | LT / RT |

- **By position, not by label.** A is always the bottom face button and B the right one, as on an Xbox pad. On a Nintendo-style pad (Switch Pro, 8BitDo) that means A is the button labelled **B**.
- The left stick works as the D-pad once pushed past half way, and [`getAxis`](#analog-sticks-and-triggers) reads how far. The stick clicks are not used, and neither are the buttons that stand in for a trigger: the Switch Pro's ZL and ZR are digital, so they do nothing, and its triggers read `0`. A controller without a right stick reads `0` there.
- **Home (or PS) opens the system menu**, where the D-pad and A/B move: a player with only a controller can always leave a game.
- A controller that drops (switched off, out of range, flat battery) releases every button it held: nothing stays stuck.

Supported: Bluetooth Classic HID controllers. PicoDeck has a button layout for the DualShock 4, DualSense, Switch Pro Controller (in its simple mode) and 8BitDo pads; any other gamepad is read with the common Android/Xbox numbering, which most Bluetooth "Android" pads use. Not supported: Bluetooth LE-only controllers (the Xbox Wireless Controller with current firmware, Stadia, Steam), a single Joy-Con, and Wii remotes. If a controller pairs but its buttons come out wrong, [open an issue](https://github.com/PicoDeck/picodeck/issues) saying which one.

## picocalc.gamepad

`nil` on firmware older than API version 9. Check before use:

```lua
local gp = picocalc.gamepad
if not gp then
    -- older firmware: read picocalc.input instead
end
```

`getAxis` and the `AXIS_*` constants are `nil` on firmware older than API version 10: check `gp.getAxis` before use.

**`PAD_*` and `BTN_*` share their numbers.** `PAD_A` is `BTN_ENTER`, `PAD_B` is `BTN_ESC`, `PAD_START` is `BTN_F4`, and so on: only the directions mean the same thing. So `picocalc.input.getButtonsPressed() & gp.PAD_A` raises no error, and reads Enter. Test `PAD_*` only against `picocalc.gamepad`'s calls, and `BTN_*` only against `picocalc.input`'s.

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

#### `picocalc.gamepad.getButtonsRepeated()`
Like `getButtonsPressed()`, plus a repeat edge for each button still held after `picocalc.input.setRepeat()`'s delay (200 ms unless you change it), then one every rate (80 ms). Use it for anything that should keep moving while a button is held.

- **Returns:** (number) Bitmask of `PAD_*` constants

Every call in one frame returns the same mask. `picocalc.input.setRepeat()` sets the timing for this and for `picocalc.input.getButtonsRepeated()`, and `setRepeat(0)` turns both off. A button already held when your app starts (or when it calls `picocalc.input.clearState()`) does not repeat until it is pressed again.

---

#### `picocalc.gamepad.getNavPressed()`
What a menu or list acts on, so one call drives it from the keyboard, a rebound key or a controller:

- the D-pad, with auto-repeat as in `getButtonsRepeated()`;
- the other buttons' presses, without repeat (holding A confirms once);
- `PAD_A` when Enter is pressed and `PAD_B` when Esc is pressed, unless the player bound that key to a button (then it means that button only).

- **Returns:** (number) Bitmask of `PAD_*` constants

Every call in one frame returns the same mask.

```lua
local gp = picocalc.gamepad
while true do
    picocalc.input.update()
    local nav = gp.getNavPressed()
    if nav & gp.PAD_UP ~= 0 then sel = math.max(1, sel - 1) end
    if nav & gp.PAD_DOWN ~= 0 then sel = math.min(#items, sel + 1) end
    if nav & gp.PAD_A ~= 0 then open(items[sel]) end
    if nav & gp.PAD_B ~= 0 then return end -- B or Esc: back
    draw()
    picocalc.display.flush()
end
```

`PAD_UP`, `PAD_DOWN`, `PAD_LEFT`, `PAD_RIGHT`, `PAD_A` and `PAD_B` have the same values as `BTN_UP`, `BTN_DOWN`, `BTN_LEFT`, `BTN_RIGHT`, `BTN_ENTER` and `BTN_ESC`.

Both functions are newer than the rest of `picocalc.gamepad`; check `if gp and gp.getNavPressed then` on older firmware.

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

---

#### `picocalc.gamepad.getAxis(axis)`
One analog axis as of the last `picocalc.input.update()`, with the dead zone applied. With no stick in use, `AXIS_LX` and `AXIS_LY` read the D-pad ([Analog sticks and triggers](#analog-sticks-and-triggers), above). API version 10.

- **Parameters:**
  - `axis` (number): One `AXIS_*` constant
- **Returns:** (number) `-1.0` to `1.0` for a stick, right and down positive; `0.0` to `1.0` for a trigger

Raises an error when `axis` is not an `AXIS_*` constant.

```lua
local gp = picocalc.gamepad
local x, y = 160, 160
while true do
    picocalc.input.update()
    x = x + gp.getAxis(gp.AXIS_LX) * 3
    y = y + gp.getAxis(gp.AXIS_LY) * 3
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then return end
    draw(x, y)
    picocalc.display.flush()
end
```

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

### Axis Constants

| Constant | Axis | Reads |
|----------|------|-------|
| `picocalc.gamepad.AXIS_LX` | Left stick, left to right | `-1.0` to `1.0` |
| `picocalc.gamepad.AXIS_LY` | Left stick, up to down | `-1.0` to `1.0` |
| `picocalc.gamepad.AXIS_RX` | Right stick, left to right | `-1.0` to `1.0` |
| `picocalc.gamepad.AXIS_RY` | Right stick, up to down | `-1.0` to `1.0` |
| `picocalc.gamepad.AXIS_LT` | Left trigger | `0.0` to `1.0` |
| `picocalc.gamepad.AXIS_RT` | Right trigger | `0.0` to `1.0` |

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

Button names are `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `l`, `r`, `start`, `select`; key names are those above. Both ignore case. A missing or corrupt file, or one over 2 KB, means the defaults (or no override), never a failed launch.

An unknown button name is ignored. An unknown key name leaves that slot unbound, and it does not bring back the button's default: `{"b":["Delete"]}` leaves B with no key, since the key's name is `Del`. Both are logged on the serial console as `[GAMEPAD]` lines.

## Native API (C)

`api->gamepad` (`picocalc_gamepad_t` in [`sdk/native/os.h`](https://github.com/PicoDeck/picodeck/blob/master/sdk/native/os.h)), API version 9. The pointer sits after `version` in `PicoCalcAPI`, so on older firmware the field does not exist: check the version first.

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
| `float getAxis(int axis)` | API version 10: one `AXIS_*`, as [above](#analog-sticks-and-triggers); `0.0` for anything else |

The masks and axes update when `api->sys->poll()` runs, like `api->input`'s.

`getAxis` is at the end of the table, so older firmware does not have it: check for version 10, not 9. It returns the float the way the rest of the API does, in `r0` (the base AAPCS, which `-mcpu=cortex-m33 -mthumb` and `-mfloat-abi=softfp` both use; never build an app with `-mfloat-abi=hard`).

```c
if (api->version >= 10) {
    api->sys->poll();
    float steer = api->gamepad->getAxis(AXIS_LX);
    float throttle = api->gamepad->getAxis(AXIS_RT);
}
```
