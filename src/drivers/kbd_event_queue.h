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

// Button masks as kbd_poll maintains them.
typedef struct {
  uint32_t curr;     // held now
  uint32_t prev;     // held at the previous poll
  uint32_t tapped;   // got a fresh PRESSED during this poll
  uint32_t deferred; // pressed and released inside this poll: released next
  bool in_bg;        // background polls ran since the last foreground poll
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
//          as well (the STM32 may repeat HOLD), until the key is released and
//          pressed again — a held 'y' can never answer ui_confirm.
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

// ── Injected input (dev commands) ────────────────────────────────────────────
// The bookkeeping keyboard.c keeps for keypress/keydown/keyup injection, as
// pure functions over the button masks so the host test runs the same code.
//   pending  one-shot presses awaiting publication by the next kbd_poll
//   active   one-shots published and held for at least hold_ms of wall time
//   held     keys latched by keydown until kbd_release_buttons (keyup)
//   ch       a char injected with kbd_inject_char, until getChar reads it
typedef struct {
  uint32_t pending;
  uint32_t active;
  uint32_t active_since_ms;
  uint32_t held;
  char ch;
} kbd_inject_t;

// Every kbd_poll, after kbd_poll_begin: retire the active one-shot once it
// has been held for hold_ms (a release edge), publish a pending one (a press
// edge) unless one was retired in this very poll (so a re-injected key reads
// released for at least one poll), then fold injected keys into curr.
// A background poll neither retires nor publishes.
static inline void kbd_inject_poll(kbd_inject_t *j, kbd_buttons_t *b,
                                   kbd_input_t *in, bool bg, uint32_t now_ms,
                                   uint32_t hold_ms) {
  bool retired_now = false;
  if (!bg && j->active && (now_ms - j->active_since_ms >= hold_ms)) {
    uint32_t retired = j->active & ~j->held;
    b->curr &= ~j->active;
    j->active = 0;
    retired_now = true;
    kbd_input_button_events(in, retired, KBD_EV_UP, b->curr);
  }
  if (!bg && !retired_now && !j->active && j->pending) {
    j->active = j->pending;
    j->pending = 0;
    j->active_since_ms = now_ms;
    kbd_input_button_events(in, j->active & ~b->curr, KBD_EV_DOWN,
                            b->curr | j->active);
  }
  b->curr |= j->active | j->held;
}

// After kbd_clear_state has zeroed the masks: an injected key that is down
// (an active one-shot, or a keydown latch) stays down WITHOUT a press edge,
// as a physical key held across the clear does (the unseen-HOLD rule), and
// its retire or keyup still gives a release edge. Without this, the next
// poll ORs it back into curr with prev == 0 — a fresh press the modal that
// the injection itself opened would answer. A pending injection is left to
// be published (with its edge) by the next poll: it was queued for whatever
// is about to be shown. A char not yet read is dropped.
static inline void kbd_inject_after_clear(kbd_inject_t *j, kbd_buttons_t *b) {
  b->curr = b->prev = j->active | j->held;
  j->ch = 0;
}

// Drop the one-shots (pending and active) for good: the key that woke the
// dimmed screen, or input discarded before a modal. The keydown latch stays.
static inline void kbd_inject_drop_oneshots(kbd_inject_t *j, kbd_buttons_t *b) {
  b->curr &= ~j->active;
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
// keys (else the next poll's fold would resurrect them).
static inline void kbd_inject_release(kbd_inject_t *j, kbd_buttons_t *b,
                                      kbd_input_t *in, uint32_t buttons) {
  j->held &= ~buttons;
  j->active &= ~buttons;
  j->pending &= ~buttons;
  kbd_input_button_events(in, buttons, KBD_EV_UP, b->curr & ~buttons);
  b->curr &= ~buttons;
}
