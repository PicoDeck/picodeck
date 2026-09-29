---@meta
-- PicoDeck Lua API stubs for LuaLS (lua-language-server).
-- This file is never executed — it exists solely to provide IDE autocomplete,
-- hover documentation, and type checking for PicoDeck app development.
--
-- Place `.luarc.json` at the project root and set:
--   "Lua.workspace.library": ["sdk/lua"]
--
-- Generated from src/os/lua_bridge_*.c — keep in sync with the bridge sources.
-- tools/check_lua_stub.py (run by `pytest tests/unit`) compares every signature
-- here with the C function behind it: parameter count, optional parameters and
-- return count.
-- Note: only base/table/string/math stdlib is available (no utf8, no coroutine,
-- no io/os/package/debug).

-- =============================================================================
-- App globals (injected by launcher before app starts)
-- =============================================================================

---Absolute path to the app's directory on the SD card, e.g. `"/apps/hello"`.
---@type string
APP_DIR = ""

---Human-readable display name from `app.json`.
---@type string
APP_NAME = ""

---Reverse-domain app identifier from `app.json`, e.g. `"com.example.hello"`.
---@type string
APP_ID = ""

---Requirements granted to this app (booleans): `root_filesystem`, `http`,
---`audio`. A convenience copy: the OS enforces the C-side identity, so
---rewriting it grants nothing. `"sysconfig"` and `"system-update"` have no
---field here; test `picocalc.sysconfig ~= nil` / `picocalc.sys.applyUpdate ~= nil`
---(each is registered only for an app that declares the requirement).
---@type { root_filesystem: boolean, http: boolean, audio: boolean }
APP_REQUIREMENTS = {}

-- =============================================================================
-- picocalc  (top-level namespace)
-- =============================================================================

---@class picocalc
picocalc = {}

-- =============================================================================
-- picocalc.display
-- =============================================================================

---@class picocalc.display
---@field BLACK   integer RGB565 black  (0x0000)
---@field WHITE   integer RGB565 white  (0xFFFF)
---@field RED     integer RGB565 red
---@field GREEN   integer RGB565 green
---@field BLUE    integer RGB565 blue
---@field YELLOW  integer RGB565 yellow
---@field CYAN    integer RGB565 cyan
---@field GRAY    integer RGB565 gray
---@field FONT_6X8            integer Built-in 6×8 bitmap font
---@field FONT_8X12           integer Built-in 8×12 bitmap font
---@field FONT_SCIENTIFICA    integer Scientifica: monospace 6×12, includes box-drawing glyphs 0x80-0x9F
---@field FONT_SCIENTIFICA_BOLD integer Scientifica Bold: monospace 6×12, includes box-drawing glyphs 0x80-0x9F
picocalc.display = {}

---Clear the display to a solid colour (default: BLACK).
---@param color? integer RGB565 fill colour
function picocalc.display.clear(color) end

---Set a single pixel.
---@param x integer
---@param y integer
---@param color integer RGB565 colour
function picocalc.display.setPixel(x, y, color) end

---Fill a solid rectangle.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param color integer RGB565 colour
function picocalc.display.fillRect(x, y, w, h, color) end

---Draw a rectangle outline.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param color integer RGB565 colour
function picocalc.display.drawRect(x, y, w, h, color) end

---Draw a line between two points.
---@param x0 integer
---@param y0 integer
---@param x1 integer
---@param y1 integer
---@param color integer RGB565 colour
function picocalc.display.drawLine(x0, y0, x1, y1, color) end

---Draw a circle outline.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r integer Radius
---@param color integer RGB565 colour
function picocalc.display.drawCircle(cx, cy, r, color) end

---Fill a solid circle.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r integer Radius
---@param color integer RGB565 colour
function picocalc.display.fillCircle(cx, cy, r, color) end

---Fill a vertical line (fast; useful for raycasting and column effects).
---@param x integer
---@param y0 integer Start y
---@param y1 integer End y
---@param color integer RGB565
function picocalc.display.fillVLine(x, y0, y1, color) end

---Fill a horizontal line (fast).
---@param y integer
---@param x0 integer Start x
---@param x1 integer End x
---@param color integer RGB565
function picocalc.display.fillHLine(y, x0, x1, color) end

---Fill a solid triangle.
---@param x0 integer
---@param y0 integer
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@param color integer RGB565
function picocalc.display.fillTriangle(x0, y0, x1, y1, x2, y2, color) end

---Draw a textured vertical column (raycasting wall slice) from an image.
---tex_x selects the texture column; tex_y0/tex_y1 select the source row range.
---@param x integer Destination column
---@param y0 integer Destination start y
---@param y1 integer Destination end y
---@param tex PicoDeckImage Texture image
---@param tex_x integer Texture column
---@param tex_y0 integer Texture start row
---@param tex_y1 integer Texture end row
function picocalc.display.drawTexturedColumn(x, y0, y1, tex, tex_x, tex_y0, tex_y1) end

---Fill a vertical line with a two-colour gradient (sky/floor shading).
---@param x integer
---@param y0 integer Start y
---@param y1 integer End y
---@param color_top integer RGB565 at y0
---@param color_bottom integer RGB565 at y1
function picocalc.display.fillVLineGradient(x, y0, y1, color_top, color_bottom) end

---Apply a full-framebuffer post-processing effect.
---Effects and arguments:
---  "invert"
---  "darken"   (factor? 0=black, 255=no change, default 128)
---  "brighten" (factor? 0=no change, 255=white, default 128)
---  "tint"     (r, g, b, strength? default 128)
---  "fade"     (r, g, b, factor? default 128) — tint toward target colour
---  "grayscale"
---  "blend"    (image: PicoDeckImage, alpha? 0-255, default 128)
---  "palette"  (lut: integer[] 1-256 RGB565 entries — remap each pixel to nearest entry)
---  "dither"   (levels? default 4)
---  "scanline" (intensity? 0=none, 255=black lines, default 128)
---  "posterize"(levels? 2-32 per channel, default 4)
---@param name string Effect name (see above)
---@param ... any Effect arguments
function picocalc.display.applyEffect(name, ...) end

---Restrict all drawing primitives to a rectangle (split-screen, panels,
---partial redraw). `clear()` and post-effects are NOT clipped.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function picocalc.display.setClipRect(x, y, w, h) end

---Return the current clip rectangle.
---@return integer x
---@return integer y
---@return integer w
---@return integer h
function picocalc.display.getClipRect() end

---Restore the clip rectangle to the full screen.
function picocalc.display.clearClipRect() end

---Render a Mode 7-style perspective ground plane from an image (SNES F-Zero /
---Mario Kart floor). The camera sits at (cam_x, cam_y) in texture space,
---`cam_z` units above the plane, facing `angle` radians (0 = toward +Y).
---Rows below `horizon_y` are filled. Power-of-two texture dimensions wrap
---seamlessly; other sizes clamp at edges. Respects the clip rect.
---@param tex PicoDeckImage Ground texture
---@param cam_x number Camera x in texture space
---@param cam_y number Camera y in texture space
---@param cam_z number Camera height above the plane
---@param angle? number Facing in radians (default 0)
---@param horizon_y? integer Horizon scanline (default 120)
---@param scale? number FOV/zoom tuning, larger = further view (default 1.0)
function picocalc.display.drawPlane(tex, cam_x, cam_y, cam_z, angle, horizon_y, scale) end

---Configure the hardware scroll area (ST7365P VSCRDEF). Frame memory is 480
---lines (visible panel = 0..319); the three values must sum to 480. The
---standard ring configuration is setScrollArea(0, 320, 160).
---@param top integer Fixed rows at the top of frame memory
---@param height integer Height of the scrolling region in lines
---@param bottom integer Fixed rows at the bottom of frame memory
function picocalc.display.setScrollArea(top, height, bottom) end

---Set the hardware vertical scroll offset within the scroll region. Instant
---register remap; waits out any in-flight flush DMA. 0 restores identity.
---@param offset integer Frame-memory row shown at the top of the scroll area
function picocalc.display.setScrollOffset(offset) end

---Last offset written with setScrollOffset plus a write counter. The OS
---resets the offset to 0 on screen takeovers (system menu, app switch);
---poll both values each frame to detect that and repaint.
---@return integer offset Last written scroll offset
---@return integer writeCount Total register writes since boot
function picocalc.display.getScrollOffset() end

---Draw a text string. Background defaults to BLACK if omitted; pass `false`
---for a transparent background (glyph pixels only).
---Returns the pixel width of the rendered text.
---@param x integer
---@param y integer
---@param text string
---@param fg integer RGB565 foreground colour
---@param bg? integer|false RGB565 background colour, or `false` for transparent
---@return integer width Pixel width of the drawn text
function picocalc.display.drawText(x, y, text, fg, bg) end

---Flush the framebuffer to the LCD (non-blocking DMA). Call once per frame.
function picocalc.display.flush() end

---Present only rows y0..y1 (inclusive) of the current draw buffer to the LCD
---WITHOUT swapping buffers (non-blocking DMA). Rows clamp to 0-319. Drawing
---continues into the same buffer — ideal for repeated band updates (HUD,
---status line) while the rest of the screen keeps its last contents.
---@param y0 integer First row (inclusive)
---@param y1 integer Last row (inclusive)
function picocalc.display.flushRows(y0, y1) end

---Like `flush()` but transfers only rows y0..y1 (inclusive): swaps buffers,
---then re-syncs the flushed band into the new back buffer so both buffers
---match. Rows clamp to 0-319. Use in a normal double-buffered loop when only
---a horizontal band changed.
---@param y0 integer First row (inclusive)
---@param y1 integer Last row (inclusive)
function picocalc.display.flushRegion(y0, y1) end

---Returns the display width in pixels (320).
---@return integer
function picocalc.display.getWidth() end

---Returns the display height in pixels (320).
---@return integer
function picocalc.display.getHeight() end

---Set the LCD backlight brightness.
---@param brightness integer 0 (off) – 255 (maximum)
function picocalc.display.setBrightness(brightness) end

---Return the pixel width of a string in the current font, without drawing it.
---@param text string
---@return integer width
function picocalc.display.textWidth(text) end

---Select the active font for subsequent drawText/textWidth calls.
---@param font_id integer One of the FONT_* constants, or an id returned by loadFont. An id that is not currently loaded is ignored.
function picocalc.display.setFont(font_id) end

---Return the currently active font ID.
---@return integer
function picocalc.display.getFont() end

---Return the maximum glyph advance of the current font in pixels. For a
---proportional font this is the widest glyph, not every glyph's width; use
---textWidth to measure a specific string.
---@return integer
function picocalc.display.getFontWidth() end

---Return the glyph height of the current font in pixels.
---@return integer
function picocalc.display.getFontHeight() end

---Load a `.pfn` bitmap font from an absolute SD path (sandbox-checked, like
---image loading). Returns an id (4-11, at most 8 loaded at once) for use
---with setFont, or nil on sandbox denial or load failure. Never raises.
---Every font an app loads is freed automatically when it exits, or earlier
---via unloadFont.
---@param path string Absolute path to a `.pfn` file
---@return integer? id Font id (4-11), or nil on failure
function picocalc.display.loadFont(path) end

---Free a font previously returned by loadFont. If it is the active font,
---the active font falls back to FONT_6X8 first. No-op for built-in ids
---(0-3) or an id that is not currently loaded.
---@param id integer Font id returned by loadFont
function picocalc.display.unloadFont(id) end

---Convert 8-bit R/G/B components to a packed RGB565 colour integer.
---@param r integer 0–255
---@param g integer 0–255
---@param b integer 0–255
---@return integer color RGB565
function picocalc.display.rgb(r, g, b) end

-- =============================================================================
-- picocalc.input
-- =============================================================================

---@class picocalc.input
---@field BTN_UP        integer D-pad up
---@field BTN_DOWN      integer D-pad down
---@field BTN_LEFT      integer D-pad left
---@field BTN_RIGHT     integer D-pad right
---@field BTN_ENTER     integer Enter / OK
---@field BTN_ESC       integer Escape / back
---@field BTN_MENU      integer System menu (Sym key)
---@field BTN_F1        integer Function key 1
---@field BTN_F2        integer Function key 2
---@field BTN_F3        integer Function key 3
---@field BTN_F4        integer Function key 4
---@field BTN_F5        integer Function key 5
---@field BTN_F6        integer Function key 6
---@field BTN_F7        integer Function key 7
---@field BTN_F8        integer Function key 8
---@field BTN_F9        integer Function key 9
---@field BTN_BACKSPACE integer Backspace
---@field BTN_TAB       integer Tab
---@field BTN_DEL       integer Delete (Fn+Backspace)
---@field BTN_SHIFT     integer Shift modifier
---@field BTN_CTRL      integer Ctrl modifier
---@field BTN_ALT       integer Alt modifier
---@field BTN_FN        integer Fn / Symbol modifier
picocalc.input = {}

---Poll the keyboard and trigger the system menu if Menu is pressed.
---Call once per frame before reading button state.
function picocalc.input.update() end

---Return a bitmask of all currently held buttons (BTN_* constants).
---@return integer bitmask
function picocalc.input.getButtons() end

---Return a bitmask of buttons pressed *this frame* (edge-detect, not held).
---@return integer bitmask
function picocalc.input.getButtonsPressed() end

---Return a bitmask of buttons released *this frame*.
---@return integer bitmask
function picocalc.input.getButtonsReleased() end

---Return the character typed this frame, or `nil` if none. One char per
---`update()`: when several keys arrive in one poll, the rest are returned by
---the following frames in order (up to 4 are kept). Holding a key repeats it.
---Enter returns `"\n"`, Backspace `"\b"`, Ctrl+letter a control code.
---Includes full keyboard layout; use for text input.
---@return string|nil
function picocalc.input.getChar() end

