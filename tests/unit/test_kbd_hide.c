// Host unit tests for the hidden keys of src/drivers/kbd_event_queue.h: what is
// down when the system menu opens or closes stays out of every app-facing view,
// each key until it is released (issue #60). Each test replays the items the
// keyboard controller reports for a physical key (picocalc_keyboard
// keyboard.ino: PRESSED, then repeats every 100 ms after 300 ms, as HOLD for
// F-keys, Esc and modifiers and as PRESSED for arrows, letters, Enter, Tab,
// Backspace and Delete) through the steps kbd_poll_impl runs.
#include "check.h"
#include "kbd_event_queue.h"

#define HOLD_MS 80

static kbd_input_t in;
static kbd_buttons_t btn;
static kbd_inject_t inj;
static kbd_padmap_t map;
static kbd_hide_t hide;
static uint8_t raw;
static uint32_t now;

static void reset(void) {
  kbd_input_clear(&in);
  memset(&btn, 0, sizeof(btn));
  memset(&inj, 0, sizeof(inj));
  memset(&hide, 0, sizeof(hide));
  kbd_padmap_t defaults = KBD_PAD_DEFAULT_MAP;
  map = defaults;
  raw = 0;
  now = 1000;
}

// keyboard.c's kbd_clear_state and kbd_hide_held.
static void clear_state(void) {
  memset(&btn, 0, sizeof(btn));
  kbd_input_clear(&in);
  raw = 0;
  kbd_inject_after_clear(&inj, &btn, &map);
}

static void hide_held(void) {
  kbd_hide_add_down(&hide, &in.down, inj.active | inj.held);
  kbd_inject_hide(&inj);
  clear_state();
}

// One poll of kbd_poll_impl with these items, `ms` after the last one.
typedef struct {
  uint8_t state, key;
} item_t;

static void poll_items(uint32_t ms, bool bg, const item_t *items, int n) {
  now += ms;
  kbd_poll_begin(&btn, bg, &raw);
  in.ev_pushed = 0;
  in.char_pushed = 0;
  kbd_inject_poll(&inj, &btn, &in, &map, bg, now, HOLD_MS);
  for (int i = 0; i < n; i++) {
    if (kbd_hide_item(&hide, &btn.pad, &map, items[i].state, items[i].key))
      continue;
    uint8_t r = kbd_fifo_apply_pad(&in, &btn, &map, items[i].state, items[i].key);
    if (r)
      raw = r;
  }
  kbd_hide_tick(&hide, now);
}

static void poll_one(uint32_t ms, uint8_t state, uint8_t key) {
  item_t it = {state, key};
  poll_items(ms, false, &it, 1);
}

static void poll_none(uint32_t ms) { poll_items(ms, false, NULL, 0); }

static uint32_t pressed(void) { return btn.curr & ~btn.prev; }
static uint32_t released(void) { return btn.prev & ~btn.curr; }
static uint32_t pad_pressed(void) { return btn.pad.curr & ~btn.pad.prev; }
static uint32_t pad_released(void) { return btn.pad.prev & ~btn.pad.curr; }

// The app sees nothing at all of any key this poll.
#define CHECK_NOTHING() do { \
  CHECK_EQ_U32(btn.curr, 0); CHECK_EQ_U32(pressed(), 0); \
  CHECK_EQ_U32(released(), 0); CHECK_EQ_U32(btn.pad.curr, 0); \
  CHECK_EQ_U32(pad_pressed(), 0); CHECK_EQ_U32(pad_released(), 0); \
  CHECK_EQ_INT(in.q.count, 0); CHECK_EQ_INT(in.char_count, 0); \
  CHECK_EQ_INT(raw, 0); \
  for (int _w = 0; _w < 8; _w++) CHECK_EQ_U32(in.down.bits[_w], 0); \
} while (0)

// The key presses and is read in the menu; the menu closes with it down.
static void press_in_menu(uint8_t key) {
  poll_one(16, KBD_FIFO_PRESSED, key);
  hide_held();
}

