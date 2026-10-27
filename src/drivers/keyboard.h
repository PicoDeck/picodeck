#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// PicoCalc Keyboard Driver
// Reads key events from the STM32F103 keyboard controller via I2C1.
// The STM32 also manages battery status and LCD backlight.
//
// Key event format: 2 bytes read from REG_FIF (0x09)
//   byte[0] = state:   1=pressed, 2=hold, 3=released, 0=idle
//   byte[1] = keycode: ASCII for printable keys, or a special constant below
//
// Source: clockworkpi/PicoCalc picocalc_keyboard firmware (reg.h / keyboard.h)
// =============================================================================

// Special key codes (non-ASCII, from STM32 keyboard firmware keyboard.h)
#define KEY_UP     0xB5
#define KEY_DOWN   0xB6
#define KEY_LEFT   0xB4
#define KEY_RIGHT  0xB7
#define KEY_ENTER  0x0A   // LF — what the firmware sends for Enter
#define KEY_ESC    0xB1
#define KEY_BKSPC  0x08   // ASCII backspace
#define KEY_TAB    0x09   // ASCII tab
#define KEY_DEL    0xD4   // Delete key (Shift+Delete sends End, 0xD5)
#define KEY_NONE   0x00   // No key / idle

// Modifier key codes (sent as separate events when CFG_REPORT_MODS is set)
#define KEY_MOD_ALT  0xA1
#define KEY_MOD_SHL  0xA2   // Left Shift
#define KEY_MOD_SHR  0xA3   // Right Shift
#define KEY_MOD_SYM  0xA4   // Symbol / Fn
#define KEY_MOD_CTRL 0xA5

// Special system keys
#define KEY_BRK    0xD0   // Break key — intercepted by OS for screenshots

// What the keyboard controller sends for a key while Shift is held (it
// recomputes the code at every transition, picocalc_keyboard keyboard.ino):
// Shift+F1..F5 = F6..F10 (F10 is the system menu key), Shift+Esc = Brk,
// and these. Shift+Left/Right/Backspace/Space send nothing at all.
#define KEY_INSERT 0xD1   // Shift+Enter (also Alt+I)
#define KEY_HOME   0xD2   // Shift+Tab
#define KEY_END    0xD5   // Shift+Delete
#define KEY_PGUP   0xD6   // Shift+Up
#define KEY_PGDN   0xD7   // Shift+Down

// Function keys
#define KEY_F1     0x81
#define KEY_F2     0x82
#define KEY_F3     0x83
#define KEY_F4     0x84
#define KEY_F5     0x85
#define KEY_F6     0x86
#define KEY_F7     0x87
#define KEY_F8     0x88
#define KEY_F9     0x89
#define KEY_F10    0x90

// Init I2C1 and keyboard polling. Returns true if STM32 responded.
bool kbd_init(void);

// Poll the keyboard controller. Must be called once per frame.
// Populates the internal key state used by all other functions.
void kbd_poll(void);

// Returns the ASCII char typed this frame (0 = none). Also returns KEY_BKSPC
// (0x08) when backspace is pressed. One char per kbd_poll(): when several keys
// arrive in one poll the rest are kept (KBD_CHAR_BACKLOG) and returned by the
// following polls, in order.
char kbd_get_char(void);

// Returns the raw keycode of the last key pressed this frame (0 = none).
// Unlike kbd_get_char(), this captures every key including arrows, F-keys,
// modifiers, and any other keycode the STM32 sends — useful for debugging
// and for mapping keys that don't have BTN_* entries yet.
uint8_t kbd_get_raw_key(void);

// Bitmask of currently held button/key states (BTN_* flags from os.h)
uint32_t kbd_get_buttons(void);

// Edge-detect: buttons that became pressed this frame
uint32_t kbd_get_buttons_pressed(void);

// Edge-detect: buttons that were released this frame
uint32_t kbd_get_buttons_released(void);

// Battery percent (0-100), -1 until the first read. The bus engine reads it
// every 5 s in the background; this never touches the bus.
int kbd_get_battery_percent(void);

// Charging flag from the most recent battery read (the bus engine refreshes
// it at most every 5 s). False until the first successful read.
bool kbd_is_charging(void);

// Queue a backlight level (0-255) for the bus engine; the latest wins and it
// is written within one FIFO re-read interval (10 ms normally, up to 500 ms
// in USB storage mode — see kbd_set_poll_interval_ms). Never blocks.
void kbd_set_backlight(uint8_t brightness);

