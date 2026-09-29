// Host unit tests for the gamepad layer of src/drivers/kbd_event_queue.h:
// keys resolved to PAD_* buttons through a pad map by the decode kbd_poll
// runs on the device (kbd_fifo_apply_pad), by the simulator's decoded events
// (kbd_pad_event) and by injected keys (kbd_inject_*). The keys themselves
// must keep reporting as themselves (aliasing, not remapping).
#include "check.h"
#include "kbd_event_queue.h"

static kbd_input_t in;
static kbd_buttons_t btn;
static kbd_inject_t inj;
static kbd_padmap_t map;
static uint8_t raw;

#define HOLD_MS 80

static void reset(void) {
  kbd_input_clear(&in);
  memset(&btn, 0, sizeof(btn));
  memset(&inj, 0, sizeof(inj));
  kbd_padmap_t defaults = KBD_PAD_DEFAULT_MAP;
  map = defaults;
  raw = 0;
}

// keyboard.c's kbd_clear_state.
static void clear_state(void) {
  memset(&btn, 0, sizeof(btn));
  kbd_input_clear(&in);
  kbd_inject_after_clear(&inj, &btn, &map);
}

static void poll_start(bool bg) {
  kbd_poll_begin(&btn, bg, &raw);
  in.ev_pushed = 0;
  in.char_pushed = 0;
}

// A foreground poll with its injection step, as kbd_poll_impl runs it.
static void fg_poll(uint32_t now) {
  poll_start(false);
  kbd_inject_poll(&inj, &btn, &in, &map, false, now, HOLD_MS);
}

static void apply(uint8_t state, uint8_t key) {
  kbd_fifo_apply_pad(&in, &btn, &map, state, key);
}

static uint32_t pad_pressed(void) { return btn.pad.curr & ~btn.pad.prev; }
static uint32_t pad_released(void) { return btn.pad.prev & ~btn.pad.curr; }
static uint32_t btn_pressed(void) { return btn.curr & ~btn.prev; }

static kbd_event_t pop(void) {
  kbd_event_t e = {0, 0, 0, 0};
  kbd_evq_pop(&in.q, &e);
  return e;
}

// Every default binding: press gives a press edge on its button only,
// release a release edge.
static void test_default_bindings(void) {
  static const struct {
    uint8_t key;
    uint32_t pad;
  } want[] = {
      {KEY_UP, PAD_UP},     {KEY_DOWN, PAD_DOWN},   {KEY_LEFT, PAD_LEFT},
      {KEY_RIGHT, PAD_RIGHT}, {KEY_F4, PAD_A},      {KEY_F5, PAD_B},
      {KEY_DEL, PAD_X},     {KEY_BKSPC, PAD_Y},     {KEY_F2, PAD_L},
      {KEY_F3, PAD_R},      {KEY_F1, PAD_START},    {KEY_TAB, PAD_SELECT},
  };
  for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
    reset();
    poll_start(false);
    apply(KBD_FIFO_PRESSED, want[i].key);
    CHECK_EQ_U32(pad_pressed(), want[i].pad);
    CHECK_EQ_U32(btn.pad.curr, want[i].pad);
    poll_start(false);
    CHECK_EQ_U32(pad_pressed(), 0);
    CHECK_EQ_U32(btn.pad.curr, want[i].pad);   // held
    apply(KBD_FIFO_RELEASED, want[i].key);
    CHECK_EQ_U32(pad_released(), want[i].pad);
    CHECK_EQ_U32(btn.pad.curr, 0);
  }
}

// Aliasing: the bound key still reports as itself (button bit, events,
// key-down set). The gamepad is an extra view, not a remap.
static void test_alias_keeps_the_key(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F4);
  CHECK_EQ_U32(btn_pressed(), BTN_F4);
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  CHECK(kbd_keyset_test(&in.down, KEY_F4));
  kbd_event_t e = pop();
  CHECK_EQ_INT(e.type, KBD_EV_DOWN);
  CHECK_EQ_INT(e.key, KEY_F4);
}