// Esc closes the menu and is held on: its first HOLD comes after 300 ms, then
// one every 100 ms. None of them reads as anything, however long it goes on.
// Its RELEASED is silent too, and the next press is a press with all it gives.
static void test_held_esc_repeats_as_hold(void) {
  reset();
  press_in_menu(KEY_ESC);
  CHECK_NOTHING();
  for (int i = 0; i < 18; i++) {  // the 300 ms to the first HOLD
    poll_none(16);
    CHECK_NOTHING();
  }
  for (int i = 0; i < 40; i++) {
    if (i % 6 == 0)
      poll_one(16, KBD_FIFO_HOLD, KEY_ESC);
    else
      poll_none(16);
    CHECK_NOTHING();
  }
  poll_one(16, KBD_FIFO_RELEASED, KEY_ESC);
  CHECK_NOTHING();
  poll_none(16);
  CHECK_NOTHING();
  CHECK_EQ_INT(hide.count, 0);

  poll_one(16, KBD_FIFO_PRESSED, KEY_ESC);
  CHECK_EQ_U32(btn.curr, BTN_ESC);
  CHECK_EQ_U32(pressed(), BTN_ESC);
  CHECK(kbd_keyset_test(&in.down, KEY_ESC));
  CHECK_EQ_INT(in.q.count, 1);
  CHECK_EQ_INT(raw, KEY_ESC);
  // Its own HOLD is now a repeat of a press the app saw, as ever.
  kbd_event_t e;
  CHECK(kbd_evq_pop(&in.q, &e));
  CHECK_EQ_INT(e.type, KBD_EV_DOWN);
  poll_one(300, KBD_FIFO_HOLD, KEY_ESC);
  CHECK_EQ_U32(pressed(), 0);
  CHECK(kbd_evq_pop(&in.q, &e));
  CHECK(e.flags & KBD_EVF_REPEAT);
  poll_one(16, KBD_FIFO_RELEASED, KEY_ESC);
  CHECK_EQ_U32(released(), BTN_ESC);
}

// An arrow repeats as fresh PRESSED items, with its gamepad button: the bug at
// its worst, each one a press edge. Hidden, they read as nothing.
static void test_held_arrow_repeats_as_pressed(void) {
  reset();
  press_in_menu(KEY_UP);
  for (int i = 0; i < 12; i++) {
    poll_one(100, KBD_FIFO_PRESSED, KEY_UP);
    CHECK_NOTHING();
  }
  poll_one(16, KBD_FIFO_RELEASED, KEY_UP);
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_PRESSED, KEY_UP);
  CHECK_EQ_U32(pressed(), BTN_UP);
  CHECK_EQ_U32(pad_pressed(), PAD_UP);
  poll_one(16, KBD_FIFO_RELEASED, KEY_UP);
  CHECK_EQ_U32(released(), BTN_UP);
  CHECK_EQ_U32(pad_released(), PAD_UP);
}

// A repeat and the release in one poll, or in one background run: the decode
// never sees the repeat, so no edge or char of it is left when the release ends
// the hiding. (The simulator's HAL keeps edges and chars apart from its events
// and has to take them back: keyboard_stub.c, test_menu_key_leak.py.)
static void test_repeat_and_release_together(void) {
  static const uint8_t keys[] = {KEY_UP, KEY_ENTER, 'w', KEY_ESC};
  for (unsigned k = 0; k < sizeof(keys); k++) {
    uint8_t rep = keys[k] == KEY_ESC ? KBD_FIFO_HOLD : KBD_FIFO_PRESSED;
    item_t both[] = {{rep, keys[k]}, {KBD_FIFO_RELEASED, keys[k]}};
    reset();
    press_in_menu(keys[k]);
    poll_items(16, false, both, 2);
    CHECK_NOTHING();
    poll_none(16);
    CHECK_NOTHING();

    reset();
    press_in_menu(keys[k]);
    poll_items(10, true, &both[0], 1);
    poll_items(10, true, &both[1], 1);
    CHECK_NOTHING();
    poll_none(10);
    CHECK_NOTHING();
    poll_none(10);
    CHECK_NOTHING();
  }
}