---Return the raw hardware key code for the last key event.
---@return integer
function picocalc.input.getRawKey() end

---Clear all latched input state (held buttons, pending edges, queued
---`pollEvent` events, `isKeyDown` state). Useful on scene transitions so
---stale presses don't leak into the new scene.
function picocalc.input.clearState() end

---Configure key auto-repeat for menus and lists.
---@param delay_ms integer Initial hold time before repeat starts (0 = disable repeat)
---@param rate_ms? integer Interval between repeats once started (default 80, min 1)
function picocalc.input.setRepeat(delay_ms, rate_ms) end

---Like `getButtonsPressed()`, but held buttons also produce synthetic repeat
---edges according to `setRepeat()`. Use this for menu navigation.
---@return integer bitmask
function picocalc.input.getButtonsRepeated() end

---@class picocalc.input.Event
---@field type "down"|"up"|"char"
---@field key integer Keycode: ASCII for printable keys, else the STM32 code (as `getRawKey()`; e.g. Up=0xB5, Esc=0xB1, Ctrl=0xA5)
---@field char? string `"char"` events only: the character (as `getChar()` would return it)
---@field mods integer BTN_SHIFT/BTN_CTRL/BTN_ALT/BTN_FN held at the event
---@field button? integer The BTN_* constant for keys that have one
---@field repeat? boolean true for a `down`/`char` produced by holding the key (read it as `ev["repeat"]`: `repeat` is a Lua keyword)

---Pop the oldest keyboard event, or `nil` when none is queued. Events are
---filled by `update()` in the order keys were pressed and released, so taps
---shorter than a frame and several chars in one frame are all reported. A
---key press gives `down` (+ `char` if it types one), its release `up`.
---Independent of `getChar()`/`getButtons*()`: reading one does not consume
---the other. 16 events are kept (the oldest is dropped); a new app starts
---with an empty queue.
---```lua
---input.update()
---for ev in input.pollEvent do
---  if ev.type == "char" then text = text .. ev.char end
---end
---```
---@return picocalc.input.Event|nil
function picocalc.input.pollEvent() end

---True while a key is held. `k` is a one-character string (`"w"`; letters
---ignore case) or an integer keycode as `pollEvent` reports it. Updated by
---`update()`. Reliable for buttons (arrows, Enter, Esc, F-keys, modifiers);
---letters and shifted symbols rely on the keyboard reporting their release and
---are pending hardware confirmation. `clearState()` clears a key that sticks.
---@param k string|integer
---@return boolean
function picocalc.input.isKeyDown(k) end

-- =============================================================================
-- picocalc.sys
-- =============================================================================

---@class picocalc.sys
picocalc.sys = {}

---Return milliseconds elapsed since boot.
---@return integer
function picocalc.sys.getTimeMs() end

---Microseconds since boot, wrapping modulo 2^32 (negative after ~35.8 min).
---Subtract two readings as integers; exact for gaps under ~35 minutes.
---@return integer
function picocalc.sys.getTimeUs() end

---Return battery charge (0–100), or -1 if unknown / USB-powered.
---Result is cached for ~5 seconds to avoid slow I²C reads.
---@return integer
function picocalc.sys.getBattery() end

---Log a message to the USB serial port (115200 baud). Prefix: `[APP]`.
---@param message string
function picocalc.sys.log(message) end

---Sleep for `ms` milliseconds. HTTP/TCP callbacks are processed while sleeping.
---@param ms integer
function picocalc.sys.sleep(ms) end

---Exit the app cleanly and return to the launcher. Never returns.
function picocalc.sys.exit() end

---Reboot the device via the watchdog timer. Never returns.
function picocalc.sys.reboot() end

---Return `true` if USB power is connected (GP24 VBUS sense).
---@return boolean
function picocalc.sys.isUSBPowered() end

---Return a table with charging/percentage info.
---@return { charging: boolean, percent: integer }
function picocalc.sys.getPowerStatus() end

---Return current time, optionally NTP-synced.
---@return { synced: boolean, hour: integer, min: integer, sec: integer, epoch: integer }
function picocalc.sys.getClock() end

---Return a snapshot of heap usage (bytes). `psram_largest_block` is the biggest
---single free block (what one large allocation can get; compare with
---`min_psram_kb`), `psram_fragmentation` a 0-100 figure. The `small_pool_*`
---fields describe the Lua small-object pools (objects of up to 128 B live in
---4 KB slabs carved from the PSRAM heap): slab count, slab bytes, live objects
---and their bytes. `pio_psram_*` describe the mainboard PIO PSRAM.
---@return { psram_free: integer, psram_used: integer, psram_total: integer, psram_largest_block: integer, psram_fragmentation: integer, small_pool_slabs: integer, small_pool_bytes: integer, small_pool_objects: integer, small_pool_object_bytes: integer, sram_free: integer, sram_used: integer, pio_psram_available: boolean, pio_psram_size: integer }
function picocalc.sys.getMemInfo() end

---Return the OS firmware version string.
---@return string
function picocalc.sys.getVersion() end

---Apply an OTA firmware update from a raw `.bin` image. Needs
---`/system/update.sha256` (exactly 64 hex digits, the SHA-256 of the image)
---and `/system/update.sig` (DER ECDSA P-256 signature of the image by the
---PicoDeck update key the firmware was built with); refuses the image otherwise.
---Then asks the user to confirm; reboots on success. **Only present** for OS apps (id
---`net.picodeck.updater`/`net.picodeck.store`, or under `/system/`) that declare the
---`"system-update"` requirement; nil otherwise.
---@param path string Absolute SD card path to the `.bin` file
---@return boolean ok false with an error ("cancelled" if declined)
---@return string? error
function picocalc.sys.applyUpdate(path) end

---Register a custom item in the system-menu overlay (max 4 per app).
---@param label string Menu item text
---@param callback fun() Called when the item is selected
function picocalc.sys.addMenuItem(label, callback) end

---Remove all app-registered menu items. Called automatically on app exit.
function picocalc.sys.clearMenuItems() end

---Deliberately trigger a HardFault for crash-handler testing. **Never call in production.**
function picocalc.sys.triggerFault() end

---Reset the idle screen-dim timer (call on user activity to keep the screen on).
function picocalc.sys.resetIdleTimer() end

---Pause Core 1 background work (network polling, audio updates).
---@return boolean was_paused Previous state
function picocalc.sys.pauseBackground() end

---Resume Core 1 background work after `pauseBackground()`.
function picocalc.sys.resumeBackground() end

---Load and run a Lua library file from the SD card (like `require` for a path).
---Returns the chunk's return value, or `nil, error`.
---@param name string Module path or name
---@return any
function picocalc.sys.loadlib(name) end

---Read bytes from PIO PSRAM (second 8MB chip, if present) into a string.
---Apps may use addresses from 0x48000 (288 KB) to the end of the chip; the
---range below is the OS's (MP3 ring, video) and raises an error, as does
---anything negative or past the chip.
---@param addr integer Byte address in PIO PSRAM (>= 0x48000)
---@param len integer Bytes to read
---@return string? data nil if PIO PSRAM unavailable
function picocalc.sys.pioPsramRead(addr, len) end

---Write a string to PIO PSRAM. Returns bytes written (0 if unavailable).
---Same address rules as `pioPsramRead` (errors below 0x48000).
---@param addr integer Byte address in PIO PSRAM (>= 0x48000)
---@param data string
---@return integer bytes_written
function picocalc.sys.pioPsramWrite(addr, data) end

---Return the PIO PSRAM size in bytes (0 if not present).
---@return integer
function picocalc.sys.pioPsramSize() end

---Allocate a buffer from the QMI PSRAM (umm_malloc) heap.
---Returns a bounds-checked buffer handle, or nil on OOM. The block is freed
---by `qmiPsramFree` or when the handle is garbage-collected.
---@param size integer Bytes to allocate (> 0)
---@return userdata? handle
function picocalc.sys.qmiPsramAlloc(size) end

---Free a buffer from `qmiPsramAlloc` (idempotent; later access raises).
---@param handle userdata
function picocalc.sys.qmiPsramFree(handle) end

---Write a string into a QMI PSRAM buffer at `offset`. Returns bytes written.
---Raises if offset+#data is past the buffer or the buffer was freed.
---@param ptr userdata Handle from qmiPsramAlloc
---@param offset integer Byte offset into the allocation
---@param data string
---@return integer bytes_written
function picocalc.sys.qmiPsramWrite(ptr, offset, data) end

---Read bytes from a QMI PSRAM buffer into a string (raises when out of range).
---@param ptr userdata Handle from qmiPsramAlloc
---@param offset integer Byte offset into the allocation
---@param len integer Bytes to read
---@return string? data
function picocalc.sys.qmiPsramRead(ptr, offset, len) end

-- =============================================================================
-- picocalc.fs
-- =============================================================================

---@class picocalc.fs
picocalc.fs = {}

---An open file.  Full userdata owned by Lua: a dropped handle is closed by
---the garbage collector, `local f <close> = picocalc.fs.open(...)` closes at
---scope exit, and files still open when the app exits are closed by the OS.
---Using a closed handle raises "attempt to use a closed file"; `close` is
---idempotent.  Methods mirror the `picocalc.fs` functions (`h:read(n)` ==
---`picocalc.fs.read(h, n)`).  At most 16 files can be open at once (FatFS).
---@class PicoDeckFile : userdata
local PicoDeckFile = {}

---Read up to `len` bytes (clamped to what is left in the file).
---@param len integer Must be >= 0
---@return string? data `nil` at end of file or on error
function PicoDeckFile:read(len) end

---Write data. Returns bytes written (-1 on error).
---@param data string
---@return integer bytes_written
function PicoDeckFile:write(data) end

---Close the file (no-op if already closed).
function PicoDeckFile:close() end

---Seek to an absolute byte offset.
---@param offset integer Must be >= 0
---@return boolean ok
function PicoDeckFile:seek(offset) end

---Current byte offset.
---@return integer offset
function PicoDeckFile:tell() end

---Open a file on the SD card.
---@param path string Absolute SD card path
---@param mode? string `"r"` (default), `"w"`, `"a"`, `"r+"`, etc.
---@return PicoDeckFile? handle
---@return string? error `"permission denied"`, `"cannot open file"` or `"too many open files"`
function picocalc.fs.open(path, mode) end

---Read up to `len` bytes from an open file (clamped to what is left in it).
---Raises on a closed handle or a negative `len`.
---@param file PicoDeckFile
---@param len integer
---@return string? data `nil` on EOF or error
function picocalc.fs.read(file, len) end

---Write data to an open file. Returns bytes written (-1 on error).
---Raises on a closed handle.
---@param file PicoDeckFile
---@param data string
---@return integer bytes_written
function picocalc.fs.write(file, data) end

---Close an open file handle. Idempotent; `nil` is ignored.
---@param file PicoDeckFile?
function picocalc.fs.close(file) end

---Seek to an absolute byte offset within an open file.
---@param file PicoDeckFile
---@param offset integer Must be >= 0
---@return boolean ok
function picocalc.fs.seek(file, offset) end

---Return the current byte offset within an open file.
---@param file PicoDeckFile
---@return integer offset
function picocalc.fs.tell(file) end

---Return `true` if a path exists on the SD card.
---@param path string
---@return boolean
function picocalc.fs.exists(path) end

---Read an entire file into a string.
---Returns `nil` when the path is denied, missing or unreadable. A file too
---big for free memory raises a memory error ("not enough memory") instead of
---returning `nil`; use `pcall` when the size is not known to fit.
---@param path string
---@return string? contents `nil` on error
function picocalc.fs.readFile(path) end

---Return the size of a file in bytes, or -1 if the file does not exist.
---@param path string
---@return integer
function picocalc.fs.size(path) end

---@class PicoDeckDirEntry
---@field name string File or directory name (not full path)
---@field is_dir boolean `true` for directories
---@field size integer File size in bytes (0 for directories)
---@field year? integer
---@field month? integer
---@field day? integer
---@field hour? integer
---@field min? integer
---@field sec? integer

---List directory contents.
---@param path string
---@return PicoDeckDirEntry[]
function picocalc.fs.listDir(path) end

---Create a directory (and any missing parents).
---@param path string
---@return boolean ok
function picocalc.fs.mkdir(path) end

---Return the writable per-app data path for `name`, i.e. `/data/<APP_ID>/name`.
---@param name string Relative filename
---@return string path
function picocalc.fs.appPath(name) end

---Show a fullscreen file-browser UI and return the selected path, or `nil` if cancelled.
---@param start_path? string Initial directory
---@return string? selected_path
function picocalc.fs.browse(start_path) end

---Delete a file or empty directory.
---@param path string
---@return boolean ok
---@return string? error
function picocalc.fs.delete(path) end

---Rename/move a file or directory.
---@param src string
---@param dst string
---@return boolean ok
---@return string? error
function picocalc.fs.rename(src, dst) end

---Copy a file. Optional progress callback receives `(done: integer, total: integer)`.
---@param src string
---@param dst string
---@param progress_fn? fun(done: integer, total: integer)
---@return boolean ok
---@return string? error
function picocalc.fs.copy(src, dst, progress_fn) end

---@class PicoDeckStatResult
---@field size integer
---@field is_dir boolean
---@field year? integer
---@field month? integer
---@field day? integer
---@field hour? integer
---@field min? integer
---@field sec? integer

---Return metadata for a path.
---@param path string
---@return PicoDeckStatResult? info `nil` if path does not exist
---@return string? error
function picocalc.fs.stat(path) end

---Return free and total SD card space in kilobytes.
---@return { free: integer, total: integer }? info
---@return string? error
function picocalc.fs.diskInfo() end

