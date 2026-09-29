#pragma once

// =============================================================================
// Keyboard event queue and STM32 FIFO decoder (header-only, static inline)
//
// Shared by the firmware driver (src/drivers/keyboard.c), the simulator's
// keyboard stub (simulator/stubs/keyboard_stub.c) and its input HAL
// (simulator/hal/hal_input.c), and the host unit test
// (tests/unit/test_kbd_event_queue.c). Only one driver is linked per build, so
// the state (kbd_input_t) is a static in whichever driver includes this.
//
// What it fixes: kbd_poll() used to read the STM32 FIFO directly and keep
// only the net button state and the last char, so a tap shorter than a poll
// produced no edge and two chars in one poll became one. Now the bus engine
// (kbd_i2c.c) reads the FIFO in the background and kbd_poll() decodes what
// it read, in order (up to 8 items per poll; the rest wait for the next), into
//   - a small event queue   (down / up / char, drained by input.pollEvent)
//   - a char backlog        (getChar still returns one char per poll, but a
//                            second char in the same poll arrives next poll)
//   - a key-down set        (input.isKeyDown, any keycode incl. letters)
//   - the button masks      (a press+release inside one poll is held for one
//                            poll, so getButtons()/getButtonsPressed() see it)
//   - the gamepad masks     (PAD_*: keys resolved through the pad map, same
//                            rules as the button masks; see "Gamepad" below)
// A HOLD item is a repeat of a press already seen, never a fresh press edge.
// =============================================================================

#include "keyboard.h"
#include "../os/os.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// STM32 FIFO item states (fifo_item.state)
#define KBD_FIFO_IDLE 0
#define KBD_FIFO_PRESSED 1
#define KBD_FIFO_HOLD 2
#define KBD_FIFO_RELEASED 3

#define KBD_EVENT_QUEUE_LEN 16 // events; oldest dropped when full
#define KBD_CHAR_BACKLOG 4     // chars awaiting getChar; oldest dropped

#define KBD_MOD_BUTTONS (BTN_SHIFT | BTN_CTRL | BTN_ALT | BTN_FN)

typedef struct {
  kbd_event_t ev[KBD_EVENT_QUEUE_LEN];
  uint8_t head;  // index of the oldest event
  uint8_t count;
} kbd_event_queue_t;

typedef struct {
  uint32_t bits[8]; // one bit per keycode 0..255 (letters case-folded)
} kbd_keyset_t;

typedef struct {
  kbd_event_queue_t q;
  kbd_keyset_t down;
  kbd_keyset_t unseen; // held keys whose press we never saw (quiet HOLD)
  char chars[KBD_CHAR_BACKLOG];
  uint8_t char_head;
  uint8_t char_count;
  uint8_t ev_pushed;   // wrapping counters: how many were added this poll
  uint8_t char_pushed;
} kbd_input_t;

// Gamepad state: masks (PAD_* bits) kept by the button masks' rules, which
// map slots have their key down, and the controller's Shift / Alt state.
typedef struct {
  uint16_t curr, prev, tapped, deferred; // as in kbd_buttons_t
  uint32_t down; // slots whose key is down: bit 2 * button + slot
  uint8_t mods;  // KBD_PADMOD_*
} kbd_pad_t;

#define KBD_PADMOD_SHL 0x01
#define KBD_PADMOD_SHR 0x02
#define KBD_PADMOD_ALT 0x04
#define KBD_PADMOD_SHIFT (KBD_PADMOD_SHL | KBD_PADMOD_SHR)

// Button masks as kbd_poll maintains them.
typedef struct {
  uint32_t curr;     // held now
  uint32_t prev;     // held at the previous poll
  uint32_t tapped;   // got a fresh PRESSED during this poll
  uint32_t deferred; // pressed and released inside this poll: released next
  bool in_bg;        // background polls ran since the last foreground poll
  kbd_pad_t pad;     // the gamepad, polled (and cleared) with the buttons
} kbd_buttons_t;

// ── Keycode helpers ──────────────────────────────────────────────────────────

// keycode <-> BTN_* table. Both Shift keys map to BTN_SHIFT; the reverse
// lookup returns the first (left Shift). Kept in a function so a translation
// unit that includes this header without using it gets no unused-variable
// warning.
typedef struct {
  uint8_t key;
  uint32_t btn;
} kbd_btn_map_t;

