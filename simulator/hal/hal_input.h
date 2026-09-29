// HAL Input - SDL2 Keyboard Mapping
// Maps PC keyboard to PicoDeck buttons

#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>

// Button bitflags (must match src/os/os.h)
#define BTN_UP        (1 << 0)
#define BTN_DOWN      (1 << 1)
#define BTN_LEFT      (1 << 2)
#define BTN_RIGHT     (1 << 3)
#define BTN_ENTER     (1 << 4)    // Enter key
#define BTN_ESC       (1 << 5)    // Escape key
#define BTN_MENU      (1 << 6)    // System menu trigger
#define BTN_F1        (1 << 7)
#define BTN_F2        (1 << 8)
#define BTN_F3        (1 << 9)
#define BTN_F4        (1 << 10)
#define BTN_F5        (1 << 11)
#define BTN_F6        (1 << 12)
#define BTN_F7        (1 << 13)
#define BTN_F8        (1 << 14)
#define BTN_F9        (1 << 15)
#define BTN_BACKSPACE (1 << 16)   // Backspace key
#define BTN_TAB       (1 << 17)   // Tab key
#define BTN_DEL       (1 << 18)   // Delete key
#define BTN_SHIFT     (1 << 19)   // Shift modifier
#define BTN_CTRL      (1 << 20)   // Ctrl modifier
#define BTN_ALT       (1 << 21)   // Alt modifier
#define BTN_FN        (1 << 22)   // Fn/Symbol modifier

// Initialize input subsystem
bool hal_input_init(void);

// Shutdown input subsystem
void hal_input_shutdown(void);

// Handle SDL event
void hal_input_handle_event(const SDL_Event* event);

// Update input state (call once per frame)
void hal_input_update(void);

// Get current button states
uint32_t hal_input_get_buttons(void);
uint32_t hal_input_get_buttons_pressed(void);

// Atomically snapshot held+pressed buttons and clear edge state.
// Injected one-shot clicks stay asserted for at least HAL_INJECT_HOLD_MS
// (80ms) of wall-clock time before auto-release. A guaranteed released
// read-cycle separates repeated same-button injections, enforced by
// retire_and_publish_injected_click_locked(). Taking the mutex once for
// the whole operation closes the race where an RPC inject landing between
// separate read/clear calls was dropped.
void hal_input_read_buttons(uint32_t* out_buttons, uint32_t* out_pressed);

// Get character input (for text entry)
char hal_input_get_char(void);

// Poll for character (non-blocking)
bool hal_input_poll_char(char* out_char);

// Inject button press (for RPC control)
void hal_input_inject_buttons(uint32_t buttons);

// Hold buttons down until hal_input_release_buttons (modifier chords).
void hal_input_hold_buttons(uint32_t buttons);

// Release (possibly injected) buttons immediately (for RPC control)
void hal_input_release_buttons(uint32_t buttons);

// Inject a typed character into the char ring buffer (for RPC control)
void hal_input_inject_char(char c);

// Pop the oldest staged key event (down/up/char, in the order the keys were
// typed or injected). keyboard_stub.c's kbd_poll drains these into the event
// queue behind picocalc.input.pollEvent.
struct kbd_event_s;  // kbd_event_t, src/drivers/keyboard.h
bool hal_input_pop_event(struct kbd_event_s *out);

// Drop queued chars and pending/active one-shot button injections (the
// counterpart of keyboard.c's kbd_discard_pending FIFO drain).
void hal_input_discard_pending(void);

// ── Injection sequence numbers (test sync) ─────────────────────────────────
// Every injection (click/press/release/char, and the menu click, which the
// keyboard stub handles outside the button state) gets the next seq. An
// injection is consumed once the OS has read it: a button event when
// hal_input_read_buttons() returns with it published, a char when
// hal_input_get_char()/hal_input_poll_char() hands it out, the menu click
// when kbd_consume_menu_press() takes it.
uint32_t hal_input_last_issued_seq(void);
// consumed: highest seq such that it and every earlier seq are consumed.
void hal_input_get_seq_state(uint32_t *issued, uint32_t *consumed);
void hal_input_raise_menu(bool injected);  // injected: also note its input seq
bool hal_input_take_menu(void);            // test-and-clear the flag and its seq

#endif // HAL_INPUT_H