// A letter's repeats give chars; so does Enter, for getChar.
static void test_held_keys_give_no_chars(void) {
  reset();
  poll_one(16, KBD_FIFO_PRESSED, 'y');
  poll_one(16, KBD_FIFO_PRESSED, KEY_ENTER);
  hide_held();
  CHECK_EQ_INT(hide.count, 2);
  item_t both[] = {{KBD_FIFO_PRESSED, 'y'}, {KBD_FIFO_PRESSED, KEY_ENTER}};
  for (int i = 0; i < 5; i++) {
    poll_items(100, false, both, 2);
    CHECK_NOTHING();
  }
  item_t up[] = {{KBD_FIFO_RELEASED, 'y'}, {KBD_FIFO_RELEASED, KEY_ENTER}};
  poll_items(16, false, up, 2);
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_PRESSED, 'y');
  CHECK_EQ_INT(kbd_chars_pop(&in), 'y');
}

// Keys pressed while another is hidden read as usual, modifiers' flags aside.
static void test_other_keys_read_normally(void) {
  reset();
  press_in_menu(KEY_ESC);
  poll_one(16, KBD_FIFO_PRESSED, 'a');
  CHECK_EQ_INT(in.q.count, 2);  // down, char
  CHECK_EQ_INT(kbd_chars_pop(&in), 'a');
  CHECK(kbd_keyset_test(&in.down, 'a'));
  CHECK(!kbd_keyset_test(&in.down, KEY_ESC));
  poll_one(16, KBD_FIFO_PRESSED, KEY_F4);
  CHECK_EQ_U32(pressed(), BTN_F4);
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  poll_one(16, KBD_FIFO_HOLD, KEY_ESC);
  CHECK_EQ_U32(btn.curr & BTN_ESC, 0);
  CHECK_EQ_U32(btn.curr, BTN_F4);
  poll_one(16, KBD_FIFO_RELEASED, KEY_F4);
  CHECK_EQ_U32(pad_released(), PAD_A);
  CHECK(kbd_hide_has(&hide, KEY_ESC));
}

// The menu key is Shift+F5: F10 goes down under Shift and may come up as F5
// once Shift is up. Both are the same key here, and Shift is hidden too.
static void test_menu_key_released_under_either_code(void) {
  reset();
  poll_one(16, KBD_FIFO_PRESSED, KEY_MOD_SHL);
  press_in_menu(KEY_F10);
  CHECK_EQ_INT(hide.count, 2);
  CHECK(kbd_hide_has(&hide, KEY_F5));
  CHECK(kbd_hide_has(&hide, KEY_MOD_SHL));
  poll_one(300, KBD_FIFO_HOLD, KEY_F10);
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_RELEASED, KEY_MOD_SHL);
  CHECK_NOTHING();
  // F5 still down, Shift up: its repeat comes under its own code, PAD_B's key.
  poll_one(100, KBD_FIFO_HOLD, KEY_F5);
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_RELEASED, KEY_F5);
  CHECK_NOTHING();
  CHECK_EQ_INT(hide.count, 0);
  poll_one(16, KBD_FIFO_PRESSED, KEY_F5);
  CHECK_EQ_U32(pad_pressed(), PAD_B);
}

// The other way: Esc goes down plain, Shift joins, Esc comes up as Brk.
static void test_esc_released_as_brk(void) {
  reset();
  press_in_menu(KEY_ESC);
  poll_one(16, KBD_FIFO_PRESSED, KEY_MOD_SHL);
  CHECK_EQ_U32(btn.curr, BTN_SHIFT);  // a new key, read as one
  poll_one(100, KBD_FIFO_HOLD, KEY_BRK);
  CHECK_EQ_U32(btn.curr, BTN_SHIFT);
  poll_one(16, KBD_FIFO_RELEASED, KEY_BRK);
  CHECK_EQ_INT(hide.count, 0);
  poll_one(16, KBD_FIFO_RELEASED, KEY_MOD_SHL);
  poll_one(16, KBD_FIFO_PRESSED, KEY_ESC);
  CHECK_EQ_U32(pressed(), BTN_ESC);
}