---Return directory entries whose names match a glob pattern (`*` and `?`), case-insensitive.
---@param path string Directory to search
---@param pattern string Glob pattern, e.g. `"*.lua"`
---@return PicoDeckDirEntry[]
function picocalc.fs.glob(path, pattern) end

-- =============================================================================
-- picocalc.config / picocalc.appconfig  (per-app, /data/<APP_ID>/config.json)
-- Two names for the SAME per-app store. System-wide config is picocalc.sysconfig.
-- =============================================================================

---@class picocalc.config
picocalc.config = {}

---@class picocalc.appconfig
picocalc.appconfig = {}

---Read a per-app config value, returning `fallback` if the key is absent.
---@param key string
---@param fallback? string
---@return string? value
function picocalc.config.get(key, fallback) end

---Write a per-app config value (in memory only — call `save()` to persist).
---@param key string
---@param value string
function picocalc.config.set(key, value) end

---Persist the per-app config to `/data/<APP_ID>/config.json`.
---@return boolean ok
function picocalc.config.save() end

---Reload the per-app config from `/data/<APP_ID>/config.json`.
---@return boolean ok
function picocalc.config.load() end

---Clear all in-memory per-app entries (does not delete the file).
function picocalc.config.clear() end

---Delete the per-app config file and clear all keys from memory.
---@return boolean ok
function picocalc.config.reset() end

-- picocalc.appconfig is an exact alias of picocalc.config (same store):
picocalc.appconfig.get   = picocalc.config.get
picocalc.appconfig.set   = picocalc.config.set
picocalc.appconfig.save  = picocalc.config.save
picocalc.appconfig.load  = picocalc.config.load
picocalc.appconfig.clear = picocalc.config.clear
picocalc.appconfig.reset = picocalc.config.reset

-- =============================================================================
-- picocalc.sysconfig  (system-wide, /system/config.json)
-- =============================================================================

---**Only present** when the app's `app.json` declares the `"sysconfig"`
---requirement (nil otherwise).
---@class picocalc.sysconfig
picocalc.sysconfig = {}

---Read a system config value. Well-known keys: `"wifi_ssid"`,
---`"brightness"`, `"dim_timeout_s"`. `"wifi_pass"` is write-only: `get`
---always returns nil for it.
---@param key string
---@return string? value `nil` if key does not exist
function picocalc.sysconfig.get(key) end

---Write a system config value (pass `nil` to delete the key).
---@param key string
---@param value string|nil
function picocalc.sysconfig.set(key, value) end

---Persist system config to `/system/config.json`.
---@return boolean ok
function picocalc.sysconfig.save() end

---Reload system config from `/system/config.json`.
---@return boolean ok
function picocalc.sysconfig.load() end

-- =============================================================================
-- picocalc.audio  (simple tone generation)
-- =============================================================================

---@class picocalc.audio
picocalc.audio = {}

---Play a tone at the given frequency. If `duration_ms` is 0 or omitted the
---tone plays indefinitely until `stopTone()` is called.
---@param freq integer Frequency in Hz (e.g. 440 for A4)
---@param duration_ms? integer Duration in milliseconds; 0 = indefinite
function picocalc.audio.playTone(freq, duration_ms) end

---Stop the currently playing tone immediately.
function picocalc.audio.stopTone() end

---Set the master volume. It scales everything the mixer plays: tones,
---samples, the PCM stream (FilePlayer, MOD player) and MP3. It resets to
---100 when the app exits.
---@param volume integer 0 (mute) – 100 (maximum)
function picocalc.audio.setVolume(volume) end

-- =============================================================================
-- picocalc.sound  (sample / fileplayer / MP3 player)
-- =============================================================================

---@class picocalc.sound
picocalc.sound = {}

---@class PicoDeckSample : userdata
---Holds raw PCM audio data loaded from a WAV file.
local PicoDeckSample = {}

---@class PicoDeckSamplePlayer : userdata
local PicoDeckSamplePlayer = {}

---@class PicoDeckFilePlayer : userdata
local PicoDeckFilePlayer = {}

---@class PicoDeckMp3Player : userdata
local PicoDeckMp3Player = {}

-- ── picocalc.sound top-level constructors ────────────────────────────────────

---Return the audio clock time in milliseconds since the last `resetTime()`.
---@return integer ms
function picocalc.sound.getCurrentTime() end

---Reset the audio clock to zero.
function picocalc.sound.resetTime() end

---Return the number of currently active audio sources.
---@return integer
function picocalc.sound.playingSources() end

---Create a Sample, optionally loading a WAV file immediately. WAVs must be
---8- or 16-bit PCM, 1-2 channels (float, 24/32-bit, ADPCM are refused); only
---the first 64 KB of sample data is kept.
---@param path_or_duration? string|number WAV file path, or duration in seconds for an empty sample
---@param opts? {sampleRate?: integer, bits?: integer, channels?: integer} Format of an empty sample (with a duration only)
---@return PicoDeckSample? sample
---@return string? error
function picocalc.sound.sample(path_or_duration, opts) end

---Create a SamplePlayer, optionally pre-loading a sample.
---The player keeps its sample alive: dropping your own reference to the sample
---is safe while the player plays it. A path loads a Sample that belongs to the
---player (`getSample()` returns it); it is freed with the player, or once a
---`setSample` replaces it. Samples have no fixed limit; at most 8 sampleplayers
---exist at once, freed by the garbage collector.
---@param sample_or_path? PicoDeckSample|string
---@return PicoDeckSamplePlayer
function picocalc.sound.sampleplayer(sample_or_path) end

---Create a FilePlayer for streaming WAV files from the SD card.
---@param buffer_size? integer Internal streaming buffer size in bytes
---@return PicoDeckFilePlayer
function picocalc.sound.fileplayer(buffer_size) end

---Create an MP3Player for streaming MP3 files from the SD card.
---@return PicoDeckMp3Player
function picocalc.sound.mp3player() end

-- ── PicoDeckSample methods ──────────────────────────────────────────────────────

---Load a WAV file into this sample.
---@param path string
---@return boolean ok
---@return string? error
function PicoDeckSample:load(path) end

---Return the number of PCM sample frames.
---@return integer
function PicoDeckSample:getLength() end

---Return the sample rate in Hz (e.g. 44100).
---@return integer
function PicoDeckSample:getSampleRate() end

---Return format metadata.
---@return { bits: integer, channels: integer, sampleRate: integer }
function PicoDeckSample:getFormat() end

---Decompress the sample (if compressed). Returns `self` for chaining.
---@return PicoDeckSample
function PicoDeckSample:decompress() end

---Return a new Sample containing the sub-range `[start, end]` (sample frames).
---@param start_frame integer
---@param end_frame integer
---@return PicoDeckSample
function PicoDeckSample:getSubsample(start_frame, end_frame) end

---Play this sample immediately. `when` is accepted but ignored (no scheduler
---on this hardware — playback starts now).
---@param when number Reserved; ignored (but required)
---@param vol? integer Volume 0–100 (default 100; larger values clamp to 100)
---@param rightvol? integer Right volume (ignored — mono PWM)
---@param rate? number Playback rate multiplier (default 1.0)
---@return PicoDeckSamplePlayer player The player that is playing the sample (raises if none is free)
function PicoDeckSample:playAt(when, vol, rightvol, rate) end

-- ── PicoDeckSamplePlayer methods ────────────────────────────────────────────────

---Attach a sample to this player. The player keeps it alive and lets go of
---the previous one.
---@param sample PicoDeckSample
---@return boolean ok
function PicoDeckSamplePlayer:setSample(sample) end

---Return the currently attached sample (the same Sample object), or `nil`.
---@return PicoDeckSample?
function PicoDeckSamplePlayer:getSample() end

---Start playback. `repeat_count` = number of repetitions (0 = use loop flag).
---@param repeat_count? integer
---@return boolean ok
function PicoDeckSamplePlayer:play(repeat_count) end

---Stop playback.
function PicoDeckSamplePlayer:stop() end

---Return `true` while playing.
---@return boolean
function PicoDeckSamplePlayer:isPlaying() end

---Pause or resume playback.
---@param paused boolean
function PicoDeckSamplePlayer:setPaused(paused) end

---Return the sample length in frames.
---@return integer
function PicoDeckSamplePlayer:getLength() end

---Seek to a position in seconds.
---@param seconds number
function PicoDeckSamplePlayer:setOffset(seconds) end

---Return the current playback position in seconds.
---@return number
function PicoDeckSamplePlayer:getOffset() end

---Set playback volume.
---@param vol integer 0–100 (larger values clamp to 100)
function PicoDeckSamplePlayer:setVolume(vol) end

---Return the current volume.
---@return integer
function PicoDeckSamplePlayer:getVolume() end

---Restrict playback to a sub-range (in sample frames).
---@param start_frame integer
---@param end_frame integer
function PicoDeckSamplePlayer:setPlayRange(start_frame, end_frame) end

---Set playback rate multiplier (1.0 = normal speed).
---@param rate number
function PicoDeckSamplePlayer:setRate(rate) end

---Return the current playback rate.
---@return number
function PicoDeckSamplePlayer:getRate() end

-- ── PicoDeckFilePlayer methods ──────────────────────────────────────────────────

---Open a WAV file for streaming. 16-bit PCM only (1-2 channels): an 8-bit
---WAV that a Sample would accept is refused here (returns false).
---@param path string
---@return boolean ok
function PicoDeckFilePlayer:load(path) end

---Start streaming playback.
---@param repeat_count? integer 0 = infinite
---@return boolean ok
function PicoDeckFilePlayer:play(repeat_count) end

---Stop playback.
function PicoDeckFilePlayer:stop() end

---Pause playback.
function PicoDeckFilePlayer:pause() end

---Resume after pause.
function PicoDeckFilePlayer:resume() end

---@return boolean
function PicoDeckFilePlayer:isPlaying() end

---Return total file length in seconds.
---@return number
function PicoDeckFilePlayer:getLength() end

---Return current playback position in seconds.
---@return number
function PicoDeckFilePlayer:getOffset() end

---Seek to a position in seconds.
---@param seconds number
function PicoDeckFilePlayer:setOffset(seconds) end

---Set the volume. `right` is accepted for Playdate compatibility, but both
---channels play at `left`.
---@param left integer 0–100 (larger values clamp to 100)
---@param right? integer 0–100
function PicoDeckFilePlayer:setVolume(left, right) end

---Return the current left and right channel volumes.
---@return integer left
---@return integer right
function PicoDeckFilePlayer:getVolume() end

---Set the loop region (in seconds). Omit both args to loop the whole file.
---@param start_sec? number
---@param end_sec? number
function PicoDeckFilePlayer:setLoopRange(start_sec, end_sec) end

---Return `true` if a buffer underrun occurred since the last call.
---@return boolean
function PicoDeckFilePlayer:didUnderrun() end

---Register a callback called when playback finishes.
---@param fn fun()
function PicoDeckFilePlayer:setFinishCallback(fn) end

---If `true`, stop automatically on buffer underrun rather than filling with silence.
---@param flag boolean
function PicoDeckFilePlayer:setStopOnUnderrun(flag) end

-- ── PicoDeckMp3Player methods ───────────────────────────────────────────────────

---Open an MP3 file for streaming.
---@param path string
---@return boolean? ok `true`, or `nil` if the file cannot be opened or is not an MP3
---@return string? error
function PicoDeckMp3Player:load(path) end

---Start playback from the beginning of the file (also after it finished).
---A `play()` while playing fades out first (~1.5 ms, no click).
---@param repeat_count? integer 0 = infinite
---@return boolean ok
function PicoDeckMp3Player:play(repeat_count) end

---Stop playback (fades out over ~1.5 ms).
function PicoDeckMp3Player:stop() end

---Pause playback (fades out over ~1.5 ms).
function PicoDeckMp3Player:pause() end

---Resume after pause (fades back in).
function PicoDeckMp3Player:resume() end

---`true` until the last decoded audio has played.
---@return boolean
function PicoDeckMp3Player:isPlaying() end

---Return the frames played since `play()` (divide by `getSampleRate()` for
---seconds; it keeps counting across loops). `0` once stopped or finished.
---@return integer
function PicoDeckMp3Player:getPosition() end

---Return `0`: an MP3's length is not known without decoding all of it.
---@return integer
function PicoDeckMp3Player:getLength() end

---Return the sample rate of the MP3 stream in Hz.
---@return integer
function PicoDeckMp3Player:getSampleRate() end

---Set playback volume.
---@param vol integer 0–100 (larger values clamp to 100)
function PicoDeckMp3Player:setVolume(vol) end

---Return the current volume.
---@return integer
function PicoDeckMp3Player:getVolume() end

---Enable or disable looping.
---@param loop boolean
function PicoDeckMp3Player:setLoop(loop) end

-- =============================================================================
-- picocalc.wifi  (low-level WiFi control)
-- =============================================================================

---@class picocalc.wifi
---@field STATUS_DISCONNECTED integer
---@field STATUS_CONNECTING    integer
---@field STATUS_CONNECTED     integer IP assigned, internet NOT verified
---@field STATUS_FAILED        integer
---@field STATUS_ONLINE        integer Internet connectivity confirmed
picocalc.wifi = {}

---Return `true` if WiFi hardware is present on this device.
---@return boolean
function picocalc.wifi.isAvailable() end

---Initiate a connection to an SSID.
---@param ssid string
---@param password? string
function picocalc.wifi.connect(ssid, password) end

---Disconnect from the current network.
function picocalc.wifi.disconnect() end

---Return the current connection status (STATUS_* constant).
---@return integer
function picocalc.wifi.getStatus() end

---Return the current IP address string, or `nil` if not connected.
---@return string?
function picocalc.wifi.getIP() end

---Return the connected SSID, or `nil` if not connected.
---@return string?
function picocalc.wifi.getSSID() end