static inline const kbd_btn_map_t *kbd_btn_map(void) {
  static const kbd_btn_map_t map[] = {
      {KEY_UP, BTN_UP},         {KEY_DOWN, BTN_DOWN},
      {KEY_LEFT, BTN_LEFT},     {KEY_RIGHT, BTN_RIGHT},
      {KEY_ENTER, BTN_ENTER},   {KEY_ESC, BTN_ESC},
      {KEY_F1, BTN_F1},         {KEY_F2, BTN_F2},
      {KEY_F3, BTN_F3},         {KEY_F4, BTN_F4},
      {KEY_F5, BTN_F5},         {KEY_F6, BTN_F6},
      {KEY_F7, BTN_F7},         {KEY_F8, BTN_F8},
      {KEY_F9, BTN_F9},         {KEY_F10, BTN_MENU},
      {KEY_BKSPC, BTN_BACKSPACE}, {KEY_TAB, BTN_TAB},
      {KEY_DEL, BTN_DEL},
      {KEY_MOD_SHL, BTN_SHIFT}, {KEY_MOD_SHR, BTN_SHIFT},
      {KEY_MOD_CTRL, BTN_CTRL}, {KEY_MOD_ALT, BTN_ALT},
      {KEY_MOD_SYM, BTN_FN},    {0, 0}};
  return map;
}

static inline uint32_t kbd_keycode_to_button(uint8_t key) {
  for (const kbd_btn_map_t *m = kbd_btn_map(); m->btn; m++)
    if (m->key == key)
      return m->btn;
  return 0;
}

// `btn` must be a single BTN_* bit.
static inline uint8_t kbd_button_to_keycode(uint32_t btn) {
  for (const kbd_btn_map_t *m = kbd_btn_map(); m->btn; m++)
    if (m->btn == btn)
      return m->key;
  return 0;
}

static inline uint8_t kbd_mods_from_buttons(uint32_t b) {
  return (uint8_t)(((b & BTN_SHIFT) ? KBD_MOD_SHIFT : 0) |
                   ((b & BTN_CTRL) ? KBD_MOD_CTRL : 0) |
                   ((b & BTN_ALT) ? KBD_MOD_ALT : 0) |
                   ((b & BTN_FN) ? KBD_MOD_FN : 0));
}

static inline uint32_t kbd_buttons_from_mods(uint8_t m) {
  return ((m & KBD_MOD_SHIFT) ? BTN_SHIFT : 0) |
         ((m & KBD_MOD_CTRL) ? BTN_CTRL : 0) | ((m & KBD_MOD_ALT) ? BTN_ALT : 0) |
         ((m & KBD_MOD_FN) ? BTN_FN : 0);
}

// The char a key produces (what getChar returns), 0 for none. Ctrl+letter
// gives the control code 0x01-0x1A.
static inline char kbd_keycode_to_char(uint8_t key, bool ctrl) {
  if (key >= 0x20 && key < 0x7F) {
    char upper = (char)(key & ~0x20);
    if (ctrl && upper >= 'A' && upper <= 'Z')
      return (char)(upper - 'A' + 1);
    return (char)key;
  }
  if (key == KEY_BKSPC)
    return (char)KEY_BKSPC;
  if (key == KEY_ENTER)
    return '\n';
  return 0;
}

// The OS consumes these; apps never see them as events.
static inline bool kbd_key_is_os_only(uint8_t key) {
  return key == KEY_F10 || key == KEY_BRK;
}

// Letters are tracked case-folded: Shift can be released before the letter,
// and the STM32 names the key by the char it makes at that moment.
static inline uint8_t kbd_key_fold(uint8_t key) {
  return (key >= 'A' && key <= 'Z') ? (uint8_t)(key | 0x20) : key;
}

// ── Key-down set ─────────────────────────────────────────────────────────────

static inline bool kbd_keyset_test(const kbd_keyset_t *s, uint8_t key) {
  key = kbd_key_fold(key);
  return (s->bits[key >> 5] >> (key & 31)) & 1u;
}

static inline void kbd_keyset_set(kbd_keyset_t *s, uint8_t key) {
  key = kbd_key_fold(key);
  s->bits[key >> 5] |= 1u << (key & 31);
}

static inline void kbd_keyset_clear(kbd_keyset_t *s, uint8_t key) {
  key = kbd_key_fold(key);
  s->bits[key >> 5] &= ~(1u << (key & 31));
}