// A letter pressed plain, repeating as a capital (Shift), released either way.
static void test_letter_case_is_one_key(void) {
  reset();
  press_in_menu('w');
  poll_one(100, KBD_FIFO_PRESSED, 'W');
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_RELEASED, 'W');
  CHECK_NOTHING();
  CHECK_EQ_INT(hide.count, 0);
}

// A hidden Shift's items still reach the gamepad's Shift tracking, which goes
// on folding other keys' codes under it.
static void test_hidden_shift_still_tracked_for_the_gamepad(void) {
  reset();
  poll_one(16, KBD_FIFO_PRESSED, KEY_MOD_SHR);
  hide_held();
  CHECK_EQ_INT(btn.pad.mods, 0);  // the clear drops it, as ever...
  poll_one(100, KBD_FIFO_HOLD, KEY_MOD_SHR);
  CHECK_EQ_INT(btn.pad.mods, KBD_PADMOD_SHR);  // ...and its next report restores it
  CHECK_NOTHING();
  // Shift+F4 (F9) is PAD_A's key under Shift: read, as a key of its own.
  poll_one(16, KBD_FIFO_PRESSED, KEY_F9);
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  poll_one(16, KBD_FIFO_RELEASED, KEY_F9);
  CHECK_EQ_U32(pad_released(), PAD_A);
  poll_one(16, KBD_FIFO_RELEASED, KEY_MOD_SHR);
  CHECK_EQ_INT(btn.pad.mods, 0);
  CHECK_EQ_INT(hide.count, 0);
}

// A key that never reports its release ends by itself: Left under Shift is
// silent, repeat and release alike, and after that its next press must read.
static void test_silent_release_expires(void) {
  reset();
  press_in_menu(KEY_LEFT);
  poll_one(16, KBD_FIFO_PRESSED, KEY_MOD_SHL);
  poll_one(16, KBD_FIFO_RELEASED, KEY_MOD_SHL);
  CHECK(kbd_hide_has(&hide, KEY_LEFT));
  // Its first tick may count up to one poll gap too many.
  for (uint32_t t = 0; t < KBD_HIDE_QUIET_MS - 2 * KBD_HIDE_POLL_GAP_MS - 32;
       t += 16) {
    poll_none(16);
    CHECK(kbd_hide_has(&hide, KEY_LEFT));
  }
  for (uint32_t t = 0; t < 2 * KBD_HIDE_POLL_GAP_MS + 32; t += 16)
    poll_none(16);
  CHECK_EQ_INT(hide.count, 0);
  poll_one(16, KBD_FIFO_PRESSED, KEY_LEFT);
  CHECK_EQ_U32(pressed(), BTN_LEFT);
}

// Reports keep it alive for as long as they come: the wait for the first HOLD
// (300 ms, plus the scan and the bus) is well inside the quiet time.
static void test_reports_keep_it_hidden(void) {
  reset();
  press_in_menu(KEY_ESC);
  for (int i = 0; i < 25; i++)  // 400 ms of a slow first report
    poll_none(16);
  CHECK(kbd_hide_has(&hide, KEY_ESC));
  for (int i = 0; i < 60; i++) {
    poll_one(116, KBD_FIFO_HOLD, KEY_ESC);
    CHECK_NOTHING();
  }
  CHECK(kbd_hide_has(&hide, KEY_ESC));
}

// A stall counts for little: the app read no items in it, so the silence says
// nothing about the key. Even so, a key whose release went missing does end at
// a slow poller.
static void test_a_stall_does_not_expire_it(void) {
  reset();
  press_in_menu(KEY_UP);
  poll_none(5000);
  CHECK(kbd_hide_has(&hide, KEY_UP));
  poll_one(100, KBD_FIFO_PRESSED, KEY_UP);
  CHECK_NOTHING();
  for (uint32_t t = 0; t < KBD_HIDE_QUIET_MS + KBD_HIDE_POLL_GAP_MS;
       t += KBD_HIDE_POLL_GAP_MS)
    poll_none(200);
  CHECK_EQ_INT(hide.count, 0);
}