void kbd_apply_clock(void);

// Returns true (once) when F10 (the system menu key) was pressed since last call,
// or a pad source's Home button (pad_source.h). The press is consumed and
// will not appear in kbd_get_buttons() — the OS intercepts BTN_MENU before
// apps can see it.
bool kbd_consume_menu_press(void);

// Returns true (once) when the Brk key (0xD0) was pressed since last call.
// Brk is intercepted by the OS for screenshots and is never visible to apps.
bool kbd_consume_screenshot_press(void);

// Clear all keyboard state (buttons, the gamepad, chars, the event queue, the
// key-down set). Call this after an app exits to prevent button presses in
// the launcher from being "inherited" by the next app. A key still physically
// held afterwards is picked up by its next HOLD report without a press edge.
void kbd_clear_state(void);

// Drop all queued input (the STM32 key FIFO, pending injected keys and
// chars) and clear the state. Non-blocking; items the STM32 queued before the
// call never reach the app. For consent dialogs: a key typed before the
// dialog appeared must not answer it.
void kbd_discard_pending(void);

// Force I2C bus recovery — useful after USB MSC mode or other bus-corrupting
// events. Pulses SCL 9 times to clear stuck STM32 state and reinitializes the
// I2C peripheral. Blocks ~12 ms; the bus engine is paused around it.
void kbd_recover_i2c_bus(void);

// Stop the keyboard bus engine at a transaction boundary (waits <= ~20 ms),
// e.g. before a clock change; kbd_apply_clock() or kbd_resume_bus() restarts it.
void kbd_pause_bus(void);
void kbd_resume_bus(void);
// Call right before a deliberate reset (reboot, BOOTSEL, OTA): stops the
// keyboard bus engine at a transaction boundary (<= ~20 ms) so the reset
// never cuts an STM32 transaction mid-byte, which can lock the STM32's I2C
// slave until a power cycle. Task context, interrupts enabled.
void kbd_prepare_reset(void);
// FIFO re-read interval once it reads empty; 0 restores the default (10 ms).
// USB storage mode slows it to 500 ms.
void kbd_set_poll_interval_ms(uint32_t ms);

// Inject a one-shot button press (BTN_* from os.h). The press is published by
// the next kbd_poll() and then held for a minimum wall-clock duration
// (KBD_INJECT_HOLD_MS, currently 80ms) before being auto-released, rather
// than for exactly one poll cycle — apps that call kbd_poll() more than once
// per logical frame (e.g. watchdog-feed pumps) would otherwise retire the
// press before ever sampling it. An app's update→read sequence still always
// observes both a press and a release edge; a repeat injection of the same
// button while it's still active is queued and only republished after a full
// release cycle, guaranteeing a real release-then-press edge. A gamepad
// button bound to the key follows it (so do kbd_hold_buttons' latches).
void kbd_inject_buttons(uint32_t buttons);

// Hold buttons down until kbd_release_buttons() — for injected modifier
// chords (e.g. hold ctrl, type 's', release ctrl). BTN_MENU is click-only.
void kbd_hold_buttons(uint32_t buttons);

// Release injected buttons. Clears both latched holds (from kbd_hold_buttons)
// and any in-flight injected one-shot clicks (active and pending), ensuring
// a keyup always terminates the key completely.
void kbd_release_buttons(uint32_t buttons);

// Inject a character. The character is stored in s_last_char and consumed on the
// next call to kbd_get_char() (similar to real keyboard input). It is also
// queued as a down / char / up event triple for kbd_poll_event(), and a
// gamepad button bound to its key reads as a tap at the next kbd_poll() (held
// for that poll, released at the one after).
void kbd_inject_char(char c);

// ── Event queue (picocalc.input.pollEvent / isKeyDown) ───────────────────────
// kbd_poll() decodes the STM32 FIFO items, in order (up to 8 per poll; the
// rest wait for the next), into a small queue
// (KBD_EVENT_QUEUE_LEN in kbd_event_queue.h; the oldest event is dropped when
// it is full). The queue is independent of kbd_get_char()/the button masks:
// reading one does not consume the other.

#define KBD_EV_DOWN 1
#define KBD_EV_UP 2
#define KBD_EV_CHAR 3

// kbd_event_t.flags: modifiers held at the event, plus the repeat flag.
#define KBD_MOD_SHIFT 0x01
#define KBD_MOD_CTRL 0x02
#define KBD_MOD_ALT 0x04
#define KBD_MOD_FN 0x08
#define KBD_EVF_REPEAT 0x80 // down/char produced by the STM32's HOLD report