-- =============================================================================
-- picocalc.network  (HTTP client)
-- =============================================================================

---@class picocalc.network
---@field kStatusNotConnected integer WiFi not connected (0)
---@field kStatusConnected     integer WiFi connected (1)
---@field kStatusNotAvailable  integer WiFi hardware absent (2)
picocalc.network = {}

---Return the current network status (kStatus* constant).
---@return integer
function picocalc.network.getStatus() end

---Enable or disable WiFi. `callback` is fired synchronously with `nil`
---(reserved for a future async result — do not rely on it receiving a status).
---@param flag boolean
---@param callback? fun(err: string|nil)
function picocalc.network.setEnabled(flag, callback) end

---Return `true` if the WiFi hardware has been disconnected/disabled
---(e.g. by video playback boosting the clock).
---@return boolean
function picocalc.network.isHwDisconnected() end

---@class picocalc.network.http
picocalc.network.http = {}

---@class PicoDeckHttpConn : userdata
local PicoDeckHttpConn = {}

---Create a new HTTP(S) connection object. Does not connect until a request is made.
---@param server string Hostname or IP (no scheme prefix)
---@param port? integer Default: 80 for HTTP, 443 for HTTPS
---@param use_ssl? boolean `true` for HTTPS
---@return PicoDeckHttpConn? conn
---@return string? error
function picocalc.network.http.new(server, port, use_ssl) end

---HTTPS verifies the server certificate against the OS root bundle and the
---host name, and fails with an error starting "clock not set" until SNTP has
---set the clock after WiFi connects (retry a few seconds later). A
---certificate that does not verify fails with "TLS: <reason>".
---`setInsecure(true)` (before get/post) turns both off for THIS connection —
---only for self-signed development servers.
---@param flag boolean default false
function PicoDeckHttpConn:setInsecure(flag) end

---Enable or disable HTTP keep-alive for this connection.
---@param flag boolean
function PicoDeckHttpConn:setKeepAlive(flag) end

---Request a specific byte range (for resumable downloads).
---@param from integer Start byte offset (inclusive)
---@param to integer End byte offset (inclusive)
function PicoDeckHttpConn:setByteRange(from, to) end

---Set the connection timeout in seconds (default: 10).
---@param seconds number
function PicoDeckHttpConn:setConnectTimeout(seconds) end

---Set the read timeout in seconds (default: 30).
---@param seconds number
function PicoDeckHttpConn:setReadTimeout(seconds) end

---Set the internal read buffer size in bytes (default 4096, max 2097152 / 2 MiB).
---@param bytes integer
---@return boolean ok
function PicoDeckHttpConn:setReadBufferSize(bytes) end

---Send an HTTP GET request.
---@param path string URL path, e.g. `"/api/data"`
---@param headers? string Extra request headers (raw HTTP format)
---@return boolean ok
---@return string? error
function PicoDeckHttpConn:get(path, headers) end

---Send an HTTP POST request.
---@param path string
---@param headers? string Extra headers
---@param body? string Request body
---@return boolean ok
---@return string? error
function PicoDeckHttpConn:post(path, headers, body) end

-- There is no `query` method (older docs called it an alias for `post`; it
-- was never registered). Use `post`.

---Close the connection.
function PicoDeckHttpConn:close() end

---Return the last error string, or `nil` if no error.
---@return string?
function PicoDeckHttpConn:getError() end

---Return download progress. `total` is -1 if Content-Length is unknown.
---@return integer bytes_received
---@return integer total
function PicoDeckHttpConn:getProgress() end

---Return the number of bytes available to read.
---@return integer
function PicoDeckHttpConn:getBytesAvailable() end

---Read up to `length` bytes from the response body (max 131072 per call).
---Returns `nil` when done.
---@param length? integer Max bytes to read
---@return string?
function PicoDeckHttpConn:read(length) end

---Return the HTTP response status code (e.g. 200), or `nil` if not yet received.
---@return integer?
function PicoDeckHttpConn:getResponseStatus() end

---Return all response headers as a key→value table, or `nil` if not yet received.
---@return { [string]: string }?
function PicoDeckHttpConn:getResponseHeaders() end

---Register a callback fired each time new response data arrives.
---@param fn fun()
function PicoDeckHttpConn:setRequestCallback(fn) end

---Register a callback fired once response headers have been parsed.
---@param fn fun()
function PicoDeckHttpConn:setHeadersReadCallback(fn) end

---Register a callback fired when the full response body has been received.
---@param fn fun()
function PicoDeckHttpConn:setRequestCompleteCallback(fn) end

---Register a callback fired when the request fails (not after a successful response). Called with no arguments; read getError() inside it, since the connection is released afterwards.
---@param fn fun()
function PicoDeckHttpConn:setConnectionClosedCallback(fn) end

-- =============================================================================
-- picocalc.tcp  (raw TCP/TLS client)
-- =============================================================================

---@class picocalc.tcp
picocalc.tcp = {}

---@class PicoDeckTcpConn : userdata
local PicoDeckTcpConn = {}

---Create a TCP (or TLS) connection object (nothing is sent until `connect`).
---Returns `nil, err` when the 4-socket pool is full.  With `use_ssl` the server
---certificate is verified (OS root bundle + host name) and the socket counts
---as connected only after the TLS handshake; a TLS connect before SNTP has set
---the clock fails with "clock not set".
---@param host string Hostname or IP
---@param port? integer Default: 80
---@param use_ssl? boolean `true` for TLS
---@return PicoDeckTcpConn? conn
---@return string? err
function picocalc.tcp.new(host, port, use_ssl) end

---Start connecting (non-blocking). Returns `true`, or `false, err` (WiFi not
---available, already connecting/connected). Completion: `isConnected()`,
---`waitConnected()`, the connect callback or `CB_CONNECT` in `getEvents()`.
---@return boolean ok
---@return string? err
function PicoDeckTcpConn:connect() end

---Before `connect()`: TLS without certificate verification or the clock
---check (self-signed development servers only). Default false.
---@param flag boolean
function PicoDeckTcpConn:setInsecure(flag) end

---Write data to the connection. Returns bytes written, or -1 on error.
---@param data string
---@return integer
function PicoDeckTcpConn:write(data) end

---Read up to `max_len` bytes. Returns `nil` if no data is available.
---@param max_len? integer
---@return string?
function PicoDeckTcpConn:read(max_len) end

---Close the connection. The object is unusable afterwards (I/O raises).
function PicoDeckTcpConn:close() end

---Return the number of bytes available to read.
---@return integer
function PicoDeckTcpConn:available() end

---Return the last error string, or `nil`. (The method is `error`, not
---`getError` as older docs said.)
---@return string?
function PicoDeckTcpConn:error() end

---Return `true` if the connection is currently established.
---@return boolean
function PicoDeckTcpConn:isConnected() end

---@param seconds number
function PicoDeckTcpConn:setConnectTimeout(seconds) end

---Read timeout; off by default, 0 disables it. Applies to the connection.
---@param seconds number
function PicoDeckTcpConn:setReadTimeout(seconds) end

---Register a callback fired when the connection is established.
---@param fn fun(conn: PicoDeckTcpConn)
function PicoDeckTcpConn:setConnectCallback(fn) end

---Register a callback fired when data arrives.
---@param fn fun(conn: PicoDeckTcpConn)
function PicoDeckTcpConn:setReadCallback(fn) end

---Register a callback fired when the connection closes.
---@param fn fun(conn: PicoDeckTcpConn)
function PicoDeckTcpConn:setCloseCallback(fn) end

---Return and clear the pending events that have no callback registered, as a
---bitmask of `picocalc.tcp.CB_CONNECT` (1), `CB_READ` (2), `CB_WRITE` (4),
---`CB_CLOSED` (8), `CB_FAILED` (16). Callbacks receive the socket and never
---nest; buffered data stays readable after the peer closes.
---@return integer
function PicoDeckTcpConn:getEvents() end

---Block until connected, with an optional timeout. Returns `true` if connected.
---@param timeout_seconds? number
---@return boolean
function PicoDeckTcpConn:waitConnected(timeout_seconds) end

---Block until data is available, with an optional timeout. Returns `true` if data arrived.
---@param timeout_seconds? number
---@return boolean
function PicoDeckTcpConn:waitData(timeout_seconds) end

-- =============================================================================
-- picocalc.ui  (modal dialogs and HUD widgets)
-- =============================================================================

---@class picocalc.ui
picocalc.ui = {}

---Draw a status bar at the top of the screen with a title and battery/WiFi/clock indicators.
---@param title string
---@return integer rows the header covers (21); content starts at this y
function picocalc.ui.drawHeader(title) end

---Draw a status bar at the bottom of the screen.
---@param left? string Left-aligned text
---@param right? string Right-aligned text
function picocalc.ui.drawFooter(left, right) end

---Draw a tab bar. Returns the new active tab index and the tab-bar height.
---@param y integer Top y coordinate
---@param tabs string[] Tab label strings
---@param active_index integer Currently active tab (1-based)
---@param prev_key? integer Button constant to switch to previous tab
---@param next_key? integer Button constant to switch to next tab
---@return integer active_index
---@return integer height
function picocalc.ui.drawTabs(y, tabs, active_index, prev_key, next_key) end

---Show a blocking modal text-input dialog. Returns the entered string, or `nil` if cancelled.
---@param prompt? string
---@param default? string
---@return string?
function picocalc.ui.textInput(prompt, default) end

---Show a blocking yes/no confirmation dialog. Returns `true` if the user confirms.
---@param message string
---@return boolean
function picocalc.ui.confirm(message) end

---Draw a spinning progress indicator.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r? integer Radius
---@param frame? integer Animation frame index (auto-advances if omitted)
function picocalc.ui.drawSpinner(cx, cy, r, frame) end

-- =============================================================================
-- picocalc.perf  (performance profiling)
-- =============================================================================

---@class picocalc.perf
picocalc.perf = {}

---Mark the start of a frame for FPS measurement.
function picocalc.perf.beginFrame() end

---Mark the end of a frame. Call after `display.flush()`.
function picocalc.perf.endFrame() end

---Return the rolling-average FPS.
---@return integer
function picocalc.perf.getFPS() end

---Return the last frame time in milliseconds.
---@return integer
function picocalc.perf.getFrameTime() end

---Draw a colour-coded FPS counter at (x, y).
---@param x? integer Default: right-aligned 8px from the right edge
---@param y? integer Default: 24, just below the standard header
function picocalc.perf.drawFPS(x, y) end

---Set a target FPS cap (0 = uncapped).
---@param fps integer
function picocalc.perf.setTargetFPS(fps) end

-- =============================================================================
-- picocalc.graphics  (images, sprites, spritesheets, animations, fonts)
-- =============================================================================

---@class picocalc.graphics
picocalc.graphics = {}

---Set the default drawing colour used by `clear()` and filled graphic operations.
---@param color integer RGB565
function picocalc.graphics.setColor(color) end

---Set the background (erase) colour.
---@param color integer RGB565
function picocalc.graphics.setBackgroundColor(color) end

---Set a global transparent colour for image blitting (`nil` to disable).
---@param color integer|nil RGB565
function picocalc.graphics.setTransparentColor(color) end

---Return the current global transparent colour, or `nil`.
---@return integer?
function picocalc.graphics.getTransparentColor() end

---Clear the display using the current background colour.
---@param color? integer RGB565 override
function picocalc.graphics.clear(color) end

---Draw a grid of `cols`×`rows` cells in one call (replaces many drawRect calls).
---@param x integer
---@param y integer
---@param cell_w integer Cell width
---@param cell_h integer Cell height
---@param cols integer
---@param rows integer
---@param color integer RGB565
function picocalc.graphics.drawGrid(x, y, cell_w, cell_h, cols, rows, color) end

---Fill a rect and outline it in one call (dialogue boxes, panels).
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param fill integer RGB565 fill colour
---@param border integer RGB565 outline colour
function picocalc.graphics.fillBorderedRect(x, y, w, h, fill, border) end

---Draw a 2D block-grid playfield (Tetris/Puzzle-style): grid lines plus a
---filled block for every non-zero cell. `playfield[row][col]` = colour (0 = empty).
---@param playfield integer[][] 2D array [row][col] of RGB565 colours
---@param ox integer Origin x
---@param oy integer Origin y
---@param block_size integer Pixels per cell
---@param cols integer
---@param rows integer
---@param grid_color integer RGB565 grid line colour
function picocalc.graphics.drawPlayfield(playfield, ox, oy, block_size, cols, rows, grid_color) end

---Update, draw, and compact a particle system in one C call.
---`particles` is a flat sequence of 6 numbers per particle:
---`{ x, y, vx, vy, life_ms, color, ... }`. Each live particle moves by
---`vx*dt, vy*dt`, loses `dt*1000` ms of life, and is drawn as one pixel.
---Dead particles are removed in place.
---@param particles number[] Flat particle array (modified in place)
---@param dt number Delta time in seconds
---@return integer live_count Particles still alive
function picocalc.graphics.updateDrawParticles(particles, dt) end

---Set a global stencil pattern (8 bytes, checkerboard phase).
---@param pattern integer[] 8 bytes
function picocalc.graphics.setStencilPattern(pattern) end

---Draw text using the default font. Returns pixel width.
---@param text string
---@param x integer
---@param y integer
---@param font? PicoDeckFont
---@return integer width
function picocalc.graphics.drawText(text, x, y, font) end

---Draw text with horizontal alignment. alignment: 0=left, 1=centre, 2=right.
---@param text string
---@param x integer
---@param y integer
---@param alignment integer 0|1|2
---@param font? PicoDeckFont
function picocalc.graphics.drawTextAligned(text, x, y, alignment, font) end

