// Pad sources: whole-state reports ORed into the logical gamepad. See
// pad_source.h for the contract; this file has no hardware dependency (host
// tests: tests/unit/test_pad_source.c).
#include "pad_source.h"
#include "../os/os.h" // PAD_*, BTN_*

#include <stdatomic.h>

// Marks a published state: the source is connected. Never a PAD_* bit.
#define PAD_SOURCE_LIVE (1u << 31)
#define PAD_SOURCE_BITS (PAD_SOURCE_BUTTONS | PAD_SOURCE_HOME)

_Static_assert((PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A | PAD_B |
                PAD_X | PAD_Y | PAD_L | PAD_R | PAD_START | PAD_SELECT) ==
                   PAD_SOURCE_BUTTONS,
               "PAD_SOURCE_BUTTONS covers every PAD_* button");

typedef struct {
  // Written by the producer, read (state) and taken (latch) by Core 0.
  _Atomic uint32_t state; // PAD_* | HOME | LIVE; 0 = disconnected
  _Atomic uint32_t latch; // PAD_* | HOME that went down since the last take
  // The producer's own: its previous publish, for the rising edges.
  uint32_t last;
} pad_slot_t;

static pad_slot_t s_slot[PAD_SOURCE_COUNT];

// ── Core 0 (consumer) state ─────────────────────────────────────────────────
static uint16_t s_curr, s_prev; // the sources' combined PAD_* mask
static uint16_t s_quiet;        // held without a press edge (wake swallow)
static uint16_t s_fresh;        // the last poll's new presses
static uint16_t s_acc;          // presses gathered by background polls
static bool s_fresh_bg;         // s_fresh came from a background poll
// The test source's timed hold: armed by pad_source_test_set, started by the
// next foreground poll, released once it has run out.
static bool s_test_armed, s_test_running;
static uint32_t s_test_hold_ms, s_test_since_ms;

// ── Producer side ───────────────────────────────────────────────────────────

void pad_source_publish(pad_source_id_t id, uint32_t state) {
  if ((unsigned)id >= PAD_SOURCE_COUNT)
    return;
  pad_slot_t *s = &s_slot[id];
  state = (state & PAD_SOURCE_BITS) | PAD_SOURCE_LIVE;
  uint32_t rising = state & ~s->last & PAD_SOURCE_BITS;
  s->last = state;
  // The state first, then the latch: Core 0 loads the state before it takes
  // the latch, so a press it saw held and then released cannot come back
  // from the latch as a second press (pad_sources_poll).
  atomic_store_explicit(&s->state, state, memory_order_release);
  if (rising)
    atomic_fetch_or_explicit(&s->latch, rising, memory_order_release);
}

void pad_source_disconnect(pad_source_id_t id) {
  if ((unsigned)id >= PAD_SOURCE_COUNT)
    return;
  s_slot[id].last = 0;
  atomic_store_explicit(&s_slot[id].state, 0, memory_order_release);
}

bool pad_source_is_connected(pad_source_id_t id) {
  if ((unsigned)id >= PAD_SOURCE_COUNT)
    return false;
  return (atomic_load_explicit(&s_slot[id].state, memory_order_acquire) &
          PAD_SOURCE_LIVE) != 0;
}

uint32_t pad_sources_connected(void) {
  uint32_t mask = 0;
  for (int i = 0; i < PAD_SOURCE_COUNT; i++)
    if (pad_source_is_connected((pad_source_id_t)i))
      mask |= 1u << i;
  return mask;
}

// ── The test source ─────────────────────────────────────────────────────────

void pad_source_test_set(uint32_t state, uint32_t hold_ms) {
  pad_source_publish(PAD_SOURCE_TEST, state);
  s_test_armed = hold_ms > 0;
  s_test_running = false;
  s_test_hold_ms = hold_ms;
}

void pad_source_test_off(void) {
  s_test_armed = s_test_running = false;
  pad_source_disconnect(PAD_SOURCE_TEST);
}

// Start or end the timed hold (a foreground poll, before sampling).
static void test_hold_poll(uint32_t now_ms) {
  if (s_test_armed) {
    s_test_armed = false;
    s_test_running = true;
    s_test_since_ms = now_ms;
  } else if (s_test_running &&
             (uint32_t)(now_ms - s_test_since_ms) >= s_test_hold_ms) {
    s_test_running = false;
    if (pad_source_is_connected(PAD_SOURCE_TEST))
      pad_source_publish(PAD_SOURCE_TEST, 0);
  }
}

// ── Consumer side ───────────────────────────────────────────────────────────