// Keys that are not bound, and the OS's own keys, change nothing.
static void test_unbound_and_os_keys(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'q');
  apply(KBD_FIFO_PRESSED, KEY_F6);
  apply(KBD_FIFO_PRESSED, KEY_F10);
  apply(KBD_FIFO_PRESSED, KEY_BRK);
  apply(KBD_FIFO_PRESSED, KEY_ESC);  // Esc is not a gamepad button by default
  CHECK_EQ_U32(btn.pad.curr, 0);
  CHECK_EQ_U32(kbd_pad_lookup(&map, KEY_F10), 0);
  CHECK_EQ_U32(kbd_pad_lookup(NULL, KEY_F4), 0);
  CHECK_EQ_U32(kbd_pad_lookup(&map, 0), 0);
}

// Primary and alternate slots of one button overlap cleanly: the button is
// held while either key is down and released when the last one comes up.
static void test_alternate_slot(void) {
  reset();
  map.key[4][1] = 'z';  // A = F4 / Z
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'z');
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F4);
  CHECK_EQ_U32(pad_pressed(), 0);          // already held: no second edge
  poll_start(false);
  apply(KBD_FIFO_RELEASED, 'z');
  CHECK_EQ_U32(pad_released(), 0);         // F4 still holds it
  CHECK_EQ_U32(btn.pad.curr, PAD_A);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_F4);
  CHECK_EQ_U32(pad_released(), PAD_A);
  // The alternate alone.
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'Z');            // Shift held: the STM32 sends 'Z'
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  CHECK_EQ_U32(kbd_pad_lookup(&map, 'Z'), PAD_A);
}

// A bound letter cannot stick: Shift may come up first, so the release names
// the lower-case letter after an upper-case press (case-folded as the
// key-down set is); and a press whose release never arrives is dropped by
// kbd_clear_state (system menu, app exit) without an edge afterwards.
static void test_bound_letter_releases(void) {
  reset();
  map.key[0][1] = 'w';  // Up = Up / W
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, 'W');
  CHECK_EQ_U32(pad_pressed(), PAD_UP);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  apply(KBD_FIFO_RELEASED, 'w');
  CHECK_EQ_U32(pad_released(), PAD_UP);
  CHECK_EQ_U32(btn.pad.curr, 0);
  // Pressed, release lost.
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'w');
  CHECK_EQ_U32(btn.pad.curr, PAD_UP);
  clear_state();
  poll_start(false);
  CHECK_EQ_U32(btn.pad.curr, 0);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(pad_released(), 0);
}

// A tap shorter than one poll still gives one press and one release edge:
// held (pressed) for that poll, released at the next.
static void test_tap_inside_one_poll(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F5);
  apply(KBD_FIFO_RELEASED, KEY_F5);
  CHECK_EQ_U32(pad_pressed(), PAD_B);
  CHECK_EQ_U32(btn.pad.curr, PAD_B);
  poll_start(false);
  CHECK_EQ_U32(pad_released(), PAD_B);
  CHECK_EQ_U32(btn.pad.curr, 0);
  poll_start(false);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(pad_released(), 0);
}

// Tapping the alternate while the primary is held changes nothing.
static void test_tap_while_other_key_holds(void) {
  reset();
  map.key[4][1] = 'z';
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F4);
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'z');
  apply(KBD_FIFO_RELEASED, 'z');
  CHECK_EQ_U32(pad_pressed(), 0);
  poll_start(false);
  CHECK_EQ_U32(pad_released(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_A);
}

// A key pressed before kbd_clear_state (a menu closing, an app starting) is
// held without a press edge at its next HOLD report, and released normally.
static void test_hold_after_clear_is_quiet(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_LEFT);
  clear_state();
  poll_start(false);
  CHECK_EQ_U32(btn.pad.curr, 0);           // nothing until the STM32 reports
  apply(KBD_FIFO_HOLD, KEY_LEFT);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_LEFT);
  apply(KBD_FIFO_HOLD, KEY_LEFT);          // the STM32 may repeat HOLD
  poll_start(false);
  CHECK_EQ_U32(pad_pressed(), 0);
  apply(KBD_FIFO_RELEASED, KEY_LEFT);
  CHECK_EQ_U32(pad_released(), PAD_LEFT);
  // A HOLD of a key whose press was seen is only a repeat.
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_RIGHT);
  poll_start(false);
  apply(KBD_FIFO_HOLD, KEY_RIGHT);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_RIGHT);
}