// ── Gamepad ──────────────────────────────────────────────────────────────────
// Every key transition, whatever its source, reaches the gamepad as a FIFO
// state (PRESSED / HOLD / RELEASED) for one keycode: kbd_fifo_apply_pad for
// STM32 items, kbd_pad_event for decoded events (the simulator), kbd_inject_*
// for injected keys. The keycode is resolved through the pad map
// (kbd_padmap_t, keyboard.h) to a map slot, whose key is then down or up in
// kbd_pad_t.down; a button is held while any of its slots is down. The masks
// follow the button masks' rules:
//   PRESSED  press edge; a press+release inside one poll reads as held for
//            that poll and released at the next (tapped / deferred);
//   HOLD     held without a press edge when the button was not held (a key
//            pressed before kbd_clear_state or a map change);
//   RELEASED released once none of the button's slots is down.
// The gamepad does not use the key-down set: the keyboard controller
// (picocalc_keyboard, keyboard.ino transition_to) recomputes a key's code
// at every transition from the modifiers held at that moment, so a key
// pressed before Shift or Alt goes down is held and released under another
// code, or under none. The controller also clears its own Shift flag one
// scan (>= 17 ms) after it reports Shift up, and scans the matrix keys
// before the side buttons (Shift, then the arrows), so shifted codes still
// arrive just after the Shift RELEASED item. The gamepad tracks Shift and
// Alt itself and:
//   - folds the codes keys take under Shift back to the key (kbd_pad_unshift
//     over kbd_key_unshift: F6-F10, End, Home, Insert, Brk, PgUp, PgDn;
//     letters are case-folded anyway) whatever the tracked Shift state:
//     only Shift produces them (keyboard.ino has no key that sends them
//     unshifted). F10 (the system menu key) and Brk (the screenshot key) are
//     the OS's when pressed: they only ever release a button, never press
//     or hold one. Insert is also Alt+I (with or without Shift): its press
//     only taps Enter's button (while Shift alone is tracked), its HOLD
//     (only Enter repeats as one) holds it, and its release releases it
//     unless Alt alone is tracked;
//   - releases Left, Right, Backspace and Space when Shift goes down while
//     they are held: the controller reports nothing for them under Shift;
//   - releases B, I and Space when Alt goes down while they are held, and
//     ignores their presses while Alt is held: under Alt the controller
//     rewrites their press and release (nothing for B and Space, Insert for
//     I) but not their repeats, which come as plain PRESSED items, so a
//     button pressed by a repeat would never see its release.
// Held on through Shift / Alt, those keys come back at their next repeat
// (a PRESSED) once the modifier is up. A key pressed within a scan of Shift
// going up may miss its press edge (Left, Right, Backspace, Space, Enter),
// but never sticks. Two cases can leave a button held until the next
// kbd_clear_state (the system menu, an app exit, input.clearState()):
//   - Enter released within a scan of Shift going up while Alt is still
//     held (its Insert then reads as Alt+I);
//   - B, I or Space held while the controller's num lock is on: LShift and
//     Alt pressed together turn it on, it acts as Alt held, and it stays on
//     until Shift is pressed alone. The gamepad cannot see it (only a
//     register read could, and the bus engine does not do one).
// kbd_clear_state and a map change drop the whole state.

// The key behind a code the controller sends while Shift is held, 0 when the
// code is no shifted key's.
static inline uint8_t kbd_key_unshift(uint8_t key) {
  switch (key) {
  case KEY_F6: return KEY_F1;
  case KEY_F7: return KEY_F2;
  case KEY_F8: return KEY_F3;
  case KEY_F9: return KEY_F4;
  case KEY_F10: return KEY_F5;
  case KEY_BRK: return KEY_ESC;
  case KEY_END: return KEY_DEL;
  case KEY_HOME: return KEY_TAB;
  case KEY_INSERT: return KEY_ENTER;
  case KEY_PGUP: return KEY_UP;
  case KEY_PGDN: return KEY_DOWN;
  default: return 0;
  }
}

// The map slot (2 * button + slot) key is bound to, -1 when unbound. At most
// one: gamepad_map.c keeps one button per key.
static inline int kbd_pad_slot(const kbd_padmap_t *m, uint8_t key) {
  if (!m || !key)
    return -1;
  key = kbd_key_fold(key);
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    for (int s = 0; s < KBD_PAD_SLOTS; s++)
      if (m->key[b][s] && kbd_key_fold(m->key[b][s]) == key)
        return b * KBD_PAD_SLOTS + s;
  return -1;
}

// The PAD_* bit key is bound to (0 when unbound).
static inline uint16_t kbd_pad_lookup(const kbd_padmap_t *m, uint8_t key) {
  int slot = kbd_pad_slot(m, key);
  return slot < 0 ? 0 : (uint16_t)(1u << (slot / KBD_PAD_SLOTS));
}

// The buttons that own the slots in `slots`.
static inline uint16_t kbd_pad_buttons_of(uint32_t slots) {
  uint16_t pad = 0;
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    if (slots & (3u << (b * KBD_PAD_SLOTS)))
      pad |= (uint16_t)(1u << b);
  return pad;
}

// The slots of the keys behind a BTN_* mask (injected buttons).
static inline uint32_t kbd_pad_slots_from_buttons(const kbd_padmap_t *m,
                                                  uint32_t buttons) {
  uint32_t slots = 0;
  for (uint32_t b = 1; b && b <= buttons; b <<= 1) {
    int slot = (buttons & b) ? kbd_pad_slot(m, kbd_button_to_keycode(b)) : -1;
    if (slot >= 0)
      slots |= 1u << slot;
  }
  return slots;
}

// The PAD_* bits of the keys behind a BTN_* mask.
static inline uint16_t kbd_pad_from_buttons(const kbd_padmap_t *m,
                                            uint32_t buttons) {
  return kbd_pad_buttons_of(kbd_pad_slots_from_buttons(m, buttons));
}

