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
#define KEY_NONE   0x00   // No key / idle

// Modifier key codes (sent as separate events when CFG_REPORT_MODS is set)
#define KEY_MOD_ALT  0xA1
#define KEY_MOD_SHL  0xA2   // Left Shift
#define KEY_MOD_SHR  0xA3   // Right Shift
#define KEY_MOD_SYM  0xA4   // Symbol / Fn
#define KEY_MOD_CTRL 0xA5

// Special system keys
#define KEY_BRK    0xD0   // Break key — intercepted by OS for screenshots

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

// Read battery percent from STM32 (0-100). Returns -1 on I2C error.
// Bit 7 of the raw value is a charging flag — this function masks it off.
int kbd_get_battery_percent(void);

// Charging flag from the most recent battery read (kbd_get_battery_percent
// refreshes it at most every 5 s). False until the first successful read.
bool kbd_is_charging(void);

// Set LCD backlight brightness 0-255 via STM32
void kbd_set_backlight(uint8_t brightness);

void kbd_apply_clock(void);

// Returns true (once) when F10 (the system menu key) was pressed since last call.
// The press is consumed and will not appear in kbd_get_buttons() — the OS
// intercepts BTN_MENU before apps can see it.
bool kbd_consume_menu_press(void);

// Returns true (once) when the Brk key (0xD0) was pressed since last call.
// Brk is intercepted by the OS for screenshots and is never visible to apps.
bool kbd_consume_screenshot_press(void);

// Clear all keyboard state (buttons, chars, the event queue, the key-down
// set). Call this after an app exits to prevent button presses in the
// launcher from being "inherited" by the next app. A key still physically
// held afterwards is picked up by its next HOLD report without a press edge.
void kbd_clear_state(void);

// Drop all queued input (the STM32 key FIFO, pending injected keys and
// chars) and clear the state.  For consent dialogs: a key typed before the
// dialog appeared must not answer it.
void kbd_discard_pending(void);

// Force I2C bus recovery — useful after USB MSC mode or other bus-corrupting events.
// Pulses SCL 9 times to clear stuck STM32 state and reinitializes I2C peripheral.
void kbd_recover_i2c_bus(void);

// Inject a one-shot button press (BTN_* from os.h). The press is published by
// the next kbd_poll() and then held for a minimum wall-clock duration
// (KBD_INJECT_HOLD_MS, currently 80ms) before being auto-released, rather
// than for exactly one poll cycle — apps that call kbd_poll() more than once
// per logical frame (e.g. watchdog-feed pumps) would otherwise retire the
// press before ever sampling it. An app's update→read sequence still always
// observes both a press and a release edge; a repeat injection of the same
// button while it's still active is queued and only republished after a full
// release cycle, guaranteeing a real release-then-press edge.
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
// queued as a down / char / up event triple for kbd_poll_event().
void kbd_inject_char(char c);

// ── Event queue (picocalc.input.pollEvent / isKeyDown) ───────────────────────
// kbd_poll() decodes every STM32 FIFO item, in order, into a small queue
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