// What every source holds now, and what went down since the last take. The
// state is loaded before the latch is taken (see pad_source_publish).
static void sample(uint32_t *held, uint32_t *taps) {
  uint32_t h = 0, t = 0;
  for (int i = 0; i < PAD_SOURCE_COUNT; i++) {
    uint32_t st = atomic_load_explicit(&s_slot[i].state, memory_order_acquire);
    t |= atomic_exchange_explicit(&s_slot[i].latch, 0, memory_order_acq_rel);
    if (st & PAD_SOURCE_LIVE)
      h |= st;
  }
  *held = h;
  *taps = t;
}

bool pad_sources_poll(bool bg, uint32_t now_ms) {
  if (!bg)
    test_hold_poll(now_ms);
  uint32_t held, taps;
  sample(&held, &taps);
  bool home = (taps & PAD_SOURCE_HOME) != 0;
  if (bg) {
    // Gathered for the app's next foreground poll; Home acts at once.
    s_fresh = (uint16_t)(taps & PAD_SOURCE_BUTTONS & ~s_acc);
    s_fresh_bg = true;
    s_acc |= (uint16_t)(taps & PAD_SOURCE_BUTTONS);
    return home;
  }
  taps |= s_acc;
  s_acc = 0;
  s_prev = s_curr;
  s_curr = (uint16_t)((held | taps) & PAD_SOURCE_BUTTONS);
  // A swallowed press still held reads held, with no edge; it stops being
  // quiet once released.
  s_quiet &= (uint16_t)held;
  s_prev |= (uint16_t)(s_curr & s_quiet);
  s_fresh = (uint16_t)(s_curr & ~s_prev);
  s_fresh_bg = false;
  return home;
}

uint32_t pad_sources_fresh(void) { return s_fresh; }

void pad_sources_swallow(void) {
  s_quiet |= s_fresh;
  if (s_fresh_bg)
    s_acc &= (uint16_t)~s_fresh;
  else
    s_curr &= (uint16_t)~s_fresh;
  s_fresh = 0;
}

void pad_sources_clear(void) {
  uint32_t held = 0;
  for (int i = 0; i < PAD_SOURCE_COUNT; i++) {
    uint32_t st = atomic_load_explicit(&s_slot[i].state, memory_order_acquire);
    if (st & PAD_SOURCE_LIVE)
      held |= st;
  }
  // Held and already seen: stays held, no edge. The latch keeps presses not
  // polled yet (the next poll gives them their edge), as the keyboard's ring
  // keeps its items across a clear. What background polls gathered goes, as
  // the keyboard's does (the clear ends the run).
  s_curr &= (uint16_t)held;
  s_prev = s_curr;
  s_acc = 0;
  s_fresh = 0;
  s_quiet &= (uint16_t)held;
}

void pad_sources_discard(void) {
  uint32_t held, taps;
  sample(&held, &taps); // taps dropped, Home included
  s_acc = 0;
  s_curr = s_prev = (uint16_t)(held & PAD_SOURCE_BUTTONS);
  s_quiet = s_curr;
  s_fresh = 0;
}

uint32_t pad_sources_combine_held(uint16_t kbd_curr) {
  return (uint32_t)(kbd_curr | s_curr);
}

uint32_t pad_sources_combine_pressed(uint16_t kbd_curr, uint16_t kbd_prev) {
  return (uint32_t)((kbd_curr | s_curr) & ~(kbd_prev | s_prev)) & 0xFFFFu;
}

uint32_t pad_sources_combine_released(uint16_t kbd_curr, uint16_t kbd_prev) {
  return (uint32_t)((kbd_prev | s_prev) & ~(kbd_curr | s_curr)) & 0xFFFFu;
}

uint32_t pad_sources_pressed(void) {
  return (uint32_t)(s_curr & ~s_prev) & 0xFFFFu;
}

uint32_t pad_nav_buttons(uint32_t pad) {
  uint32_t btn = 0;
  if (pad & PAD_UP)
    btn |= BTN_UP;
  if (pad & PAD_DOWN)
    btn |= BTN_DOWN;
  if (pad & PAD_LEFT)
    btn |= BTN_LEFT;
  if (pad & PAD_RIGHT)
    btn |= BTN_RIGHT;
  if (pad & PAD_A)
    btn |= BTN_ENTER;
  if (pad & PAD_B)
    btn |= BTN_ESC;
  return btn;
}

void pad_sources_reset(void) {
  for (int i = 0; i < PAD_SOURCE_COUNT; i++) {
    atomic_store(&s_slot[i].state, 0);
    atomic_store(&s_slot[i].latch, 0);
    s_slot[i].last = 0;
  }
  s_curr = s_prev = s_quiet = s_fresh = s_acc = 0;
  s_fresh_bg = false;
  s_test_armed = s_test_running = false;
  s_test_hold_ms = s_test_since_ms = 0;
}