static inline void kbd_pad_begin_poll(kbd_pad_t *p) {
  p->prev = p->curr;
  p->curr &= (uint16_t)~p->deferred;
  p->deferred = 0;
  p->tapped = 0;
}

static inline void kbd_pad_press(kbd_pad_t *p, uint16_t bits) {
  p->curr |= bits;
  p->tapped |= bits;
  p->deferred &= (uint16_t)~bits;
}

// Held, but not a fresh press edge.
static inline void kbd_pad_hold(kbd_pad_t *p, uint16_t bits) {
  bits &= (uint16_t)~p->curr;
  p->curr |= bits;
  p->prev |= bits;
}

// Release the buttons of `bits` that no slot holds any more. One pressed in
// this very poll (a tap) reads as held until the next poll.
static inline void kbd_pad_settle(kbd_pad_t *p, uint16_t bits) {
  uint16_t rel = (uint16_t)(bits & ~kbd_pad_buttons_of(p->down));
  uint16_t defer = (uint16_t)(rel & p->tapped & ~p->prev);
  p->deferred |= defer;
  p->curr &= (uint16_t)~(rel & ~defer);
}

// The keys of `slots` went up.
static inline void kbd_pad_up(kbd_pad_t *p, uint32_t slots) {
  p->down &= ~slots;
  kbd_pad_settle(p, kbd_pad_buttons_of(slots));
}

// Release the slots bound to any of the n keys (keys going silent under a
// modifier).
static inline void kbd_pad_drop_keys(kbd_pad_t *p, const kbd_padmap_t *m,
                                     const uint8_t *keys, int n) {
  uint32_t slots = 0;
  for (int i = 0; i < n; i++) {
    int slot = kbd_pad_slot(m, keys[i]);
    if (slot >= 0)
      slots |= 1u << slot;
  }
  kbd_pad_up(p, slots & p->down);
}

// The keys whose press and release the controller rewrites under Alt.
static inline bool kbd_key_alt_mangled(uint8_t key) {
  key = kbd_key_fold(key);
  return key == 'b' || key == 'i' || key == ' ';
}

// A Shift or Alt transition (KBD_PADMOD_* bit `mod`).
static inline void kbd_pad_mod(kbd_pad_t *p, const kbd_padmap_t *m,
                               uint8_t state, uint8_t mod) {
  static const uint8_t k_shift_silent[] = {KEY_LEFT, KEY_RIGHT, KEY_BKSPC, ' '};
  static const uint8_t k_alt_silent[] = {'b', 'i', ' '};
  if (state == KBD_FIFO_RELEASED) {
    p->mods &= (uint8_t)~mod;
    return;
  }
  uint8_t before = p->mods;
  p->mods |= mod;
  if ((mod & KBD_PADMOD_SHIFT) && !(before & KBD_PADMOD_SHIFT))
    kbd_pad_drop_keys(p, m, k_shift_silent, (int)sizeof(k_shift_silent));
  if ((mod & KBD_PADMOD_ALT) && !(before & KBD_PADMOD_ALT))
    kbd_pad_drop_keys(p, m, k_alt_silent, (int)sizeof(k_alt_silent));
}

// The key a transition's code stands for on the gamepad (see "Gamepad"
// above), 0 when the transition must not reach it.
static inline uint8_t kbd_pad_unshift(const kbd_pad_t *p, uint8_t state,
                                      uint8_t key) {
  uint8_t base = kbd_key_unshift(key);
  if (!base)
    return key;
  if (key == KEY_F10 || key == KEY_BRK)  // the menu / screenshot key
    return state == KBD_FIFO_RELEASED ? base : 0;
  if (key == KEY_INSERT) {  // Shift+Enter, or Alt+I (also with Shift)
    bool shift = (p->mods & KBD_PADMOD_SHIFT) != 0;
    bool alt = (p->mods & KBD_PADMOD_ALT) != 0;
    if (state == KBD_FIFO_HOLD)
      return base;  // only Enter repeats as a HOLD Insert (I: PRESSED 'I')
    if (state == KBD_FIFO_RELEASED)
      return (shift || !alt) ? base : key;
    return key;     // PRESSED: kbd_pad_key taps Enter's button at most
  }
  return base;
}