---Word-wrap text within a bounding rect (dialogue boxes).
---@param text string
---@param rx integer Rect x
---@param ry integer Rect y
---@param rw integer Rect width
---@param rh integer Rect height
---@param alignment? integer 0=left (default), 1=centre, 2=right
---@param font? PicoDeckFont
function picocalc.graphics.drawTextInRect(text, rx, ry, rw, rh, alignment, font) end

---Measure a string in `font` (default: the active font).
---@param text string
---@param font? PicoDeckFont
---@return integer width
---@return integer height
function picocalc.graphics.getTextSize(text, font) end

---Word-wrap a string to `max_width` and measure the result.
---@param text string
---@param max_width integer
---@param font? PicoDeckFont
---@return integer width
---@return integer height
function picocalc.graphics.getTextSizeForMaxWidth(text, max_width, font) end

---Render text into a new image (word-wrapped to max_w × max_h).
---@param text string
---@param max_w integer
---@param max_h integer
---@param bg? integer RGB565 background (default: the graphics background colour)
---@param font? PicoDeckFont
---@return PicoDeckImage? img
---@return string? error
function picocalc.graphics.imageWithText(text, max_w, max_h, bg, font) end

-- ── Image ────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.image
picocalc.graphics.image = {}

---@class PicoDeckImage : userdata
local PicoDeckImage = {}

---Load an image from the SD card (BMP, JPEG, PNG, GIF). Raises on failure
---(access denied, missing or undecodable file); it never returns nil.
---@param path string
---@return PicoDeckImage img
function picocalc.graphics.image.load(path) end

---Load a sub-region of an image from the SD card.
---@param path string
---@param x integer Source x offset
---@param y integer Source y offset
---@param w integer Region width
---@param h integer Region height
---@return PicoDeckImage img Raises on failure
function picocalc.graphics.image.loadRegion(path, x, y, w, h) end

---Load and scale an image from the SD card.
---@param path string
---@param w integer Target width
---@param h integer Target height
---@return PicoDeckImage img Raises on failure
function picocalc.graphics.image.loadScaled(path, w, h) end

---Load an image from a Lua string (in-memory buffer). Format is auto-detected
---from magic bytes (BMP, JPEG, PNG, GIF).
---@param data string|userdata Raw encoded image bytes, or a qmibuf
---@param length? integer With a qmibuf: how many bytes of it to decode (default: all)
---@return PicoDeckImage img Raises on an unsupported or corrupt buffer
function picocalc.graphics.image.loadFromBuffer(data, length) end

---Create a blank (black) image of the given dimensions.
---@param width integer
---@param height integer
---@return PicoDeckImage
function picocalc.graphics.image.new(width, height) end

---Return metadata for an image file without decoding pixels (header only).
---@param path string
---@return { width: integer, height: integer, format: string } info Raises on an unreadable file
function picocalc.graphics.image.getInfo(path) end

---Return a list of supported image format strings (e.g. `{"BMP", "JPEG", ...}`).
---@return string[]
function picocalc.graphics.image.getSupportedFormats() end

---Start an asynchronous decode of an image on Core 1. Poll with `pollPreload`.
---Only one preload can be in flight at a time.
---@param path string
---@return boolean started
function picocalc.graphics.image.preload(path) end

---Poll an in-flight preload. Returns `image, ready` — `image` is non-nil when
---the decode finished (check `ready` to distinguish "still working" from done).
---@return PicoDeckImage? image
---@return boolean ready
function picocalc.graphics.image.pollPreload() end

---Cancel an in-flight preload.
function picocalc.graphics.image.cancelPreload() end

-- PicoDeckImage methods

---Return the dimensions of this image.
---@return integer width
---@return integer height
function PicoDeckImage:getSize() end

---Return a deep copy of this image.
---@return PicoDeckImage
function PicoDeckImage:copy() end

---Draw the image at (x, y). Optional options table can include `flipX` and `flipY`.
---Optional `rect` clips the source region `{x, y, w, h}`.
---@param x integer
---@param y integer
---@param options? { flipX?: boolean, flipY?: boolean }
---@param rect? { x: integer, y: integer, w: integer, h: integer }
function PicoDeckImage:draw(x, y, options, rect) end

---Draw the image with an anchor point. `ax`, `ay` in [0,1] — (0,0) = top-left, (0.5,0.5) = centre.
---@param x integer
---@param y integer
---@param ax number
---@param ay number
function PicoDeckImage:drawAnchored(x, y, ax, ay) end

---Tile-fill a rectangle of size `rect_w` × `rect_h` starting at (x, y).
---@param x integer
---@param y integer
---@param rect_w integer
---@param rect_h integer
function PicoDeckImage:drawTiled(x, y, rect_w, rect_h) end

---Draw the image at (x, y), multiplied by `scale` (bilinear) and optionally rotated.
---@param x integer
---@param y integer
---@param scale number Multiplier (a float; 2.0 = double size)
---@param angle? number Rotation in radians (default 0)
function PicoDeckImage:drawScaled(x, y, scale, angle) end

---Draw the image at (x, y), enlarged by a whole-number `scale` (nearest-neighbour, fast).
---@param x integer
---@param y integer
---@param scale integer Positive integer multiplier (a non-positive value raises an error)
function PicoDeckImage:drawScaledNN(x, y, scale) end

---Stretch the image (or srcRect of it) to exactly w x h, nearest-neighbour.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param srcRect? {x?: integer, y?: integer, w?: integer, h?: integer}
function PicoDeckImage:drawStretched(x, y, w, h, srcRect) end

---Set a transparent colour for this image (overrides global setting).
---@param color integer|nil RGB565, or `nil` to clear
function PicoDeckImage:setTransparentColor(color) end

---Return this image's transparent colour, or `nil`.
---@return integer?
function PicoDeckImage:getTransparentColor() end

---Return metadata for this image.
---@return { width: integer, height: integer, transparentColor?: integer, storage: string }
function PicoDeckImage:getMetadata() end

-- ── Sprite ────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.sprite
picocalc.graphics.sprite = {}

---@class PicoDeckSprite : userdata
local PicoDeckSprite = {}

---Create a new Sprite object, optionally with an image.
---@param image? PicoDeckImage
---@return PicoDeckSprite
function picocalc.graphics.sprite.new(image) end

---Add a sprite to the global sprite list.
---@param sprite PicoDeckSprite
function picocalc.graphics.sprite.addSprite(sprite) end

---Remove a sprite from the global sprite list.
---@param sprite PicoDeckSprite
function picocalc.graphics.sprite.removeSprite(sprite) end