// Background polls (sys.sleep) filter too, and an edge gathered in the run is
// kept for the next foreground poll.
static void test_background_polls(void) {
  reset();
  press_in_menu(KEY_ESC);
  item_t rep = {KBD_FIFO_HOLD, KEY_ESC};
  poll_items(10, true, &rep, 1);
  CHECK_NOTHING();
  item_t rel = {KBD_FIFO_RELEASED, KEY_ESC};
  poll_items(10, true, &rel, 1);
  CHECK_NOTHING();
  item_t press = {KBD_FIFO_PRESSED, KEY_ESC};
  poll_items(10, true, &press, 1);
  poll_none(10);
  CHECK_EQ_U32(pressed(), BTN_ESC);
}

// A key held since before the menu opened, hidden at the open: the menu's own
// close finds it gone from the key-down set, and it stays hidden.
static void test_hidden_at_the_open_stays_through_the_close(void) {
  reset();
  poll_one(16, KBD_FIFO_PRESSED, KEY_F3);
  CHECK_EQ_U32(pressed(), BTN_F3);
  hide_held();                      // the menu opens
  CHECK(kbd_hide_has(&hide, KEY_F3));
  poll_one(16, KBD_FIFO_PRESSED, KEY_ESC);
  CHECK_EQ_U32(pressed(), BTN_ESC);  // the menu reads its own Esc
  hide_held();                      // and closes
  CHECK(kbd_hide_has(&hide, KEY_F3));
  CHECK(kbd_hide_has(&hide, KEY_ESC));
  poll_one(100, KBD_FIFO_HOLD, KEY_F3);
  CHECK_NOTHING();
  poll_one(16, KBD_FIFO_RELEASED, KEY_ESC);
  poll_one(16, KBD_FIFO_RELEASED, KEY_F3);
  CHECK_NOTHING();
  CHECK_EQ_INT(hide.count, 0);
}

// Released during the menu: nothing is left of it.
static void test_released_in_the_menu_is_not_hidden(void) {
  reset();
  poll_one(16, KBD_FIFO_PRESSED, KEY_ENTER);
  poll_one(16, KBD_FIFO_RELEASED, KEY_ENTER);
  poll_none(16);
  hide_held();
  CHECK_EQ_INT(hide.count, 0);
  poll_one(16, KBD_FIFO_PRESSED, KEY_ENTER);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
}

// More keys than the controller can hold down at once cannot break the list.
static void test_list_is_bounded(void) {
  reset();
  for (int c = 0; c < 16; c++)
    poll_one(1, KBD_FIFO_PRESSED, (uint8_t)('a' + c));
  hide_held();
  CHECK_EQ_INT(hide.count, KBD_HIDE_MAX);
}

// Injected one-shot (the simulator's and the dev command's key): its 80 ms hold
// goes on after the menu has closed. Hidden, then retired with no edge.
static void test_injected_oneshot(void) {
  reset();
  inj.pending = BTN_ESC;
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_ESC);  // the menu reads it, and closes
  hide_held();
  CHECK_EQ_INT(hide.count, 0);       // no FIFO item will ever release it
  CHECK_EQ_U32(inj.hidden, BTN_ESC);
  CHECK_NOTHING();
  poll_none(16);
  CHECK_NOTHING();
  poll_none(16);
  CHECK_NOTHING();
  poll_none(HOLD_MS);                // retired: no up event, no release edge
  CHECK_NOTHING();
  CHECK_EQ_U32(inj.active, 0);
  CHECK_EQ_U32(inj.hidden, 0);
  inj.pending = BTN_ESC;             // the same key again: a press
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_ESC);
  kbd_event_t e;
  CHECK(kbd_evq_pop(&in.q, &e));
  CHECK_EQ_INT(e.type, KBD_EV_DOWN);
}