// One key transition (KBD_FIFO_* state).
static inline void kbd_pad_key(kbd_pad_t *p, const kbd_padmap_t *m,
                               uint8_t state, uint8_t key) {
  uint8_t mod = key == KEY_MOD_SHL   ? KBD_PADMOD_SHL
                : key == KEY_MOD_SHR ? KBD_PADMOD_SHR
                : key == KEY_MOD_ALT ? KBD_PADMOD_ALT
                                     : 0;
  if (mod) {
    kbd_pad_mod(p, m, state, mod);
    return;
  }
  key = kbd_pad_unshift(p, state, key);
  if ((p->mods & KBD_PADMOD_ALT) && state != KBD_FIFO_RELEASED &&
      kbd_key_alt_mangled(key))
    return;  // a repeat under Alt: its release will not come
  if (key == KEY_INSERT && state == KBD_FIFO_PRESSED) {
    // Shift+Enter, or I pressed within a scan of Alt going up (the
    // controller's Alt flag lags too): a tap of Enter's button, held for
    // this poll only, since the release may be I's. Enter's first repeat (a
    // HOLD Insert) then holds it quietly.
    int s = (p->mods & KBD_PADMOD_SHIFT) && !(p->mods & KBD_PADMOD_ALT)
                ? kbd_pad_slot(m, KEY_ENTER)
                : -1;
    if (s >= 0) {
      uint16_t tap = (uint16_t)(1u << (s / KBD_PAD_SLOTS));
      kbd_pad_press(p, tap);
      kbd_pad_settle(p, tap);
    }
    return;
  }
  int slot = kbd_pad_slot(m, key);
  if (slot < 0)
    return;
  uint16_t bit = (uint16_t)(1u << (slot / KBD_PAD_SLOTS));
  if (state == KBD_FIFO_PRESSED) {
    p->down |= 1u << slot;
    kbd_pad_press(p, bit);
  } else if (state == KBD_FIFO_HOLD) {
    p->down |= 1u << slot;
    kbd_pad_hold(p, bit);
  } else if (state == KBD_FIFO_RELEASED) {
    kbd_pad_up(p, 1u << slot);
  }
}

// A decoded event (the simulator's path): down = press (a repeat-flagged down
// = HOLD), up = release; char events change nothing.
static inline void kbd_pad_event(kbd_pad_t *p, const kbd_padmap_t *m,
                                 kbd_event_t e) {
  uint8_t state = KBD_FIFO_IDLE;
  if (e.type == KBD_EV_DOWN)
    state = (e.flags & KBD_EVF_REPEAT) ? KBD_FIFO_HOLD : KBD_FIFO_PRESSED;
  else if (e.type == KBD_EV_UP)
    state = KBD_FIFO_RELEASED;
  kbd_pad_key(p, m, state, e.key);
}

// The idle-dim wake swallow for the gamepad (see kbd_buttons_swallow; before
// = the pad right after kbd_poll_begin).
static inline void kbd_pad_swallow(kbd_pad_t *p, bool bg_run, kbd_pad_t before) {
  if (!bg_run) {
    p->curr &= p->prev;
    p->deferred = 0;
    return;
  }
  p->curr &= (uint16_t)(before.curr | (p->prev & ~before.prev));
  p->deferred &= before.curr;
  p->tapped &= before.curr;
}

// ── Event queue ──────────────────────────────────────────────────────────────

static inline void kbd_evq_clear(kbd_event_queue_t *q) {
  q->head = 0;
  q->count = 0;
}

// Full queue: the oldest event is dropped. Losing an old "down" leaves an
// orphan "up" (harmless); losing a new "up" would leave a key stuck.
static inline void kbd_evq_push(kbd_event_queue_t *q, kbd_event_t e) {
  if (q->count == KBD_EVENT_QUEUE_LEN) {
    q->head = (uint8_t)((q->head + 1) % KBD_EVENT_QUEUE_LEN);
    q->count--;
  }
  q->ev[(q->head + q->count) % KBD_EVENT_QUEUE_LEN] = e;
  q->count++;
}

static inline bool kbd_evq_pop(kbd_event_queue_t *q, kbd_event_t *out) {
  if (!q->count)
    return false;
  if (out)
    *out = q->ev[q->head];
  q->head = (uint8_t)((q->head + 1) % KBD_EVENT_QUEUE_LEN);
  q->count--;
  return true;
}

// Remove the newest `n` events except "up" events of keys in `seen` (keys
// the app saw go down before them); order kept. For the idle-dim wake
// swallow: an up whose down was swallowed too would be an orphan.
static inline void kbd_evq_drop_newest_keep_ups(kbd_event_queue_t *q, unsigned n,
                                                const kbd_keyset_t *seen) {
  if (n > q->count)
    n = q->count;
  unsigned first = q->count - n, out = first;
  for (unsigned i = first; i < q->count; i++) {
    kbd_event_t e = q->ev[(q->head + i) % KBD_EVENT_QUEUE_LEN];
    if (e.type == KBD_EV_UP && kbd_keyset_test(seen, e.key))
      q->ev[(q->head + out++) % KBD_EVENT_QUEUE_LEN] = e;
  }
  q->count = (uint8_t)out;
}

// ── Input state ──────────────────────────────────────────────────────────────

static inline void kbd_input_clear(kbd_input_t *in) {
  memset(in, 0, sizeof(*in));
}

static inline void kbd_input_push(kbd_input_t *in, uint8_t type, uint8_t key,
                                  char ch, uint8_t flags) {
  kbd_event_t e = {type, key, (uint8_t)ch, flags};
  kbd_evq_push(&in->q, e);
  in->ev_pushed++;
}