---Update all sprites (calls each sprite's update callback).
function picocalc.graphics.sprite.update() end

---Return all sprites in the global list.
---@return PicoDeckSprite[]
function picocalc.graphics.sprite.getAllSprites() end

---Return the number of sprites in the global list.
---@return integer
function picocalc.graphics.sprite.spriteCount() end

---Remove all sprites from the global list.
function picocalc.graphics.sprite.removeAll() end

---Remove a list of sprites from the global list.
---@param sprites PicoDeckSprite[]
function picocalc.graphics.sprite.removeSprites(sprites) end

---Call `fn(sprite)` for every sprite in the global list.
---@param fn fun(sprite: PicoDeckSprite)
function picocalc.graphics.sprite.performOnAllSprites(fn) end

---Return sprites whose bounds overlap a point.
---@param x integer
---@param y integer
---@return PicoDeckSprite[]
function picocalc.graphics.sprite.querySpritesAtPoint(x, y) end

---Return sprites whose bounds overlap a rectangle.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@return PicoDeckSprite[]
function picocalc.graphics.sprite.querySpritesInRect(x, y, w, h) end

---Return sprites whose bounds intersect a line segment.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@return PicoDeckSprite[]
function picocalc.graphics.sprite.querySpritesAlongLine(x1, y1, x2, y2) end

---Return collision info for sprites along a line segment.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@return table[]
function picocalc.graphics.sprite.querySpriteInfoAlongLine(x1, y1, x2, y2) end

---Set clip rects for sprites in a z-index range (stored, not applied).
---@param clip_rect { x: integer, y: integer, w: integer, h: integer }
---@param z_start integer
---@param z_end integer
---@overload fun(x: integer, y: integer, w: integer, h: integer, z_start: integer, z_end: integer)
function picocalc.graphics.sprite.setClipRectsInRange(clip_rect, z_start, z_end) end

---Clear clip rects for sprites in a z-index range.
---@param z_start integer
---@param z_end integer
function picocalc.graphics.sprite.clearClipRectsInRange(z_start, z_end) end

---Add an invisible collision sprite at a rect (useful for tilemaps).
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@return PicoDeckSprite
function picocalc.graphics.sprite.addEmptyCollisionSprite(x, y, w, h) end

-- PicoDeckSprite methods

---@param image PicoDeckImage|nil `nil` clears the image
---@param flip? boolean Horizontal flip
---@param scale? number Only applied when given
---@param scale_y? number Only applied when given (unlike `setScale`, it does not default to `scale`)
function PicoDeckSprite:setImage(image, flip, scale, scale_y) end

---@return PicoDeckImage?
function PicoDeckSprite:getImage() end

---Add this sprite to the global sprite list.
function PicoDeckSprite:add() end

---Remove this sprite from the global sprite list.
function PicoDeckSprite:remove() end

---@param x integer
---@param y integer
function PicoDeckSprite:moveTo(x, y) end

---@param dx integer
---@param dy integer
function PicoDeckSprite:moveBy(dx, dy) end

---@return integer x
---@return integer y
function PicoDeckSprite:getPosition() end

---@param z integer
function PicoDeckSprite:setZIndex(z) end

---@return integer
function PicoDeckSprite:getZIndex() end

---@param visible boolean
function PicoDeckSprite:setVisible(visible) end

---@return boolean
function PicoDeckSprite:isVisible() end

---Store a centre offset in whole pixels (stored only: drawing does not use it).
---@param cx integer Rounded to an integer (0.5 reads back as 1)
---@param cy integer
function PicoDeckSprite:setCenter(cx, cy) end

---@return integer cx
---@return integer cy
function PicoDeckSprite:getCenter() end

---@return integer[] point `{cx, cy}`: one table, not two values
function PicoDeckSprite:getCenterPoint() end

---@param w integer
---@param h integer
function PicoDeckSprite:setSize(w, h) end

---@return integer w
---@return integer h
function PicoDeckSprite:getSize() end

---@param scale number
---@param scale_y? number Defaults to `scale`
function PicoDeckSprite:setScale(scale, scale_y) end

---@return number scale
---@return number scale_y
function PicoDeckSprite:getScale() end

---Enable nearest-neighbour scaling.
---@param nn boolean
function PicoDeckSprite:setScaleNN(nn) end

---@param color integer|nil RGB565
function PicoDeckSprite:setTransparentColor(color) end

---Set the rotation. The angle is in radians.
---@param radians number
---@param scale? number Also sets the scale
---@param scale_y? number Also sets the vertical scale
function PicoDeckSprite:setRotation(radians, scale, scale_y) end

---@return number
function PicoDeckSprite:getRotation() end

---@return PicoDeckSprite
function PicoDeckSprite:copy() end

---Restrict image blitting to a sub-rect of the source image.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicoDeckSprite:setSourceRect(x, y, w, h) end

---Remove the source-rect restriction.
function PicoDeckSprite:clearSourceRect() end

---@param enabled boolean
function PicoDeckSprite:setUpdatesEnabled(enabled) end

---@return boolean
function PicoDeckSprite:updatesEnabled() end

---@param tag integer
function PicoDeckSprite:setTag(tag) end

---@return integer
function PicoDeckSprite:getTag() end

---Accepted and ignored (no-op): sprites draw opaque/keyed only.
---@param mode integer
function PicoDeckSprite:setImageDrawMode(mode) end

---Flip the image horizontally. There is no vertical flip.
---@param flip boolean
function PicoDeckSprite:setImageFlip(flip) end

---@return boolean flip Horizontal flip
function PicoDeckSprite:getImageFlip() end

---Stored but not applied yet (no-op).
---@param ignore boolean
function PicoDeckSprite:setIgnoresDrawOffset(ignore) end

---Set the sprite bounding box.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicoDeckSprite:setBounds(x, y, w, h) end

---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicoDeckSprite:getBounds() end

---@return { x: integer, y: integer, w: integer, h: integer }
function PicoDeckSprite:getBoundsRect() end

---@param opaque boolean
function PicoDeckSprite:setOpaque(opaque) end

---@return boolean
function PicoDeckSprite:isOpaque() end

---@param fn fun(sprite: PicoDeckSprite, x: integer, y: integer, w: integer, h: integer)
function PicoDeckSprite:setBackgroundDrawingCallback(fn) end

---Draw this sprite immediately (outside the normal update cycle).
---@param x? integer Defaults to the sprite's x
---@param y? integer Defaults to the sprite's y
function PicoDeckSprite:draw(x, y) end

---Update this sprite (calls its registered update callback).
function PicoDeckSprite:update() end

---@param enabled boolean
function PicoDeckSprite:setCollisionsEnabled(enabled) end

---@return boolean
function PicoDeckSprite:collisionsEnabled() end

---Set the collision rectangle (relative to the sprite's bounds).
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicoDeckSprite:setCollideRect(x, y, w, h) end

---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicoDeckSprite:getCollideRect() end

---Return the collision rectangle in world coordinates.
---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicoDeckSprite:getCollideBounds() end

---Clear the collision rectangle (no collision).
function PicoDeckSprite:clearCollideRect() end

---Return sprites that currently overlap this sprite's collision rect.
---@return PicoDeckSprite[]
function PicoDeckSprite:overlappingSprites() end

---Return all sprites that overlap this sprite's collision rect (including off-screen).
---@return PicoDeckSprite[]
function PicoDeckSprite:allOverlappingSprites() end

---Clear the stencil mask.
function PicoDeckSprite:clearStencil() end

---Set an ordered-dither stencil pattern of the given density (stored only: drawing does not apply it).
---@param level number 0 (nothing drawn) to 1 (everything drawn); other values clamp
function PicoDeckSprite:setStencilPattern(level) end

---Return `true` if this sprite's image collides with `other` based on alpha masks.
---@param other PicoDeckSprite
---@return boolean
function PicoDeckSprite:alphaCollision(other) end

---Move toward (goalX, goalY), sliding along any collision rects in the way.
---Returns the actual position reached plus a list of collisions; each
---collision is `{sprite, other, type, x, y, normal = {x, y}, touch}`.
---@param goalX integer
---@param goalY integer
---@return integer actualX
---@return integer actualY
---@return table[] collisions
function PicoDeckSprite:moveWithCollisions(goalX, goalY) end

---Return this sprite's collision response mode (default `"slide"`).
---@return string
function PicoDeckSprite:collisionResponse() end

---Set a mask image used as this sprite's stencil (arg 3 reserved).
---@param image PicoDeckImage
function PicoDeckSprite:setStencilImage(image) end

-- ── Tilemap ─────────────────────────────────────────────────────────────────

---@class picocalc.graphics.tilemap
picocalc.graphics.tilemap = {}

---@class PicoDeckTilemap : userdata
local PicoDeckTilemap = {}

---Create a tilemap from a tileset image cut into `tile_w`×`tile_h` tiles.
---Tile indices are 1-based, row-major across the tileset; 0 = empty tile.
---@param image PicoDeckImage Tileset image
---@param tile_w integer Tile width in pixels
---@param tile_h integer Tile height in pixels
---@return PicoDeckTilemap
function picocalc.graphics.tilemap.new(image, tile_w, tile_h) end

---Allocate the tile grid (in tiles). All cells start as 0 (empty).
---@param w integer Map width in tiles
---@param h integer Map height in tiles
function PicoDeckTilemap:setSize(w, h) end

---Set the tile index at a map position (1-based into the tileset; 0 = empty).
---@param x integer Tile column
---@param y integer Tile row
---@param tile integer Tile index
function PicoDeckTilemap:setTileAtPosition(x, y, tile) end

---Return the tile index at a map position (0 = empty or out of bounds).
---@param x integer Tile column
---@param y integer Tile row
---@return integer tile
function PicoDeckTilemap:getTileAtPosition(x, y) end

---Return the map size in tiles.
---@return integer w
---@return integer h
function PicoDeckTilemap:getSize() end

---Return the tile size in pixels.
---@return integer tile_w
---@return integer tile_h
function PicoDeckTilemap:getTileSize() end

---Return the map size in pixels.
---@return integer width
---@return integer height
function PicoDeckTilemap:getPixelSize() end

---Draw the visible portion of the map at the given pixel scroll offset.
---@param scroll_x? integer
---@param scroll_y? integer
function PicoDeckTilemap:draw(scroll_x, scroll_y) end

-- ── Spritesheet ───────────────────────────────────────────────────────────────

---@class picocalc.graphics.spritesheet
picocalc.graphics.spritesheet = {}

---@class PicoDeckSpritesheet : userdata
local PicoDeckSpritesheet = {}

---Create a spritesheet, optionally with its source image; add frames with `addFrame`.
---@param image? PicoDeckImage
---@return PicoDeckSpritesheet
function picocalc.graphics.spritesheet.new(image) end

---Create a spritesheet from a uniform grid of equal-sized frames.
---@param image PicoDeckImage Source image
---@param cols integer Frames per row
---@param rows integer Rows
---@param frame_w integer Frame width in pixels
---@param frame_h integer Frame height in pixels
---@return PicoDeckSpritesheet
function picocalc.graphics.spritesheet.newGrid(image, cols, rows, frame_w, frame_h) end

---Add a frame (a rectangle of the source image) to the spritesheet.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@return integer index Zero-based index of the new frame
function PicoDeckSpritesheet:addFrame(x, y, w, h) end

---Return the number of frames.
---@return integer
function PicoDeckSpritesheet:getFrameCount() end

---Return the bounds of frame `i` (zero-based); nothing when `i` is out of range.
---@param i integer
---@return integer[]? bounds `{x, y, w, h}` in the source image
function PicoDeckSpritesheet:getFrame(i) end

---Return the combined source image.
---@return PicoDeckImage?
function PicoDeckSpritesheet:getImage() end

---Draw frame `i` at (x, y).
---@param i integer Zero-based frame index
---@param x integer
---@param y integer
---@param flip? boolean Horizontal flip
function PicoDeckSpritesheet:drawFrame(i, x, y, flip) end

-- ── AnimationLoop ─────────────────────────────────────────────────────────────

---@class picocalc.graphics.animation
picocalc.graphics.animation = {}

---@class picocalc.graphics.animation.loop
picocalc.graphics.animation.loop = {}

---@class PicoDeckAnimationLoop : userdata
local PicoDeckAnimationLoop = {}

---Create an animation loop over a list of images (whole images, not a spritesheet).
---@param interval_ms? integer Milliseconds per frame (default 100)
---@param frames? PicoDeckImage[] The frame images, in order
---@param looping? boolean Repeat forever (default true)
---@return PicoDeckAnimationLoop
function picocalc.graphics.animation.loop.new(interval_ms, frames, looping) end

---Draw the current frame at (x, y).
---@param x integer
---@param y integer
---@param flip? boolean Horizontal flip
function PicoDeckAnimationLoop:draw(x, y, flip) end

---Advance the animation timer.
function PicoDeckAnimationLoop:update() end

---Return the current frame image.
---@return PicoDeckImage?
function PicoDeckAnimationLoop:image() end

---Return `true` if the animation still has frames (always `true` for loops).
---@return boolean
function PicoDeckAnimationLoop:isValid() end

---Return the current zero-based frame index.
---@return integer
function PicoDeckAnimationLoop:getFrameIndex() end

---Replace the frames and restart at frame 0.
---@param frames? PicoDeckImage[] A list of images; anything else clears the loop
function PicoDeckAnimationLoop:setImageTable(frames) end

---Set the milliseconds per frame.
---@param ms integer
function PicoDeckAnimationLoop:setInterval(ms) end

---Enable or disable looping.
---@param loop boolean
function PicoDeckAnimationLoop:setLooping(loop) end

---Reset to frame 0.
function PicoDeckAnimationLoop:reset() end

-- ── Blinker ───────────────────────────────────────────────────────────────────

---@class picocalc.graphics.animation.blinker
picocalc.graphics.animation.blinker = {}

---@class PicoDeckBlinker : userdata
local PicoDeckBlinker = {}

---Create a blinker (on/off flash timer). Every argument is optional.
---@param on_ms? integer Milliseconds on (default 500)
---@param off_ms? integer Milliseconds off (default 500)
---@param loop? boolean Repeat forever (default true)
---@param cycles? integer With `loop == false`: stop after this many cycles (0 = never)
---@param invert? boolean Start in the off state (currently overridden: `start`/`update` begin "on")
---@return PicoDeckBlinker
function picocalc.graphics.animation.blinker.new(on_ms, off_ms, loop, cycles, invert) end

---Update all blinkers.
function picocalc.graphics.animation.blinker.updateAll() end

---Stop all blinkers.
function picocalc.graphics.animation.blinker.stopAll() end

---Start (or restart) the blinker. Same optional arguments as `blinker.new`;
---any given replace the stored ones.
---@param on_ms? integer Milliseconds on
---@param off_ms? integer Milliseconds off
---@param loop? boolean Repeat forever
---@param cycles? integer With `loop == false`: stop after this many cycles
---@param invert? boolean (currently has no effect: start always begins "on")
function PicoDeckBlinker:start(on_ms, off_ms, loop, cycles, invert) end

---Start looping indefinitely with the stored durations (takes no arguments).
function PicoDeckBlinker:startLoop() end

---Stop the blinker.
function PicoDeckBlinker:stop() end

---Remove from the global blinker list.
function PicoDeckBlinker:remove() end

---@return boolean
function PicoDeckBlinker:isRunning() end

---Advance the blinker timer by the elapsed time.
---@return boolean on Whether the blinker is currently on
function PicoDeckBlinker:update() end

-- ── Animator ──────────────────────────────────────────────────────────────────

---@class picocalc.graphics.animation.animator
picocalc.graphics.animation.animator = {}

---@class PicoDeckAnimator : userdata
---@field easingAmplitude number Stored and readable, but no easing curve uses it yet (default 1)
---@field easingPeriod number Stored and readable, but no easing curve uses it yet (default 0)
---@field repeatCount integer Times the animation runs (default 1)
---@field reverses boolean Play back to `from` after reaching `to` (default false)
local PicoDeckAnimator = {}

---Create an Animator that interpolates a value from `from` to `to` over `duration_ms`.
---It starts at once, or after `delay_ms`.
---@param duration_ms integer Duration comes first
---@param from number
---@param to number
---@param easing? string Easing name: "linear" (default), "sineIn", "sineOut", "sineInOut", "quadIn", "quadOut", "quadInOut", "cubicIn", "cubicOut", "cubicInOut" (an unknown name is linear; a function is not accepted)
---@param delay_ms? integer Wait this long before starting
---@return PicoDeckAnimator
function picocalc.graphics.animation.animator.new(duration_ms, from, to, easing, delay_ms) end

---Return the current interpolated value.
---@return number
function PicoDeckAnimator:currentValue() end

---Return the interpolated value at a given elapsed time in milliseconds.
---@param ms integer
---@return number
function PicoDeckAnimator:valueAtTime(ms) end

---Return completion progress in [0, 1].
---@return number
function PicoDeckAnimator:progress() end

---Reset the animation to the start.
---@param duration_ms? integer New duration
function PicoDeckAnimator:reset(duration_ms) end

---Return `true` when the animation has finished.
---@return boolean
function PicoDeckAnimator:ended() end

-- ── Font ──────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.font
picocalc.graphics.font = {}

---@class PicoDeckFont : userdata
local PicoDeckFont = {}

---Create a font, either one of the built-in names or a `.pfn` path.
---A path is sandbox-checked and loaded; the returned object frees its
---loaded slot when garbage-collected. Every font an app loads is also
---freed automatically when the app exits.
---@param name_or_path string One of "6x8", "8x12", "scientifica", "scientifica-bold", or a `.pfn` path
---@return PicoDeckFont font Errors (never returns nil) on access denied or load failure
function picocalc.graphics.font.new(name_or_path) end

---Draw text at (x, y) using this font. bg defaults to BLACK if omitted.
---@param x integer
---@param y integer
---@param text string
---@param fg integer RGB565 foreground colour
---@param bg? integer RGB565 background colour
---@return integer width Pixel width of the drawn text
function PicoDeckFont:drawText(x, y, text, fg, bg) end

---Draw text with horizontal alignment.
---@param x integer
---@param y integer
---@param text string
---@param alignment integer 0=left, 1=centre, 2=right
---@param fg integer RGB565
---@param bg? integer RGB565
function PicoDeckFont:drawTextAligned(x, y, text, alignment, fg, bg) end

---Word-wrap text within a bounding rect. Wrapping breaks at spaces and uses
---each glyph's real advance, so it works for both monospace and
---proportional fonts.
---@param x integer Rect x
---@param y integer Rect y
---@param w integer Rect width
---@param h integer Rect height
---@param text string
---@param alignment? integer 0=left (default), 1=centre, 2=right
---@param fg? integer RGB565
---@param bg? integer RGB565
function PicoDeckFont:drawTextInRect(x, y, w, h, text, alignment, fg, bg) end

---Return the glyph height of this font.
---@return integer
function PicoDeckFont:getHeight() end

---Return the maximum glyph advance of this font in pixels. For a
---proportional font this is the widest glyph, not every glyph's width;
---use getTextWidth to measure a specific string.
---@return integer
function PicoDeckFont:getWidth() end

---Return the pixel width of a string in this font (real per-glyph advances).
---@param text string
---@return integer
function PicoDeckFont:getTextWidth(text) end

---Return the string this font was created with: a built-in name, or the
---`.pfn` path for a loaded font.
---@return string
function PicoDeckFont:getName() end

-- =============================================================================
-- picocalc.gfx3d
-- =============================================================================

---@class PicoDeckMesh : userdata
local PicoDeckMesh = {}
---@return integer nverts
---@return integer ntris
---@return number cx
---@return number cy
---@return number cz
---@return number radius
function PicoDeckMesh:getInfo() end

---@class picocalc.gfx3d
---@field DOUBLE_SIDED integer
---@field UNLIT integer
---@field NO_FOG integer
picocalc.gfx3d = {}
---@param verts number[] flat x, y, z list
---@param tris integer[] flat 1-based index list, 3 per triangle, CCW = front
---@param colors integer|integer[] one RGB565 colour, or one per triangle
---@param flags? integer|integer[] DOUBLE_SIDED | UNLIT | NO_FOG
---@return PicoDeckMesh
function picocalc.gfx3d.newMesh(verts, tris, colors, flags) end
function picocalc.gfx3d.setViewport(x, y, w, h) end
function picocalc.gfx3d.setProjection(fovY, near, far) end
function picocalc.gfx3d.setCamera(x, y, z, yaw, pitch, roll) end
---@param ux? number Up vector (default 0, 1, 0)
---@param uy? number
---@param uz? number
function picocalc.gfx3d.lookAt(ex, ey, ez, tx, ty, tz, ux, uy, uz) end
---@param ambient? number Default 0.25
function picocalc.gfx3d.setLight(dx, dy, dz, ambient) end
function picocalc.gfx3d.setFog(near, far, color) end
---@param bands {[1]: number, [2]: integer}[]|nil
function picocalc.gfx3d.setSky(bands) end
---@param clearColor? integer
function picocalc.gfx3d.beginScene(clearColor) end
---@param scale? number Default 1; must be positive
---@param bias? number Default 0
---@param sortAsOne? boolean
function picocalc.gfx3d.draw(mesh, x, y, z, yaw, pitch, roll, scale, bias, sortAsOne) end
---@param scale? number Default 1; must be positive
---@param bias? number Default 0
---@param sortAsOne? boolean
function picocalc.gfx3d.drawBasis(mesh, x, y, z, fx, fy, fz, ux, uy, uz, scale, bias, sortAsOne) end
---@param list table an array of meshes built in world coordinates
---@param bias number|nil depth bias for every mesh (default 0)
---@param start integer|nil the first index drawn; the rest wrap round (default 1)
function picocalc.gfx3d.drawList(list, bias, start) end
function picocalc.gfx3d.drawBackground(mesh) end
---@param sx? integer Source rect x (default 0)
---@param sy? integer Source rect y (default 0)
---@param sw? integer Source rect width (default: the rest of the image)
---@param sh? integer Source rect height (default: the rest of the image)
---@param bias? number Default 0
function picocalc.gfx3d.drawSprite(image, x, y, z, size, sx, sy, sw, sh, bias) end
function picocalc.gfx3d.endScene() end
---@return number|nil sx, number sy, number depth
function picocalc.gfx3d.project(x, y, z) end
---@param t table|nil a table to fill and return instead of a new one
---@return {tris_in: integer, culled: integer, clipped: integer, drawn: integer, sprites: integer, overflow: integer, us_geom: integer, us_raster: integer}
function picocalc.gfx3d.getStats(t) end

-- =============================================================================
-- picocalc.video  (MJPEG AVI playback)
-- =============================================================================

---@class picocalc.video
picocalc.video = {}

---@class PicoDeckVideoPlayer : userdata
local PicoDeckVideoPlayer = {}

---Create a new video player. Freed by the garbage collector when unreferenced.
---@return PicoDeckVideoPlayer
function picocalc.video.player() end

---Load an MJPEG AVI file. Returns `true` on success.
---@param path string
---@return boolean ok
function PicoDeckVideoPlayer:load(path) end

---Start playback. Raises clock to 300 MHz and disconnects WiFi for decode performance.
function PicoDeckVideoPlayer:play() end

---Pause playback.
function PicoDeckVideoPlayer:pause() end

---Resume after pause.
function PicoDeckVideoPlayer:resume() end

---Stop playback and restore system clock / reconnect WiFi.
function PicoDeckVideoPlayer:stop() end

---Advance one frame and decode it to the display. Call once per loop iteration.
---Returns `true` while still playing.
---@return boolean playing
function PicoDeckVideoPlayer:update() end

---Seek to a specific frame index. Clamps to the file and never wraps: seeking
---to/past the last frame ends the video on the next update (hold or loop).
---A seek while paused presents the target frame immediately; a seek after
---the end restarts playback from the target.
---@param frame integer
function PicoDeckVideoPlayer:seek(frame) end

---Seek to an absolute time in milliseconds (same clamping rules as `seek`).
---@param ms integer
function PicoDeckVideoPlayer:seekMs(ms) end

---Seek relative to the current position (negative = backwards). Clamps at both ends.
---@param delta_ms integer
function PicoDeckVideoPlayer:seekRelativeMs(delta_ms) end

---Total number of video frames.
---@return integer
function PicoDeckVideoPlayer:getFrameCount() end

---Total duration in milliseconds.
---@return integer
function PicoDeckVideoPlayer:getDurationMs() end

---Position of the frame currently on screen, in milliseconds.
---@return integer
function PicoDeckVideoPlayer:getPositionMs() end

---`true` once playback reached the last frame with looping off. The last frame
---stays on screen; `play()` or `resume()` replays from the start.
---@return boolean
function PicoDeckVideoPlayer:hasEnded() end

---Enable/disable the built-in progress OSD (bar + elapsed/total time drawn over
---the bottom of the video). On by default. It appears on play/pause/seek, hides
---after the timeout while playing, and stays while paused or ended.
---@param enabled boolean
function PicoDeckVideoPlayer:setOSD(enabled) end

---Show the OSD now and restart its hide timer.
function PicoDeckVideoPlayer:showOSD() end

---Set how long the OSD stays visible while playing (default 3000 ms).
---@param ms integer
function PicoDeckVideoPlayer:setOSDTimeout(ms) end

---@return boolean
function PicoDeckVideoPlayer:isPlaying() end

---@return boolean
function PicoDeckVideoPlayer:isPaused() end

---Return the video frame rate.
---@return number fps
function PicoDeckVideoPlayer:getFPS() end

---Return the video dimensions.
---@return integer width
---@return integer height
function PicoDeckVideoPlayer:getSize() end

---Return metadata and playback state.
---@return { width: integer, height: integer, frames: integer, current_frame: integer, dropped_frames: integer, has_audio: boolean, duration_ms: integer, position_ms: integer, ended: boolean, fps: number }
function PicoDeckVideoPlayer:getInfo() end

---Return `true` if the loaded AVI file contains an MP3 audio track.
---@return boolean
function PicoDeckVideoPlayer:hasAudio() end

---Set audio volume.
---@param vol integer 0–100
function PicoDeckVideoPlayer:setVolume(vol) end

---@return integer
function PicoDeckVideoPlayer:getVolume() end

---@param muted boolean
function PicoDeckVideoPlayer:setMuted(muted) end

---@return boolean
function PicoDeckVideoPlayer:isMuted() end

---Enable/disable looping (default off: the last frame is held and `hasEnded()` turns true).
---@param loop boolean
function PicoDeckVideoPlayer:setLoop(loop) end

---If `true` (default), `update()` calls `display.flush()` automatically after each frame.
---@param af boolean
function PicoDeckVideoPlayer:setAutoFlush(af) end

---Return the number of frames dropped since last `resetStats()`.
---@return integer
function PicoDeckVideoPlayer:getDroppedFrames() end

---Reset dropped-frame counter.
function PicoDeckVideoPlayer:resetStats() end

-- (Resources are freed by the garbage collector — there is no destroy() method.)

-- =============================================================================
-- picocalc.game  (camera, scene manager, save files)
-- =============================================================================

---@class picocalc.game
picocalc.game = {}

-- ── Camera ────────────────────────────────────────────────────────────────────

---@class picocalc.game.camera
picocalc.game.camera = {}

---@class PicoDeckCamera : userdata
local PicoDeckCamera = {}

---Create a new Camera at (0, 0) with zoom 1.0.
---@return PicoDeckCamera
function picocalc.game.camera.new() end

---@param x integer
---@param y integer
function PicoDeckCamera:setPosition(x, y) end

---@param dx integer
---@param dy integer
function PicoDeckCamera:move(dx, dy) end

---@return integer x
---@return integer y
function PicoDeckCamera:getPosition() end

---@param zoom number
function PicoDeckCamera:setZoom(zoom) end

---@return number
function PicoDeckCamera:getZoom() end

---Follow a world position: `update(dt)` eases the camera towards it. Call it
---again each frame to track a moving object.
---@param x number
---@param y number
---@param lag? number Smoothing time constant in seconds (default 0.15)
function PicoDeckCamera:setTarget(x, y, lag) end

---Clear the follow target.
function PicoDeckCamera:clearTarget() end

---Constrain the camera. The arguments are two corners, not a size.
---@param min_x number
---@param min_y number
---@param max_x number
---@param max_y number
function PicoDeckCamera:setBounds(min_x, min_y, max_x, max_y) end

---Remove world bounds.
function PicoDeckCamera:clearBounds() end

---Return the world bounds as `min_x, min_y, max_x, max_y`; nothing at all when
---the camera is unbounded.
---@return number? min_x
---@return number min_y
---@return number max_x
---@return number max_y
function PicoDeckCamera:getBounds() end

---Apply a full-screen shake effect. The duration counts down in the `dt`
---passed to `update(dt)`, so it is in seconds when `dt` is.
---@param amplitude number Pixels of shake
---@param duration number
function PicoDeckCamera:shake(amplitude, duration) end

---Apply a horizontal shake.
---@param amplitude number
---@param duration number In `update(dt)` units (seconds)
function PicoDeckCamera:shakeX(amplitude, duration) end

---Apply a vertical shake.
---@param amplitude number
---@param duration number In `update(dt)` units (seconds)
function PicoDeckCamera:shakeY(amplitude, duration) end

---Cancel an active shake.
function PicoDeckCamera:stopShake() end

---Convert world-space coordinates to screen-space.
---@param wx integer
---@param wy integer
---@return integer sx
---@return integer sy
function PicoDeckCamera:worldToScreen(wx, wy) end

---Convert screen-space coordinates to world-space.
---@param sx integer
---@param sy integer
---@return integer wx
---@return integer wy
function PicoDeckCamera:screenToWorld(sx, sy) end

---Update camera position (advances follow target, shake, etc.).
---@param dt number Seconds since the last update
function PicoDeckCamera:update(dt) end

---Return the current draw offset applied to the display.
---@return integer ox
---@return integer oy
function PicoDeckCamera:getOffset() end

-- ── Scene manager ─────────────────────────────────────────────────────────────

---@class picocalc.game.scene
picocalc.game.scene = {}

---@class PicoDeckScene : userdata
local PicoDeckScene = {}

---Create a new Scene. A scene is a table-like object with `update()`, `draw()`,
---`enter()`, and `exit()` lifecycle methods.
---@return PicoDeckScene
function picocalc.game.scene.new() end

---Add a scene to the manager (does not make it active).
---@param name string
---@param scene PicoDeckScene
function picocalc.game.scene.add(name, scene) end

---Remove a named scene.
---@param name string
function picocalc.game.scene.remove(name) end

---Return `true` if a scene with the given name is registered.
---@param name string
---@return boolean
function picocalc.game.scene.has(name) end

---Switch immediately to a named scene (calls `exit()` on current, `enter()` on next).
---@param name string
function picocalc.game.scene.switch(name) end

---Push a scene on the stack without exiting the current one.
---@param name string
function picocalc.game.scene.push(name) end

---Pop the top scene off the stack and return to the previous scene.
function picocalc.game.scene.pop() end

---Return the current scene.
---@return PicoDeckScene?
function picocalc.game.scene.getCurrent() end

---Call `update(dt)` on the current scene.
---@param dt? number Seconds (default 0.016)
function picocalc.game.scene.update(dt) end

---Call `draw()` on the current scene.
function picocalc.game.scene.draw() end

---Return (or create) an object pool associated with a scene.
---@param name string Scene name
---@param factory fun(): any Factory function for new objects
---@return table pool
function picocalc.game.scene.objectPool(name, factory) end

---Set a global value accessible to all scenes.
---@param key string
---@param value any
function picocalc.game.scene.setGlobal(key, value) end

---Get a global value.
---@param key string
---@return any
function picocalc.game.scene.getGlobal(key) end

---Clear all global values.
function picocalc.game.scene.clearGlobals() end

-- ── Save files ────────────────────────────────────────────────────────────────

---Per-app save slots, one JSON file each at `/data/<app id>/saves/<key>.json`.
---Keys are 1-128 bytes of `[A-Za-z0-9._-]`, contain no `..` and do not start
---with `.`; any other key is refused. A slot left in the old shared
---`/saves/<key>.json` is copied (never moved) into an app's slot the first
---time that app reads the key with `get`/`exists`.
---@class picocalc.game.save
picocalc.game.save = {}

---Write a table (nested tables, strings, numbers, booleans) to slot `key`.
---@param key string
---@param value table
---@return boolean ok
---@return string? err  "invalid save name" or an I/O error
function picocalc.game.save.set(key, value) end

---Read slot `key`; nil if it is missing, corrupt or `key` is invalid.
---@param key string
---@return table?
function picocalc.game.save.get(key) end

---Return `true` if slot `key` exists.
---@param key string
---@return boolean
function picocalc.game.save.exists(key) end

---Delete slot `key`; returns false if it did not exist or `key` is invalid.
---@param key string
---@return boolean
function picocalc.game.save.delete(key) end

---Return the names of this app's saved slots.
---@return string[]
function picocalc.game.save.list() end

-- =============================================================================
-- picocalc.terminal  (in-app virtual terminal widget)
-- =============================================================================

---@class picocalc.terminal
picocalc.terminal = {}

---@class PicoDeckTerminal : userdata
local PicoDeckTerminal = {}

---Create a terminal widget with `cols` × `rows` characters and `scrollback_lines` of history.
---@param cols integer
---@param rows integer
---@param scrollback_lines? integer
---@return PicoDeckTerminal
function picocalc.terminal.new(cols, rows, scrollback_lines) end

---Write a UTF-8 string (with ANSI escape codes) to the terminal.
---@param text string
function PicoDeckTerminal:write(text) end

---Clear all terminal content.
function PicoDeckTerminal:clear() end

---Move the cursor to (x, y) (0-based, column × row).
---@param x integer
---@param y integer
function PicoDeckTerminal:setCursor(x, y) end

---Return the current cursor position.
---@return integer x
---@return integer y
function PicoDeckTerminal:getCursor() end

---Set foreground and background colours.
---@param fg integer RGB565
---@param bg integer RGB565
function PicoDeckTerminal:setColors(fg, bg) end

---Return the current foreground and background colours.
---@return integer fg
---@return integer bg
function PicoDeckTerminal:getColors() end

---Scroll the terminal by `lines` lines (positive = down).
---@param lines integer
function PicoDeckTerminal:scroll(lines) end

---Render the full terminal to the display.
function PicoDeckTerminal:render() end

---Render only dirty (changed) cells to the display.
function PicoDeckTerminal:renderDirty() end

---@return integer
function PicoDeckTerminal:getCols() end

---@return integer
function PicoDeckTerminal:getRows() end

---@param visible boolean
function PicoDeckTerminal:setCursorVisible(visible) end

---@param blink boolean
function PicoDeckTerminal:setCursorBlink(blink) end

---Select the font for this terminal.
---@param font_id integer One of the FONT_* constants
function PicoDeckTerminal:setFont(font_id) end

---@return integer
function PicoDeckTerminal:getFont() end

---Mark all cells as dirty (forces a full re-render on next `renderDirty`).
function PicoDeckTerminal:markAllDirty() end

---Return `true` if all cells are dirty.
---@return boolean
function PicoDeckTerminal:isFullDirty() end

---Return the first and last dirty row indices.
---@return integer first
---@return integer last
function PicoDeckTerminal:getDirtyRange() end

---Return the number of lines in the scrollback buffer.
---@return integer
function PicoDeckTerminal:getScrollbackCount() end

---Return the content of scrollback line `line` as an array of cell values.
---@param line integer
---@return integer[]
function PicoDeckTerminal:getScrollbackLine(line) end

---Return the current scrollback display offset.
---@return integer
function PicoDeckTerminal:getScrollbackOffset() end

---Set the scrollback display offset.
---@param offset integer
function PicoDeckTerminal:setScrollbackOffset(offset) end

---Block until any key is pressed (system menu, HTTP callbacks etc. stay responsive).
function PicoDeckTerminal:waitForAnyKey() end

---Block until a specific key is pressed. Returns the button mask.
---@param key string Key name: `"enter"`, `"left"`, `"right"`, `"up"`, `"down"`, `"esc"`, `"f1"`–`"f5"`, `"tab"`, `"backspace"`
---@return integer button_mask
function PicoDeckTerminal:waitForKey(key) end

---Return the next key event without blocking, or `nil` if no key is pending.
---@return integer?
function PicoDeckTerminal:readKey() end

---Return the next ASCII character without blocking, or `nil`.
---@return string?
function PicoDeckTerminal:readChar() end

---Block until an ASCII character is typed. Returns the character.
---@return string
function PicoDeckTerminal:waitForChar() end

---Enable or disable line-number gutter.
---@param enabled boolean
function PicoDeckTerminal:setLineNumbers(enabled) end

---Set the starting line number displayed in the gutter.
---@param start integer
function PicoDeckTerminal:setLineNumberStart(start) end

---Set the width of the line-number gutter in characters.
---@param cols integer
function PicoDeckTerminal:setLineNumberCols(cols) end

---Set line-number gutter colours.
---@param fg integer RGB565
---@param bg integer RGB565
function PicoDeckTerminal:setLineNumberColors(fg, bg) end

---Return the number of content columns (total columns minus gutter width).
---@return integer
function PicoDeckTerminal:getContentCols() end

---Enable or disable the scrollbar.
---@param enabled boolean
function PicoDeckTerminal:setScrollbar(enabled) end

---Set scrollbar colours.
---@param bg integer RGB565 track colour
---@param thumb integer RGB565 thumb colour
function PicoDeckTerminal:setScrollbarColors(bg, thumb) end

---Set scrollbar width in pixels.
---@param width integer
function PicoDeckTerminal:setScrollbarWidth(width) end

---Update scrollbar thumb position.
---@param total_lines integer Total document line count
---@param scroll_position integer Current top-visible line
function PicoDeckTerminal:setScrollInfo(total_lines, scroll_position) end

---Enable or disable visual word-wrap (content is not modified).
---@param enabled boolean
function PicoDeckTerminal:setWordWrap(enabled) end

---Set the column at which visual word-wrap breaks.
---@param column integer
function PicoDeckTerminal:setWordWrapColumn(column) end

---Show or hide the wrap-continuation indicator.
---@param enabled boolean
function PicoDeckTerminal:setWrapIndicator(enabled) end

-- =============================================================================
-- picocalc.crypto  (hashing, symmetric encryption, key exchange)
-- =============================================================================

---@class picocalc.crypto
picocalc.crypto = {}

---@class PicoDeckAesCtr : userdata
local PicoDeckAesCtr = {}

---@class PicoDeckEcdh : userdata
local PicoDeckEcdh = {}

---Compute SHA-256. Returns a 32-byte binary string.
---@param data string
---@return string hash
function picocalc.crypto.sha256(data) end

---Compute SHA-1. Returns a 20-byte binary string.
---@param data string
---@return string hash
function picocalc.crypto.sha1(data) end

---Compute the SHA-256 of a file, streamed from the SD card (never loaded
---whole into memory). Returns a 64-character lowercase hex digest (not a
---binary string, unlike `sha256`), or `nil, error` if the file cannot be read.
---@param path string
---@return string? hex
---@return string? error
function picocalc.crypto.sha256File(path) end

---Compute HMAC-SHA256. Returns a 32-byte binary string.
---@param key string
---@param data string
---@return string mac
function picocalc.crypto.hmacSHA256(key, data) end

---Compute HMAC-SHA1. Returns a 20-byte binary string.
---@param key string
---@param data string
---@return string mac
function picocalc.crypto.hmacSHA1(key, data) end

---Generate `n` cryptographically random bytes (max 4096).
---@param n integer
---@return string bytes
function picocalc.crypto.randomBytes(n) end

---SSH key derivation (RFC 4253 §7.2). Returns `needed_len` derived bytes.
---@param K_mpint string Shared secret as an SSH mpint
---@param H string Exchange hash
---@param session_id string Session ID
---@param letter string Single-character key type (`"A"` – `"F"`)
---@param needed_len integer Number of output bytes required
---@return string derived
function picocalc.crypto.deriveKey(K_mpint, H, session_id, letter, needed_len) end

---Create an AES-CTR context. `key` must be 16 or 32 bytes; `iv` must be 16 bytes.
---@param key string AES key (16 or 32 bytes)
---@param iv string Initial counter/nonce (16 bytes)
---@return PicoDeckAesCtr
function picocalc.crypto.aes_ctr_new(key, iv) end

---Create an X25519 ECDH key-exchange context.
---@return PicoDeckEcdh
function picocalc.crypto.ecdh_x25519_new() end

---Create a P-256 ECDH key-exchange context.
---@return PicoDeckEcdh
function picocalc.crypto.ecdh_p256_new() end

---Verify an RSA signature. Returns `true` if valid.
---@param pubkey string DER-encoded RSA public key
---@param sig string Signature bytes
---@param hash string Hash of the signed data
---@return boolean valid
function picocalc.crypto.rsaVerify(pubkey, sig, hash) end

---Verify an ECDSA P-256 signature. Returns `true` if valid.
---@param pubkey string P-256 public key bytes (65-byte uncompressed)
---@param sig string Signature bytes
---@param hash string Hash of the signed data
---@return boolean valid
function picocalc.crypto.ecdsaP256Verify(pubkey, sig, hash) end

-- PicoDeckAesCtr methods

---Encrypt or decrypt `input` (XOR stream cipher — same operation in both directions).
---@param input string
---@return string output
function PicoDeckAesCtr:update(input) end

---Release the AES context. Also called by the GC.
function PicoDeckAesCtr:free() end

-- PicoDeckEcdh methods

---Return the public key bytes for this key-exchange context
---(32 bytes for X25519; 65-byte uncompressed point for P-256).
---@return string pubkey
function PicoDeckEcdh:getPublicKey() end

---Compute the shared secret from the peer's public key.
---Returns `nil, error` on failure.
---@param remote_pubkey string Peer's public key bytes
---@return string? shared_secret
---@return string? error
function PicoDeckEcdh:computeShared(remote_pubkey) end

---Release the ECDH context. Also called by the GC.
function PicoDeckEcdh:free() end

-- =============================================================================
-- picocalc.modplayer  (MOD tracker music)
-- =============================================================================

---@class picocalc.modplayer
picocalc.modplayer = {}

---@class PicoDeckModPlayer : userdata
local PicoDeckModPlayer = {}

---Return the MOD player handle. There is one player: while a handle is
---alive, `create()` returns that same handle (untouched); once every
---reference is dropped it is collected and the next `create()` makes a
---fresh one.
---@return PicoDeckModPlayer? player
---@return string? error
function picocalc.modplayer.create() end

---Load a .mod file.
---@param path string
---@return boolean ok
function PicoDeckModPlayer:load(path) end

---Start playback.
---@param loop? boolean Loop when the song ends (default false)
function PicoDeckModPlayer:play(loop) end

---Stop playback.
function PicoDeckModPlayer:stop() end

---Pause playback.
function PicoDeckModPlayer:pause() end

---Resume after pause.
function PicoDeckModPlayer:resume() end

---@return boolean
function PicoDeckModPlayer:isPlaying() end

---Set volume.
---@param vol integer 0–100
function PicoDeckModPlayer:setVolume(vol) end

---@return integer
function PicoDeckModPlayer:getVolume() end

---Enable or disable looping.
---@param loop boolean
function PicoDeckModPlayer:setLoop(loop) end

-- =============================================================================
-- picocalc.zip  (ZIP archive extraction)
-- =============================================================================

---@class picocalc.zip
picocalc.zip = {}

---List a ZIP archive's contents.
---@param zip_path string
---@return { name: string, size: integer, compressed_size: integer }[]? entries
---@return string? error
function picocalc.zip.list(zip_path) end

---Extract a ZIP archive into a directory. Optional progress callback.
---@param zip_path string
---@param dest_dir string
---@param progress_fn? fun(done: integer, total: integer)
---@return boolean ok
---@return string? error
function picocalc.zip.extract(zip_path, dest_dir, progress_fn) end

---@class PicoDeckZipArchive : userdata
local PicoDeckZipArchive = {}

---Open a ZIP archive for random access without extracting it (API v5).
---At most 4 archives may be open at once per app. Archives close via
---`:close()`, the GC, `<close>` scope exit, or automatically at app exit.
---@param path string
---@return PicoDeckZipArchive? archive
---@return string? error
function picocalc.zip.open(path) end

---List the archive's file entries (directory entries are skipped).
---@return { name: string, size: integer, compressed_size: integer }[] entries
function PicoDeckZipArchive:list() end

---Return `true` if an entry with this exact name exists.
---@param name string
---@return boolean
function PicoDeckZipArchive:exists(name) end

---Return an entry's uncompressed size in bytes, or `nil` if it does not exist.
---@param name string
---@return integer? bytes
function PicoDeckZipArchive:size(name) end

---Decompress a whole entry into a Lua string. Fails if the entry exceeds
---`max_len` (when given) or the 4 MB in-memory cap.
---@param name string
---@param max_len? integer Reject entries larger than this many bytes
---@return string? data
---@return string? error
function PicoDeckZipArchive:read(name, max_len) end

---Stream one entry to a file on the SD card (constant memory). Parent
---directories are created as needed.
---@param name string
---@param dest_path string
---@return boolean ok
---@return string? error
function PicoDeckZipArchive:extract(name, dest_path) end

---Extract every file entry into a directory. Optional progress callback.
---@param dest_dir string
---@param progress_fn? fun(done: integer, total: integer)
---@return boolean ok
---@return string? error
function PicoDeckZipArchive:extractAll(dest_dir, progress_fn) end

---Close the archive and release its SD file handle. Double close is a no-op.
---Also called by the GC and on `<close>` scope exit.
function PicoDeckZipArchive:close() end

-- =============================================================================
-- picocalc.json  (JSON encode/decode)
-- =============================================================================

---@class picocalc.json
---@field null userdata Sentinel representing JSON null (compare with json.isNull)
picocalc.json = {}

---Encode a Lua value as a JSON string.
---@param value any
---@param opts? {indent?: integer} Spaces per indent level (default 0: compact)
---@return string
function picocalc.json.encode(value, opts) end

---Decode a JSON string into Lua values. JSON null decodes to `json.null`.
---@param text string
---@return any
function picocalc.json.decode(text) end

---Return `true` if `v` is the `json.null` sentinel.
---@param v any
---@return boolean
function picocalc.json.isNull(v) end

-- =============================================================================
-- picocalc.repl  (interactive Lua REPL)
-- =============================================================================

---@class picocalc.repl
picocalc.repl = {}

---Read a line of REPL input (non-blocking). Returns the line, or nil if no
---complete line is ready (also nil on Esc).
---@return string?
function picocalc.repl.readline() end

---Print to the REPL output.
---@param ... any
function picocalc.repl.print(...) end

---Clear the REPL screen.
function picocalc.repl.clear() end

---Enable or disable input echo.
---@param flag boolean
function picocalc.repl.echo(flag) end

-- =============================================================================
-- draw3DWireframeEx  (global software-3D helper, not under picocalc.*)
-- =============================================================================

---Draw a rotating 3D wireframe (optionally filled) model.
---@param verts number[] Flat vertex array {x,y,z, x,y,z, ...}
---@param edges integer[] Flat edge index pairs {a,b, a,b, ...} (1-based)
---@param angleX number Rotation around X (radians)
---@param angleY number Rotation around Y (radians)
---@param angleZ number Rotation around Z (radians)
---@param scx integer Screen centre x
---@param scy integer Screen centre y
---@param fov number Field of view / perspective divisor
---@param edgeColor integer RGB565 line colour
---@param fillColor? integer RGB565 face fill colour (default 0)
---@param fillMode? integer 0=wireframe, 1=fill, 2=both (default 0)
---@param vertSize? integer Vertex dot size (default 3)
---@param faces? integer[] Flat vertex-index triples for filled triangles
function draw3DWireframeEx(verts, edges, angleX, angleY, angleZ, scx, scy, fov, edgeColor, fillColor, fillMode, vertSize, faces) end