typedef struct kbd_event_s {
  uint8_t type;  // KBD_EV_*
  uint8_t key;   // STM32 keycode (ASCII for printable keys, KEY_* otherwise)
  uint8_t ch;    // KBD_EV_CHAR: the char (as kbd_get_char would return it)
  uint8_t flags; // KBD_MOD_* | KBD_EVF_REPEAT
} kbd_event_t;

// Pop the oldest queued event. Returns false when the queue is empty.
bool kbd_poll_event(kbd_event_t *out);

// True while the key is held (down seen, up not yet). Letters are
// case-insensitive. Reliable for buttons (arrows, Enter, Esc, F-keys,
// modifiers). Letters and shifted symbols depend on the STM32 reporting their
// release under the same keycode: pending hardware confirmation. A key that
// sticks is cleared by kbd_clear_state().
bool kbd_is_key_down(uint8_t keycode);

// Read pending key input without starting a new app-facing poll (used by
// sys.sleep, which must see the Sym key): events and chars queue up and
// press edges accumulate, so the app's next kbd_poll() still delivers what
// arrived meanwhile. Sets the menu/screenshot flags at once.
void kbd_poll_background(void);

// Drop queued events only (held state is kept). Called when an app starts so
// it does not receive the launcher's keys.
void kbd_flush_events(void);

// ── Gamepad (picocalc.gamepad / g_api.gamepad) ───────────────────────────────
// A logical gamepad whose 12 buttons (PAD_* in os.h: bit i is button i) are
// aliases for keys: a bound key still reports as itself everywhere else
// (BTN_*, getChar, events, isKeyDown). Each button has a primary and an
// alternate slot holding a keycode (0 = unbound). Keys are matched like the
// key-down set (letters case-folded, stored lower case), and the codes a key
// takes while Shift is held fold back to it (kbd_event_queue.h, "Gamepad").
// kbd_poll() updates it with the same rules as the button masks (a tap
// inside one poll is held for that poll; a key held across kbd_clear_state
// gives no press edge), and injected keys drive it too.
// src/os/gamepad_map.h builds the map.

#define KBD_PAD_BUTTONS 12  // PAD_UP .. PAD_SELECT
#define KBD_PAD_SLOTS 2     // 0 = primary, 1 = alternate

typedef struct {
  uint8_t key[KBD_PAD_BUTTONS][KBD_PAD_SLOTS];
} kbd_padmap_t;

// The default bindings (primary slots only), in PAD_* order: arrows, then
// A=F4, B=F5, X=Delete, Y=Backspace, L=F2, R=F3, Start=F1, Select=Tab.
#define KBD_PAD_DEFAULT_MAP                                                    \
  {{{KEY_UP, 0}, {KEY_DOWN, 0}, {KEY_LEFT, 0}, {KEY_RIGHT, 0},                 \
    {KEY_F4, 0}, {KEY_F5, 0}, {KEY_DEL, 0}, {KEY_BKSPC, 0},                    \
    {KEY_F2, 0}, {KEY_F3, 0}, {KEY_F1, 0}, {KEY_TAB, 0}}}

// PAD_* held / pressed this poll / released this poll, as getButtons*: the
// keyboard's aliases ORed with every other pad source (src/drivers/
// pad_source.h: the `pad` dev command, the simulator's game controller,
// a Bluetooth pad), so a button is held while any source holds
// it and an app cannot tell where a press came from.
uint32_t kbd_get_pad(void);
uint32_t kbd_get_pad_pressed(void);
uint32_t kbd_get_pad_released(void);

// The OS's own menus (launcher, system menu): the BTN_* navigation that the
// pad sources' presses of this poll stand for (D-pad = arrows, A = Enter,
// B = Esc; pad_nav_buttons). Not the keyboard aliases: their keys already
// report as BTN_*. So a pad-only player can open the menu with Home, pick
// Exit and launch the next game.
uint32_t kbd_get_pad_nav_pressed(void);

// Install the effective map (copied). Gamepad state is dropped without edges;
// a key still held is picked up again quietly by its next HOLD report.
void kbd_set_pad_map(const kbd_padmap_t *map);
// The installed map (never NULL). Starts as KBD_PAD_DEFAULT_MAP.
const kbd_padmap_t *kbd_get_pad_map(void);