// sys.sleep's background polls: a tap during the sleep reaches the app's
// next foreground poll as a press, and the one after as a release.
static void test_background_tap(void) {
  reset();
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_TAB);
  poll_start(true);
  apply(KBD_FIFO_RELEASED, KEY_TAB);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(pad_pressed(), PAD_SELECT);
  poll_start(false);
  CHECK_EQ_U32(pad_released(), PAD_SELECT);
}

// The key that wakes the dimmed screen is swallowed from the gamepad too; a
// key an earlier poll of a background run gathered still arrives.
static void test_wake_swallow(void) {
  reset();
  poll_start(false);
  kbd_pad_t before = btn.pad;
  apply(KBD_FIFO_PRESSED, KEY_F4);
  kbd_pad_swallow(&btn.pad, false, before);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, 0);

  reset();
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_F1);         // earlier poll of the run
  apply(KBD_FIFO_RELEASED, KEY_F1);
  poll_start(true);
  before = btn.pad;
  apply(KBD_FIFO_PRESSED, KEY_F2);         // this poll's key wakes the screen
  kbd_pad_swallow(&btn.pad, true, before);
  poll_start(false);
  CHECK_EQ_U32(pad_pressed(), PAD_START);
  CHECK_EQ_U32(btn.pad.curr, PAD_START);
}

// The simulator's path: decoded events drive the same masks.
static void test_decoded_events(void) {
  reset();
  kbd_pad_t p = {0, 0, 0, 0};
  kbd_event_t down = {KBD_EV_DOWN, KEY_UP, 0, 0};
  kbd_event_t up = {KBD_EV_UP, KEY_UP, 0, 0};
  kbd_event_t ch = {KBD_EV_CHAR, 'w', 'w', 0};
  kbd_pad_begin_poll(&p);
  kbd_input_accept(&in, down);
  kbd_pad_event(&p, &map, &in.down, down);
  kbd_input_accept(&in, up);
  kbd_pad_event(&p, &map, &in.down, up);  // a tap inside one poll
  CHECK_EQ_U32(p.curr & ~p.prev, PAD_UP);
  kbd_pad_begin_poll(&p);
  CHECK_EQ_U32(p.prev & ~p.curr, PAD_UP);
  map.key[0][1] = 'w';
  kbd_pad_event(&p, &map, &in.down, ch);  // chars change nothing
  CHECK_EQ_U32(p.curr, 0);
  // A repeat-flagged down (SDL auto-repeat after a clear) is a quiet hold.
  kbd_event_t rep = {KBD_EV_DOWN, KEY_UP, 0, KBD_EVF_REPEAT};
  kbd_pad_begin_poll(&p);
  kbd_input_accept(&in, rep);
  kbd_pad_event(&p, &map, &in.down, rep);
  CHECK_EQ_U32(p.curr, PAD_UP);
  CHECK_EQ_U32(p.curr & ~p.prev, 0);
}

// Injected buttons (dev keypress, MCP): a one-shot lights the bound button
// with a press edge when published and releases it when retired.
static void test_injected_button(void) {
  reset();
  fg_poll(0);
  inj.pending |= BTN_F4;
  fg_poll(10);
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  CHECK_EQ_U32(btn_pressed(), BTN_F4);     // and still BTN_F4
  fg_poll(40);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_A);
  fg_poll(10 + HOLD_MS);
  CHECK_EQ_U32(pad_released(), PAD_A);
  CHECK_EQ_U32(btn.pad.curr, 0);
  fg_poll(200);
  CHECK_EQ_U32(btn.pad.curr, 0);
  // A background poll neither publishes nor retires.
  inj.pending |= BTN_UP;
  poll_start(true);
  kbd_inject_poll(&inj, &btn, &in, &map, true, 300, HOLD_MS);
  CHECK_EQ_U32(btn.pad.curr, 0);
  fg_poll(310);
  CHECK_EQ_U32(pad_pressed(), PAD_UP);
}