// A re-injection queued while it is held goes out as a press after the retire,
// and a pending one at the close was not down yet: it keeps its edge.
static void test_injected_pending(void) {
  reset();
  inj.pending = BTN_ESC;
  poll_none(16);
  inj.pending = BTN_ESC;  // while active
  hide_held();
  poll_none(HOLD_MS);     // retired
  CHECK_NOTHING();
  poll_none(16);          // republished
  CHECK_EQ_U32(pressed(), BTN_ESC);

  reset();
  inj.pending = BTN_ENTER;
  hide_held();
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
}

// A keydown latch, with a gamepad button bound to it: hidden until keyup, and
// a new keydown is a new press.
static void test_injected_latch(void) {
  reset();
  kbd_inject_hold(&inj, &btn, &in, BTN_F5);
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_F5);
  CHECK_EQ_U32(pad_pressed(), PAD_B);
  hide_held();
  CHECK_NOTHING();
  poll_none(16);
  CHECK_NOTHING();
  kbd_inject_release(&inj, &btn, &in, &map, BTN_F5);
  poll_none(16);
  CHECK_NOTHING();
  kbd_inject_hold(&inj, &btn, &in, BTN_F5);
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_F5);
  CHECK_EQ_U32(pad_pressed(), PAD_B);
  poll_none(16);
  kbd_inject_release(&inj, &btn, &in, &map, BTN_F5);
  CHECK_EQ_U32(released(), BTN_F5);
  CHECK_EQ_U32(pad_released(), PAD_B);
}

// Discard (a modal right after the menu): the one-shots go with their hiding.
static void test_discard_drops_hidden_oneshots(void) {
  reset();
  inj.pending = BTN_ESC;
  poll_none(16);
  hide_held();
  kbd_inject_drop_oneshots(&inj, &btn, &map);
  CHECK_EQ_U32(inj.hidden, 0);
  inj.pending = BTN_ESC;
  poll_none(16);
  CHECK_EQ_U32(pressed(), BTN_ESC);
}

// The simulator's path: decoded events through the same filter, chars included.
static void test_simulator_events(void) {
  reset();
  kbd_event_t down = {KBD_EV_DOWN, 'a', 0, 0};
  kbd_event_t rep = {KBD_EV_DOWN, 'a', 0, KBD_EVF_REPEAT};
  kbd_event_t ch = {KBD_EV_CHAR, 'a', 'a', KBD_EVF_REPEAT};
  kbd_event_t up = {KBD_EV_UP, 'a', 0, 0};
  kbd_input_accept(&in, down);
  hide_held();
  CHECK(kbd_hide_event(&hide, &btn.pad, &map, rep));
  CHECK(kbd_hide_event(&hide, &btn.pad, &map, ch));
  CHECK(kbd_hide_event(&hide, &btn.pad, &map, up));
  CHECK_EQ_INT(hide.count, 0);
  CHECK(!kbd_hide_event(&hide, &btn.pad, &map, down));
}

int main(void) {
  test_held_esc_repeats_as_hold();
  test_held_arrow_repeats_as_pressed();
  test_repeat_and_release_together();
  test_held_keys_give_no_chars();
  test_other_keys_read_normally();
  test_menu_key_released_under_either_code();
  test_esc_released_as_brk();
  test_letter_case_is_one_key();
  test_hidden_shift_still_tracked_for_the_gamepad();
  test_silent_release_expires();
  test_reports_keep_it_hidden();
  test_a_stall_does_not_expire_it();
  test_background_polls();
  test_hidden_at_the_open_stays_through_the_close();
  test_released_in_the_menu_is_not_hidden();
  test_list_is_bounded();
  test_injected_oneshot();
  test_injected_pending();
  test_injected_latch();
  test_discard_drops_hidden_oneshots();
  test_simulator_events();
  return check_report("test_kbd_hide");
}