static inline void kbd_chars_push(kbd_input_t *in, char c) {
  if (in->char_count == KBD_CHAR_BACKLOG) {
    in->char_head = (uint8_t)((in->char_head + 1) % KBD_CHAR_BACKLOG);
    in->char_count--;
  }
  in->chars[(in->char_head + in->char_count) % KBD_CHAR_BACKLOG] = c;
  in->char_count++;
  in->char_pushed++;
}

static inline char kbd_chars_pop(kbd_input_t *in) {
  if (!in->char_count)
    return 0;
  char c = in->chars[in->char_head];
  in->char_head = (uint8_t)((in->char_head + 1) % KBD_CHAR_BACKLOG);
  in->char_count--;
  return c;
}

static inline void kbd_chars_drop_newest(kbd_input_t *in, unsigned n) {
  in->char_count = (uint8_t)(n >= in->char_count ? 0 : in->char_count - n);
}

// Record an already-decoded event (the simulator's path, and injected keys):
// keeps the key-down set in step with the queue.
static inline void kbd_input_accept(kbd_input_t *in, kbd_event_t e) {
  if (e.type == KBD_EV_DOWN)
    kbd_keyset_set(&in->down, e.key);
  else if (e.type == KBD_EV_UP)
    kbd_keyset_clear(&in->down, e.key);
  kbd_evq_push(&in->q, e);
  in->ev_pushed++;
}

// Down or up events for every bit of `bits` (injected buttons). `held` is
// the button mask after the change, for the mods byte.
static inline void kbd_input_button_events(kbd_input_t *in, uint32_t bits,
                                           uint8_t type, uint32_t held) {
  for (uint32_t b = 1; b && b <= bits; b <<= 1) {
    if (!(bits & b))
      continue;
    uint8_t key = kbd_button_to_keycode(b);
    if (!key || kbd_key_is_os_only(key))
      continue;
    if (type == KBD_EV_UP && !kbd_keyset_test(&in->down, key))
      continue;
    kbd_event_t e = {type, key, 0, kbd_mods_from_buttons(held)};
    kbd_input_accept(in, e);
  }
}

// ── Per-poll button bookkeeping ──────────────────────────────────────────────

// Start of kbd_poll: the previous state becomes prev, and a key tapped
// inside the previous poll is released now (it read as held for one poll).
static inline void kbd_buttons_begin_poll(kbd_buttons_t *b) {
  b->prev = b->curr;
  b->curr &= ~b->deferred;
  b->deferred = 0;
  b->tapped = 0;
  kbd_pad_begin_poll(&b->pad);
}

// Start of a foreground (kbd_poll, the app's input.update) or background
// (kbd_poll_background, sys.sleep) poll. *raw_key is getRawKey's value.
//
// A run of background polls must leave the app's next foreground poll the
// same edges it would have got had the FIFO items waited for it:
//  - the first background poll after a foreground one starts a poll as
//    usual (prev = the curr the app last saw, the previous poll's taps are
//    released, the raw key clears);
//  - later background polls start nothing: taps, deferred releases and the
//    raw key accumulate, and prev stays the app's last curr;
//  - the first foreground poll after them keeps all of that, so a key held
//    or tapped during the background polls reads as a press edge (a tap is
//    held for exactly this poll and released at the next one), a key
//    released meanwhile as a release edge, and the raw key of the last key
//    typed meanwhile is still there.
// Returns true when this is the first background poll of a run (the caller
// uses it for nothing else than bookkeeping it owns).
static inline bool kbd_poll_begin(kbd_buttons_t *b, bool bg, uint8_t *raw_key) {
  if (bg) {
    if (b->in_bg)
      return false;
    b->in_bg = true;
  } else if (b->in_bg) {
    b->in_bg = false;
    return false;  // keep what the background polls gathered
  }
  kbd_buttons_begin_poll(b);
  *raw_key = 0;
  return bg;
}

// Drop the fresh press edges of this poll (the key that woke the dimmed
// screen). curr_before/prev_before are the masks right after kbd_poll_begin.
// Outside a background run: every fresh press of this poll goes (curr is cut
// back to prev, pending releases dropped), as before. Inside one (a
// background poll, or the foreground poll that ends a run), prev is the curr
// the app last saw, so cutting back to it would also drop what the run's
// earlier polls gathered: only this poll's additions go. A key that became
// held quietly in this poll (HOLD of an unseen key sets curr and prev) stays.
static inline void kbd_buttons_swallow(kbd_buttons_t *b, bool bg_run,
                                       uint32_t curr_before,
                                       uint32_t prev_before) {
  if (!bg_run) {
    b->curr &= b->prev;
    b->deferred = 0;
    return;
  }
  b->curr &= curr_before | (b->prev & ~prev_before);
  b->deferred &= curr_before;
  b->tapped &= curr_before;
}

// ── STM32 FIFO decoder ───────────────────────────────────────────────────────

