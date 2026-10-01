---
title: "Library Panels"
---

A declarative interactive-comics framework, modelled on the Playdate [Panels](https://github.com/cadin/panels) library. A comic is a plain Lua data table — **sequences** of **panels** of **layers** — and the library owns scrolling, parallax, keyframed animation, audio cues, transitions, branching choices, and progress save/resume.

The library's source is `system/lib/panels.lua` in the repo; it must be on the SD card at `/system/lib/panels.lua`. Load it with:

```lua
local Panels = picocalc.sys.loadlib("panels")
```

Comic content must be **pure data**: the library never `load()`s strings from the comic table, and every asset filename is resolved through a *source* object that rejects path escapes (`..`, leading `/`, backslashes).

---

## Quick Start

A complete comic app is a `main.lua` that loads the library and hands it a data table:

```lua
local Panels = picocalc.sys.loadlib("panels")

local comic = {
    name = "My First Comic",
    sequences = {
        {
            scrollType = "scroll",
            panels = {
                {
                    frame = { height = 480 },
                    layers = {
                        { image = "sky.png",  parallax = 0.15 },
                        { image = "city.png", parallax = 0.6, transparentColor = true },
                        { text = "The city never sleeps.", x = 20, y = 40 },
                    },
                },
                {
                    frame = { height = 320, marginBefore = 8 },
                    layers = {
                        { image = "hero.png" },
                        { text = "Neither do I.", x = 100, y = 200, scrollTrigger = 0.5 },
                    },
                },
            },
        },
    },
}

local result = Panels.start(comic, { resume = true })
-- result: "finished" | "quit" | "error"[, err]
```

Image files live in `<APP_DIR>/images/`, audio files in `<APP_DIR>/audio/` (see [Asset sources](#asset-sources)).

---

## Running a comic

### `Panels.start(comicData [, opts])`
Blocking entry point. Validates the comic, runs the full input/update/draw loop (including `picocalc.input.update()` and `picocalc.display.flush()`), and returns when the reader exits.

- **Parameters:**
  - `comicData` (table): The comic data table (see [Comic schema](#comic-schema))
  - `opts` (table, optional): See [Options](#options)
- **Returns:** (string [, string]) One of:
  - `"finished"` — the reader reached the end (saved progress is cleared, so a finished comic restarts fresh)
  - `"quit"` — the reader pressed **Esc** (progress is saved first)
  - `"error", err` — validation or runtime error; `err` is the message. Errors are shown on a full-screen "PANELS FAULT" display until the reader presses **Esc**.

### `Panels.new(comicData [, opts])`
Non-blocking constructor for embedding a comic in your own frame loop. Validates and returns a comic object.

```lua
local comic = Panels.new(comicData, opts)
while not comic:finished() do
    picocalc.input.update()
    comic:update()                 -- handles input, draws; does NOT flush
    picocalc.display.flush()
end
comic:teardown()
```

Comic object methods:

| Method | Description |
|--------|-------------|
| `comic:update()` | Run one frame: input, scrolling, caching, drawing. Does **not** flush. Sets `comic.drewThisFrame` (`false` when an idle frame was skipped — you may skip your `flush()` too). |
| `comic:finished()` | `true` once the comic has ended. |
| `comic:currentSequence()` | Current sequence index. |
| `comic:goToSequence(idx)` | Jump to sequence `idx` using the current sequence's transition. |
| `comic:saveProgress()` / `comic:loadProgress()` / `comic:clearProgress()` | Manual save-slot control (see [Save and resume](#save-and-resume)). |
| `comic:teardown()` | Stop audio, drop cached images, clear the clip rect. Call when you are done (`Panels.start` does this for you). |

### `Panels.validate(comicData)`
Validates the comic table and raises a descriptive error naming the offending path (e.g. `sequences[2].panels[1].choices[1]: target must index an existing sequence`). Called automatically by `Panels.new`/`Panels.start`.

### Options

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `resume` | boolean | `false` | Restore the saved sequence index and `Panels.vars` for this comic, if a save exists. |
| `startSequence` | number | `1` | Sequence to begin at (overridden by a resumed save). |
| `baseDir` | string | `APP_DIR` | Base directory for the default file source. |
| `source` | table | file source | Custom asset source — a table with `imagePath(name)` and `audioPath(name)` functions (see [Asset sources](#asset-sources)). |
| `name` | string | `comicData.name` or `"comic"` | Save-slot name for this comic. |

---

## Controls

- **D-pad along the scroll axis** — scroll (`"scroll"` mode) or step between panels (`"advance"` mode). Down/Up for a vertical axis, Right/Left for horizontal; swapped when `direction` is `REVERSE`.
- Pushing **forward at the end of a sequence** transitions to the next sequence.
- **Up/Down + Enter** — navigate and confirm a [choice menu](#choices).
- **Esc** — exit; `Panels.start` saves progress and returns `"quit"`.

Key repeat is configured from `Panels.Settings.repeatDelayMs` / `repeatRateMs` at `Panels.new` time.

---

## Comic schema

### Top level

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `sequences` | array | yes | Non-empty array of sequence tables. |
| `name` | string | no | Display/save name; default save-slot name. |
| `font` | userdata | no | Default font for text layers that use the word-wrapped (`w`/`h`) path. |

### Sequence fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `panels` | array | *required* | Non-empty array of panel tables. |
| `title` | string | — | Informational only; not rendered or read by the library. |
| `axis` | number | `Panels.ScrollAxis.VERTICAL` | `Panels.ScrollAxis.VERTICAL` (1) or `.HORIZONTAL` (2). |
| `scrollType` | string | `"scroll"` | `"scroll"` (free D-pad scrolling), `"advance"` (snap panel-to-panel, `Settings.advanceMs` ease), or `"auto"` (timed advance). |
| `direction` | number | `Panels.ScrollDirection.FORWARD` | `.FORWARD` (1) or `.REVERSE` (-1); reverse swaps the forward/back keys. |
| `advanceControl` | number | forward D-pad key | `"advance"` mode: `BTN_*` constant that steps forward. The natural forward D-pad key **also** always advances. Can also be set per panel. |
| `autoAdvanceMs` | number | `2500` | `"auto"` mode: milliseconds per panel. Can be overridden per panel. |
| `audio` | table | — | Background music: `{ file = "bgm.wav", loop = true, volume = 0.8 }`. `file` resolves under `audio/`; `loop` replays on finish; `volume` optional. Started on sequence entry, stopped on exit. |
| `backgroundColor` | number | `Settings.backgroundColor` | RGB565 clear colour for the sequence (also the panel-fill fallback). |
| `nextSequence` | number | current index + 1 | Sequence entered when scrolling/advancing past the end. Must index an existing sequence. |
| `transition` | string | `"fadeToBlack"` | Transition used when **leaving** this sequence: `"fadeToBlack"`, `"fadeToWhite"`, or `"cut"`. Fade duration is `Settings.transitionMs`. |

### Panel fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `frame` | table | `{}` | `{ height, width, marginBefore, marginAfter }`. `height` is the panel's extent on a vertical axis, `width` on a horizontal one; the off-axis dimension is always the full 320 px screen. Extent defaults to `Settings.defaultFrameSize` (320). `marginBefore`/`marginAfter` add gaps (default 0). |
| `layers` | array | — | Layer tables, drawn in order (first = back). |
| `audio` | table | — | One-shot sound effect: `{ file = "chime.wav", scrollTrigger = 0.3, volume = 1.0 }`. Fires once per viewport entry, when the panel's scroll percentage reaches `scrollTrigger` (default 0) and the panel has not fully scrolled past. Re-arms when the panel leaves the screen. |
| `effect` | table | — | `{ type = "shake", strength = 3 }` — shakes the whole panel by ±`strength` px each frame. (`"blink"` is layer-only.) |
| `borderless` | boolean | `false` | Suppress the panel border (`Settings.borderWidth` / `borderColor`). |
| `backgroundColor` | number | sequence / `Settings.backgroundColor` | RGB565 fill behind the layers. |
| `renderFunction` | function | — | `function(panel, ox, oy, pct)` — custom drawing after the layers, with the panel clip rect still active. `ox, oy` is the panel's on-screen origin, `pct` its scroll percentage 0–1. |
| `updateFunction` | function | — | `function(panel, pct)` — called every frame the panel is visible, before `renderFunction`. |
| `choices` | table | — | Branching menu (see [Choices](#choices)). |
| `advanceControl` | number | — | Per-panel override of the sequence's `advanceControl` (`"advance"` mode). |
| `autoAdvanceMs` | number | — | Per-panel override of the sequence's `autoAdvanceMs` (`"auto"` mode). |

The **scroll percentage** (`pct`) passed to triggers and callbacks runs 0 → 1 as the panel traverses the viewport, normalized so 0 and 1 are reachable even for the first/last panels of a sequence.

### Layer fields

Every layer needs at least one of `image`, `images`, or `text`.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `image` | string | — | Image filename, resolved under `images/`. |
| `images` | array | — | Array of filenames; the displayed frame follows the scroll percentage (frame `ceil(pct * n)`, clamped to 1..n). Under `"auto"` advance this reads as a timed animation. |
| `text` | string | — | Text line drawn with the built-in font. |
| `x`, `y` | number | `0` | Offset from the panel's frame origin. |
| `parallax` | number | `0` | 0–1. Extra shift along the scroll axis proportional to the panel's traversal: 0 = locked to the panel, larger values move faster (foreground). Zero shift when the panel is centred in the viewport. |
| `color` | number | `Settings.textColor` | Text colour (single-line text layers). |
| `bg` | number \| false | `false` | Text background colour; `false` = transparent glyph backgrounds. |
| `font` | userdata | comic `font` | Font for the word-wrapped text path. |
| `w`, `h` | number | — | When both are set on a text layer, the text is word-wrapped into that box via `picocalc.graphics.drawTextInRect` (with optional `align`: 0 left, 1 center, 2 right). |
| `scrollTrigger` | number | — | 0–1. Hide the layer until the panel's scroll percentage reaches this value. Ignored by `images` frame-array layers. |
| `renderCondition` | table | — | `{ var = "name", equals = value }` — draw only while `Panels.vars[var] == value` (see [Branching](#panelsvars-and-branching)). |
| `animate` | table | — | Keyframed movement: `{ x?, y?, duration, ease?, delay?, scrollTrigger? }`. `duration` (ms) is required. Arms once when `pct` reaches `scrollTrigger` (default 0), then animates from the layer's `x`/`y` to `animate.x`/`animate.y` with easing `ease` (default `"cubicOut"`; one of the [`animator.new` easing names](API-Display-and-Graphics.md#picocalcgraphicsanimationanimatornewdurationms-from-to--easing--delayms), and an unknown name is logged as `PANELS:ERR unknown ease "<name>", using linear` and moves linearly) after `delay` ms (default 0). Re-arms if the panel scrolls off-screen and back. |
| `effect` | table | — | `{ type = "shake", strength = 3 }` or `{ type = "blink", onMs = 400, offMs = 300 }`. |
| `transparentColor` | number \| true | — | Colour-key transparency for the layer's image. `true` selects the conventional magenta key `Panels.MAGENTA_KEY` (`rgb(255, 0, 254)`). Note: pure black (`0`) cannot be used as a key. |
| `visible` | boolean | `true` | `false` hides the layer entirely. |
| `opacity` | number | — | **Binary**: values below 0.5 hide the layer, 0.5 and above draw it fully (no alpha blending — see [Playdate deltas](#differences-from-playdate-panels)). |

### Choices

A panel with a `choices` table becomes a branching point: when it settles as the centre panel, a menu opens (Up/Down to select, Enter to confirm).

```lua
choices = {
    prompt = "What now?",                     -- optional heading
    { text = "Stay and fight", target = 5, setVars = { stayed = true } },
    { text = "Walk away",      target = 5, setVars = { stayed = false } },
},
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `prompt` | string | no | Heading above the options (a `prompt` field on the panel itself also works). |
| `[i].text` | string | yes | Option label. |
| `[i].target` | number | yes | Sequence index to jump to; validated to exist. |
| `[i].setVars` | table | no | Key/value pairs merged into `Panels.vars` before the jump. |

---

## `Panels.vars` and branching

`Panels.vars` is a plain table of story variables. Choices write it via `setVars`; layers read it via `renderCondition`; your own callbacks may read or write it freely. It is included in the save file, so state survives quit/resume.

```lua
-- Later sequence reacts to an earlier choice:
{ text = "You stayed. The city remembers.", x = 30, y = 140,
  renderCondition = { var = "stayed", equals = true } },
{ text = "You walked. The road remembers.", x = 30, y = 140,
  renderCondition = { var = "stayed", equals = false } },
```

---

## `Panels.Settings`

Global tuning table — assign before (or after) `Panels.new`; most values are read every frame.

| Field | Default | Description |
|-------|---------|-------------|
| `scrollSpeed` | `180` | Scroll velocity in pixels per second in `"scroll"` mode. Movement is scaled by real frame time, so the speed is identical on hardware and simulator regardless of frame rate. |
| `repeatDelayMs` | `180` | Key-repeat initial delay. |
| `repeatRateMs` | `33` | Key-repeat interval. |
| `advanceMs` | `400` | Snap animation duration in `"advance"` mode. |
| `defaultFrameSize` | `320` | Panel extent along the axis when `frame` omits it. |
| `maxCachedPanels` | `3` | Resident panel window (previous/current/next). |
| `minFreeBytes` | `716800` (700 KB) | PSRAM headroom floor: below it, caching stops and eviction runs. |
| `borderWidth` | `2` | Panel border thickness (0 disables). |
| `borderColor` | `rgb(40, 40, 48)` | Panel border colour. |
| `backgroundColor` | `rgb(12, 12, 16)` | Default clear/fill colour. |
| `textColor` | `rgb(235, 235, 235)` | Default text-layer colour. |
| `choiceColor` | `rgb(255, 210, 80)` | Choice-menu highlight colour. |
| `transitionMs` | `450` | Fade transition duration. |
| `idleSkip` | `true` | Skip draw+flush on provably static frames (no motion, no pending triggers). |
| `useHwScroll` | `true` | LCD hardware scrolling for rigid vertical scroll sequences (see below). Set `false` to force the software path. |
| `volume` | `nil` | Reserved; currently unused (`nil` = leave the system volume alone). |

---

## Hardware-accelerated scrolling

Vertical `"scroll"` sequences whose panels are **rigid** scroll in the LCD
silicon instead of being redrawn: the library lays the sequence into the
panel's frame memory as a mod-320 ring, and each frame draws only the newly
revealed strip (a few rows) before bumping the scroll register
(`display.setScrollOffset`). Full-screen blits and the 205 KB frame DMA
disappear from the loop, so scrolling is smooth regardless of how heavy the
artwork is.

A panel is rigid when its pixels depend only on its position — concretely,
when it has **no**:

- layer `parallax` greater than 0 (`0`/omitted is fine)
- `animate` blocks, `images` frame arrays, or layer `scrollTrigger`s
- `shake`/`blink` effects (panel or layer)
- `renderFunction` / `updateFunction`

Audio `scrollTrigger`s are fine — they change no pixels and are evaluated
against the true scroll position every frame. Choices are fine too: the
choice UI exits the fast path when it opens. If any panel in a sequence is
not rigid, the whole sequence uses the software path; nothing changes except
frame rate. The demo's "The Descent" sequence is a worked example.

Embedders driving `Panels.new` directly must respect `comic.drewThisFrame`:
when it is `false`, the library flushed its own strips and a full
`display.flush()` would scramble the ring (see the header example in
`panels.lua`).

On-device screenshot tools read the draw framebuffer, which holds the ring
layout while the fast path is active — captures of a scrolling sequence look
rotated. The simulator composes screenshots through its scroll emulation, so
sim captures show the true screen.

---

## Save and resume

With `{ resume = true }`, the comic saves and restores automatically:

- **What is saved:** the current sequence index and `Panels.vars` — not the scroll position within a sequence.
- **When:** on entering a new sequence, and on **Esc** quit.
- **Where:** via `picocalc.game.save`, under the key `panels_<APP_ID>_<name>` (non-alphanumeric characters replaced with `_`) in the app's own save slots (`/data/<app id>/saves/`). The long key is kept so progress saved by older firmware in the shared `/saves` directory is picked up.
- **Cleared:** automatically when the comic finishes, so a completed comic restarts from the beginning.

On firmware without `picocalc.game.save`, saving degrades to a no-op.

---

## Memory and asset caching

The unit of residency is the **panel**: all of a panel's layer images load together and evict together.

- A window of `Settings.maxCachedPanels` (default 3: previous/current/next) around the viewport stays resident.
- Panels entering the viewport load synchronously (a one-frame hitch on first sight is accepted); the wider window is warmed **asynchronously only** — a single background preload on Core 1 (`graphics.image.preload`) decodes the next panel's first missing image, so fast scrolling never stalls on an off-screen panel.
- When free PSRAM drops below `Settings.minFreeBytes` (700 KB), prefetching stops and images outside the window are evicted.
- Entering a new sequence drops **all** cached assets of the outgoing sequence.

Practical guidance: keep per-panel image totals comfortably under the headroom floor; panel-sized art (320-wide strips) caches and evicts predictably.

---

## Asset sources

By default, `Panels.fileSource(APP_DIR)` resolves layer image names under `<APP_DIR>/images/` and audio file names under `<APP_DIR>/audio/`. Names are validated: no `..`, no leading `/`, no backslashes — a comic table can never reach outside its asset directories.

A custom source is any table with the same shape:

```lua
Panels.start(comic, {
    source = {
        imagePath = function(name) return "/data/mycomic/art/" .. name end,
        audioPath = function(name) return "/data/mycomic/snd/" .. name end,
    },
})
```

---

## Constants

| Constant | Value |
|----------|-------|
| `Panels.ScrollAxis.VERTICAL` | `1` |
| `Panels.ScrollAxis.HORIZONTAL` | `2` |
| `Panels.ScrollDirection.FORWARD` | `1` |
| `Panels.ScrollDirection.REVERSE` | `-1` |
| `Panels.Effect.SHAKE` | `"shake"` |
| `Panels.Effect.BLINK` | `"blink"` |
| `Panels.MAGENTA_KEY` | `picocalc.display.rgb(255, 0, 254)` — conventional transparency key |

---

## Error handling

- `Panels.validate` (run by `new`/`start`) fails loudly with a path to the mistake, e.g. `panels: sequences[1].panels[2].layers[1]: parallax must be a number 0..1`.
- Runtime errors in `updateFunction`/`renderFunction` and validation errors surface on a full-screen **PANELS FAULT** display (also logged as `PANELS:ERR`); **Esc** returns `"error", err` from `Panels.start`.
- Missing images and failed audio loads are logged and skipped — the comic keeps running. An unknown `animate.ease` name is logged once and replaced by `"linear"`.
- On older firmware the library degrades instead of crashing: without the clip rect, layers overhang their panels; without `game.save`, progress is not persisted; without `picocalc.sound`, audio cues are silent.

---

## Differences from Playdate Panels

| Playdate | PicoDeck |
|----------|-------|
| Crank scrolling | **D-pad** scroll/advance along the sequence axis |
| 1-bit display | **RGB565 colour** (320×320) |
| Alpha-blended layer opacity | **Binary opacity** — a layer is drawn or it isn't (`opacity < 0.5` hides it) |
| Crossfade / capture-based transitions | **Fade-to-colour only** (`"fadeToBlack"`, `"fadeToWhite"`, `"cut"`) — no framebuffer capture yet |

---

## Complete example

An abridged version of the bundled `panels_demo` app (each sequence exercises one feature group):

```lua
local Panels = picocalc.sys.loadlib("panels")
local disp   = picocalc.display
local input  = picocalc.input

local yellow = disp.rgb(255, 210, 80)

local comic = {
    name = "Panels Demo",
    sequences = {

        -- 1: vertical scroll, three-layer parallax
        {
            scrollType = "scroll",
            panels = {
                {
                    frame = { height = 480 },
                    layers = {
                        { image = "s1_sky.png",  y = -110, parallax = 0.15 },
                        { image = "s1_city.png", y = -150, parallax = 0.5,
                          transparentColor = true },
                        { text = "The city never sleeps.", x = 20, y = 40 },
                    },
                },
            },
        },

        -- 2: advance mode, keyframed slide-in, blink effect
        {
            scrollType = "advance",
            advanceControl = input.BTN_RIGHT,
            panels = {
                {
                    layers = {
                        { image = "s2_bg.png" },
                        { image = "s2_hero.png", x = -80, y = 190,
                          transparentColor = true,
                          animate = { x = 60, duration = 700, ease = "cubicOut" } },
                        { image = "s2_alert.png", x = 190, y = 60,
                          transparentColor = true,
                          effect = { type = "blink", onMs = 350, offMs = 250 } },
                    },
                },
            },
        },

        -- 3: auto-advance, frame array, sfx + looping bgm
        {
            scrollType = "auto",
            autoAdvanceMs = 2200,
            audio = { file = "bgm.wav", loop = true },
            panels = {
                {
                    frame = { height = 360 },
                    audio = { file = "chime.wav", scrollTrigger = 0.3 },
                    layers = {
                        { images = { "s3_dawn1.png", "s3_dawn2.png", "s3_dawn3.png" } },
                        { text = "Morning came anyway.", x = 40, y = 30 },
                    },
                },
            },
        },

        -- 4: branching choice sets a story var
        {
            scrollType = "advance",
            panels = {
                {
                    layers = { { text = "Stay or go?", x = 100, y = 40, color = yellow } },
                    choices = {
                        prompt = "What now?",
                        { text = "Stay and fight", target = 5, setVars = { stayed = true } },
                        { text = "Walk away",      target = 5, setVars = { stayed = false } },
                    },
                },
            },
        },

        -- 5: renderCondition payoff + custom render callback
        {
            scrollType = "advance",
            transition = "fadeToBlack",
            panels = {
                {
                    borderless = true,
                    layers = {
                        { text = "You stayed. The city remembers.", x = 30, y = 140,
                          renderCondition = { var = "stayed", equals = true } },
                        { text = "You walked. The road remembers.", x = 30, y = 140,
                          renderCondition = { var = "stayed", equals = false } },
                    },
                    renderFunction = function(panel, ox, oy, pct)
                        disp.fillRect(ox + 30, oy + 156, math.floor(80 + 60 * pct), 2, yellow)
                    end,
                },
            },
        },
    },
}

local result, err = Panels.start(comic, { resume = true })
picocalc.sys.log("comic exited: " .. tostring(result))
```