// An injected one-shot still active at kbd_clear_state stays held without an
// edge and is released when it retires.
static void test_injected_button_across_clear(void) {
  reset();
  inj.pending |= BTN_TAB;
  fg_poll(1000);
  CHECK_EQ_U32(pad_pressed(), PAD_SELECT);
  clear_state();
  fg_poll(1010);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_SELECT);
  fg_poll(1000 + HOLD_MS);
  CHECK_EQ_U32(pad_released(), PAD_SELECT);
  CHECK_EQ_U32(btn.pad.curr, 0);
}

// keydown / keyup latches: pressed at the next poll, released at once.
static void test_injected_latch(void) {
  reset();
  fg_poll(0);
  kbd_inject_hold(&inj, &btn, &in, BTN_DEL);
  fg_poll(10);
  CHECK_EQ_U32(pad_pressed(), PAD_X);
  fg_poll(500);
  CHECK_EQ_U32(btn.pad.curr, PAD_X);       // a latch never retires
  kbd_inject_release(&inj, &btn, &in, &map, BTN_DEL);
  CHECK_EQ_U32(pad_released(), PAD_X);
  fg_poll(510);
  CHECK_EQ_U32(btn.pad.curr, 0);           // not resurrected
}

// Injected chars (letters have no BTN_*): the bound button is tapped by the
// next foreground poll, then released.
static void test_injected_char(void) {
  reset();
  map.key[0][1] = 'w';
  fg_poll(0);
  kbd_inject_pad_char(&inj, &map, 'W');
  kbd_inject_pad_char(&inj, &map, 'q');    // unbound: nothing
  poll_start(true);                        // sys.sleep: not yet
  kbd_inject_poll(&inj, &btn, &in, &map, true, 5, HOLD_MS);
  CHECK_EQ_U32(btn.pad.curr, 0);
  fg_poll(10);
  CHECK_EQ_U32(pad_pressed(), PAD_UP);
  fg_poll(20);
  CHECK_EQ_U32(pad_released(), PAD_UP);
  fg_poll(30);
  CHECK_EQ_U32(btn.pad.curr, 0);
  // Dropped by a clear, like the char itself.
  kbd_inject_pad_char(&inj, &map, 'w');
  clear_state();
  fg_poll(40);
  CHECK_EQ_U32(btn.pad.curr, 0);
}

// Dropping the one-shots (a modal's kbd_discard_pending) drops their gamepad
// button with them.
static void test_drop_oneshots(void) {
  reset();
  inj.pending |= BTN_F3;
  fg_poll(0);
  CHECK_EQ_U32(btn.pad.curr, PAD_R);
  kbd_inject_drop_oneshots(&inj, &btn, &map);
  clear_state();
  fg_poll(10);
  CHECK_EQ_U32(btn.pad.curr, 0);
}

// A BTN_* mask resolves through the keycodes the buttons send.
static void test_pad_from_buttons(void) {
  reset();
  CHECK_EQ_U32(kbd_pad_from_buttons(&map, BTN_UP | BTN_F4 | BTN_ENTER),
               PAD_UP | PAD_A);
  CHECK_EQ_U32(kbd_pad_from_buttons(&map, 0), 0);
  CHECK_EQ_U32(kbd_pad_from_buttons(NULL, BTN_UP), 0);
}

int main(void) {
  test_default_bindings();
  test_alias_keeps_the_key();
  test_unbound_and_os_keys();
  test_alternate_slot();
  test_bound_letter_releases();
  test_tap_inside_one_poll();
  test_tap_while_other_key_holds();
  test_hold_after_clear_is_quiet();
  test_background_tap();
  test_wake_swallow();
  test_decoded_events();
  test_injected_button();
  test_injected_button_across_clear();
  test_injected_latch();
  test_injected_char();
  test_drop_oneshots();
  test_pad_from_buttons();
  return check_report("test_kbd_pad");
}