// Apply one FIFO item (state, keycode) in order. Updates the button masks,
// the key-down set, the event queue and the char backlog. Returns the
// keycode for getRawKey (0 when the item is not a press or repeat).
//
// PRESSED  down event (+ char event and backlog char if the key makes one);
//          a button bit becomes held with a press edge.
// HOLD     the STM32's "still held" report. For a key whose press we saw it is
//          a repeat: down + char events flagged KBD_EVF_REPEAT and a backlog
//          char (getChar has always repeated on HOLD), no new press edge.
//          For a key whose press we did not see (it predates kbd_clear_state
//          or kbd_discard_pending), the key is quietly marked held and
//          "unseen": no events, no char, and its button bit is set in prev
//          too, so no press edge. Every later HOLD of an unseen key is quiet
//          as well (the STM32 repeats HOLD), until the key is released and
//          pressed again. Only F-keys, Esc, modifiers and shifted codes
//          repeat as HOLD: arrows, letters, Enter, Tab, Backspace and Delete
//          repeat as PRESSED items, fresh presses here (a held 'y' does
//          answer ui_confirm once it is armed).
// RELEASED up event if the key was down. A button pressed during this same
//          poll stays held until the next poll (see kbd_buttons_begin_poll).
static inline uint8_t kbd_fifo_apply(kbd_input_t *in, kbd_buttons_t *b,
                                     uint8_t state, uint8_t key) {
  uint32_t btn = kbd_keycode_to_button(key);
  bool os_only = kbd_key_is_os_only(key);
  bool was_down = kbd_keyset_test(&in->down, key);

  switch (state) {
  case KBD_FIFO_PRESSED: {
    kbd_keyset_set(&in->down, key);
    kbd_keyset_clear(&in->unseen, key);
    if (btn) {
      b->curr |= btn;
      b->tapped |= btn;
      b->deferred &= ~btn;
    }
    if (!os_only) {
      uint8_t mods = kbd_mods_from_buttons(b->curr);
      kbd_input_push(in, KBD_EV_DOWN, key, 0, mods);
      char c = kbd_keycode_to_char(key, (b->curr & BTN_CTRL) != 0);
      if (c) {
        kbd_input_push(in, KBD_EV_CHAR, key, c, mods);
        kbd_chars_push(in, c);
      }
    }
    return key;
  }
  case KBD_FIFO_HOLD: {
    if (btn && !(b->curr & btn)) {
      b->curr |= btn;
      b->prev |= btn; // held, but not a fresh press edge
    }
    if (!was_down || kbd_keyset_test(&in->unseen, key)) {
      kbd_keyset_set(&in->down, key);
      kbd_keyset_set(&in->unseen, key);
      return 0;
    }
    if (!os_only) {
      uint8_t flags = kbd_mods_from_buttons(b->curr) | KBD_EVF_REPEAT;
      kbd_input_push(in, KBD_EV_DOWN, key, 0, flags);
      char c = kbd_keycode_to_char(key, (b->curr & BTN_CTRL) != 0);
      if (c) {
        kbd_input_push(in, KBD_EV_CHAR, key, c, flags);
        kbd_chars_push(in, c);
      }
    }
    return key;
  }
  case KBD_FIFO_RELEASED: {
    kbd_keyset_clear(&in->down, key);
    kbd_keyset_clear(&in->unseen, key);
    if (btn) {
      if ((b->tapped & btn) && !(b->prev & btn))
        b->deferred |= btn;
      else
        b->curr &= ~btn;
    }
    if (was_down && !os_only)
      kbd_input_push(in, KBD_EV_UP, key, 0,
                     kbd_mods_from_buttons(b->curr & ~b->deferred));
    return 0;
  }
  default:
    return 0;
  }
}

// What kbd_poll runs for each FIFO item: kbd_fifo_apply, then the gamepad.
static inline uint8_t kbd_fifo_apply_pad(kbd_input_t *in, kbd_buttons_t *b,
                                         const kbd_padmap_t *m, uint8_t state,
                                         uint8_t key) {
  uint8_t raw = kbd_fifo_apply(in, b, state, key);
  kbd_pad_key(&b->pad, m, state, key);
  return raw;
}

// ── Injected input (dev commands) ────────────────────────────────────────────
// The bookkeeping keyboard.c keeps for keypress/keydown/keyup injection, as
// pure functions over the button masks so the host test runs the same code.
//   pending  one-shot presses awaiting publication by the next kbd_poll
//   active   one-shots published and held for at least hold_ms of wall time
//   held     keys latched by keydown until kbd_release_buttons (keyup)
//   ch       a char injected with kbd_inject_char, until getChar reads it
//   pad_tap  gamepad buttons of injected chars, tapped by the next poll
// The gamepad follows injected buttons through their keys' slots
// (kbd_pad_slots_from_buttons): down while active or latched, up on retire
// or keyup.
typedef struct {
  uint32_t pending;
  uint32_t active;
  uint32_t active_since_ms;
  uint32_t held;
  char ch;
  uint16_t pad_tap;
} kbd_inject_t;

