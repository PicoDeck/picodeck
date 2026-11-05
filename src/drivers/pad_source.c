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

_Static_assert(AXIS_LX == 0 && AXIS_LY == 1 && AXIS_RX == 2 && AXIS_RY == 3 &&
                   AXIS_LT == 4 && AXIS_RT == 5,
               "the axis words are indexed by AXIS_*");

// The axes' shape, in the units of pad_axes_t (127 and 255 are full).
#define STICK_INNER 15    // 11.8%: at or under this the stick is at rest
#define STICK_OUTER 121   // 95.3%: from here on it reads full
#define TRIGGER_INNER 8   // 3.1%
#define TRIGGER_OUTER 247 // 96.9%

typedef struct {
  // Written by the producer, read (state) and taken (latch) by Core 0.
  _Atomic uint32_t state; // PAD_* | HOME | LIVE; 0 = disconnected
  _Atomic uint32_t latch; // PAD_* | HOME that went down since the last take
  // Written by the producer, read by Core 0, after the shape: 0 at rest.
  _Atomic uint32_t sticks;   // int8 lx | ly << 8 | rx << 16 | ry << 24
  _Atomic uint32_t triggers; // uint8 lt | rt << 8
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
// What getAxis reads: the sources' axis words combined at the last poll.
static uint32_t s_sticks;   // as pad_slot_t.sticks
static uint16_t s_triggers; // as pad_slot_t.triggers
static bool s_axes_moved;   // the last poll changed them

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

static uint32_t isqrt(uint32_t v) {
  uint32_t r = 0;
  for (uint32_t bit = 1u << 7; bit; bit >>= 1)
    if ((r | bit) * (r | bit) <= v)
      r |= bit;
  return r;
}

static int div_round(int n, int d) { // d > 0
  return (n >= 0 ? n + d / 2 : n - d / 2) / d;
}

// One stick into one half of a word: its distance from the centre is rescaled,
// its direction kept. A rescaled length of 1 or more comes out nonzero (the
// larger component is at least 0.7 of it), so 0 on both axes means at rest,
// which is what the D-pad fallback asks.
static uint32_t stick_shape(int x, int y) {
  uint32_t d2 = (uint32_t)(x * x + y * y);
  if (d2 <= STICK_INNER * STICK_INNER)
    return 0;
  int d = (int)isqrt(d2); // 15..181: no component exceeds it
  int len = 127;
  if (d < STICK_OUTER)
    len = div_round((d - STICK_INNER) * 127, STICK_OUTER - STICK_INNER);
  x = div_round(x * len, d);
  y = div_round(y * len, d);
  return (uint32_t)(uint8_t)x | (uint32_t)(uint8_t)y << 8;
}

static uint32_t trigger_shape(int v) {
  if (v <= TRIGGER_INNER)
    return 0;
  if (v >= TRIGGER_OUTER)
    return 255;
  return (uint32_t)div_round((v - TRIGGER_INNER) * 255,
                             TRIGGER_OUTER - TRIGGER_INNER);
}

void pad_source_publish_axes(pad_source_id_t id, const pad_axes_t *a) {
  if ((unsigned)id >= PAD_SOURCE_COUNT)
    return;
  pad_slot_t *s = &s_slot[id];
  atomic_store_explicit(&s->sticks,
                        stick_shape(a->lx, a->ly) |
                            stick_shape(a->rx, a->ry) << 16,
                        memory_order_release);
  atomic_store_explicit(&s->triggers,
                        trigger_shape(a->lt) | trigger_shape(a->rt) << 8,
                        memory_order_release);
}

static void axes_zero(pad_slot_t *s) {
  atomic_store_explicit(&s->sticks, 0, memory_order_release);
  atomic_store_explicit(&s->triggers, 0, memory_order_release);
}

void pad_source_disconnect(pad_source_id_t id) {
  if ((unsigned)id >= PAD_SOURCE_COUNT)
    return;
  s_slot[id].last = 0;
  atomic_store_explicit(&s_slot[id].state, 0, memory_order_release);
  axes_zero(&s_slot[id]);
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
    if (pad_source_is_connected(PAD_SOURCE_TEST)) {
      pad_source_publish(PAD_SOURCE_TEST, 0);
      axes_zero(&s_slot[PAD_SOURCE_TEST]);
    }
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

// Each axis's largest magnitude among the sources. A stick at rest is 0 on both
// its axes, so it never beats one in use. Nearly always every word is 0.
static void axes_poll(void) {
  uint32_t sticks = 0, triggers = 0;
  for (int i = 0; i < PAD_SOURCE_COUNT; i++) {
    uint32_t w = atomic_load_explicit(&s_slot[i].sticks, memory_order_acquire);
    for (int shift = 0; shift < 32 && (w >> shift); shift += 8) {
      int v = (int8_t)(w >> shift), best = (int8_t)(sticks >> shift);
      if (v * v > best * best)
        sticks = (sticks & ~(0xFFu << shift)) | (uint32_t)(uint8_t)v << shift;
    }
    w = atomic_load_explicit(&s_slot[i].triggers, memory_order_acquire);
    if ((w & 0xFFu) > (triggers & 0xFFu))
      triggers = (triggers & 0xFF00u) | (w & 0xFFu);
    if ((w & 0xFF00u) > (triggers & 0xFF00u))
      triggers = (triggers & 0xFFu) | (w & 0xFF00u);
  }
  s_axes_moved = sticks != s_sticks || triggers != s_triggers;
  s_sticks = sticks;
  s_triggers = (uint16_t)triggers;
}

bool pad_sources_poll(bool bg, uint32_t now_ms) {
  if (!bg)
    test_hold_poll(now_ms);
  axes_poll();
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

float pad_sources_axis(int axis, uint32_t held) {
  switch (axis) {
  case AXIS_LX:
    if (!(s_sticks & 0xFFFFu))
      return (float)(!!(held & PAD_RIGHT) - !!(held & PAD_LEFT));
    break;
  case AXIS_LY:
    if (!(s_sticks & 0xFFFFu))
      return (float)(!!(held & PAD_DOWN) - !!(held & PAD_UP));
    break;
  case AXIS_RX:
  case AXIS_RY:
    break;
  case AXIS_LT:
  case AXIS_RT:
    return (float)(uint8_t)(s_triggers >> (8 * (axis - AXIS_LT))) / 255.0f;
  default:
    return 0.0f;
  }
  return (float)(int8_t)(s_sticks >> (8 * axis)) / 127.0f;
}

bool pad_sources_axes_moved(void) { return s_axes_moved; }

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
    axes_zero(&s_slot[i]);
    s_slot[i].last = 0;
  }
  s_curr = s_prev = s_quiet = s_fresh = s_acc = 0;
  s_fresh_bg = false;
  s_sticks = 0;
  s_triggers = 0;
  s_axes_moved = false;
  s_test_armed = s_test_running = false;
  s_test_hold_ms = s_test_since_ms = 0;
}