// Every kbd_poll, after kbd_poll_begin: retire the active one-shot once it
// has been held for hold_ms (a release edge), publish a pending one (a press
// edge) unless one was retired in this very poll (so a re-injected key reads
// released for at least one poll), tap the injected chars' gamepad buttons
// (held for this poll, released at the next), then fold injected keys into
// curr and the gamepad. A background poll neither retires nor publishes.
static inline void kbd_inject_poll(kbd_inject_t *j, kbd_buttons_t *b,
                                   kbd_input_t *in, const kbd_padmap_t *m,
                                   bool bg, uint32_t now_ms, uint32_t hold_ms) {
  bool retired_now = false;
  if (!bg && j->active && (now_ms - j->active_since_ms >= hold_ms)) {
    uint32_t retired = j->active & ~j->held;
    b->curr &= ~j->active;
    j->active = 0;
    retired_now = true;
    kbd_input_button_events(in, retired, KBD_EV_UP, b->curr);
    kbd_pad_up(&b->pad, kbd_pad_slots_from_buttons(m, retired));
  }
  if (!bg && !retired_now && !j->active && j->pending) {
    j->active = j->pending;
    j->pending = 0;
    j->active_since_ms = now_ms;
    kbd_input_button_events(in, j->active & ~b->curr, KBD_EV_DOWN,
                            b->curr | j->active);
  }
  if (!bg && j->pad_tap) {
    kbd_pad_press(&b->pad, j->pad_tap);
    kbd_pad_settle(&b->pad, j->pad_tap);
    j->pad_tap = 0;
  }
  b->curr |= j->active | j->held;
  uint32_t slots = kbd_pad_slots_from_buttons(m, j->active | j->held);
  b->pad.down |= slots;
  b->pad.curr |= kbd_pad_buttons_of(slots);
}

// kbd_inject_char: the gamepad button bound to the char's key (if any) is
// tapped by the next foreground poll.
static inline void kbd_inject_pad_char(kbd_inject_t *j, const kbd_padmap_t *m,
                                       char c) {
  j->pad_tap |= kbd_pad_lookup(m, (uint8_t)c);
}

// After kbd_clear_state has zeroed the masks: an injected key that is down
// (an active one-shot, or a keydown latch) stays down WITHOUT a press edge,
// as a physical key held across the clear does (the unseen-HOLD rule), and
// its retire or keyup still gives a release edge. Without this, the next
// poll ORs it back into curr with prev == 0 — a fresh press the modal that
// the injection itself opened would answer. A pending injection is left to
// be published (with its edge) by the next poll: it was queued for whatever
// is about to be shown. A char not yet read is dropped.
// The gamepad buttons of those keys likewise, and an injected char's pending
// gamepad tap goes with the char.
static inline void kbd_inject_after_clear(kbd_inject_t *j, kbd_buttons_t *b,
                                          const kbd_padmap_t *m) {
  b->curr = b->prev = j->active | j->held;
  b->pad.down = kbd_pad_slots_from_buttons(m, j->active | j->held);
  b->pad.curr = b->pad.prev = kbd_pad_buttons_of(b->pad.down);
  j->ch = 0;
  j->pad_tap = 0;
}

// Drop the one-shots (pending and active) for good: the key that woke the
// dimmed screen, or input discarded before a modal. The keydown latch stays.
static inline void kbd_inject_drop_oneshots(kbd_inject_t *j, kbd_buttons_t *b,
                                            const kbd_padmap_t *m) {
  b->curr &= ~j->active;
  kbd_pad_up(&b->pad, kbd_pad_slots_from_buttons(m, j->active & ~j->held));
  j->active = 0;
  j->active_since_ms = 0;
  j->pending = 0;
}

// keydown: latch buttons held (a down event for those not already down).
static inline void kbd_inject_hold(kbd_inject_t *j, kbd_buttons_t *b,
                                   kbd_input_t *in, uint32_t buttons) {
  kbd_input_button_events(in, buttons & ~(b->curr | j->held), KBD_EV_DOWN,
                          b->curr | j->held | buttons);
  j->held |= buttons;
}

// keyup: release latched buttons, and active/pending one-shots of the same
// keys (else the next poll's fold would resurrect them). Their gamepad
// buttons are released too, unless another of their keys is down.
static inline void kbd_inject_release(kbd_inject_t *j, kbd_buttons_t *b,
                                      kbd_input_t *in, const kbd_padmap_t *m,
                                      uint32_t buttons) {
  j->held &= ~buttons;
  j->active &= ~buttons;
  j->pending &= ~buttons;
  kbd_input_button_events(in, buttons, KBD_EV_UP, b->curr & ~buttons);
  b->curr &= ~buttons;
  kbd_pad_up(&b->pad, kbd_pad_slots_from_buttons(m, buttons));
}
