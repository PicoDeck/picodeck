// Host unit tests for src/drivers/kbd_event_queue.h: the STM32 FIFO decoder
// and the keyboard event queue behind picocalc.input.pollEvent / isKeyDown.
// Each test drives kbd_fifo_apply() with the (state, keycode) items the
// keyboard controller's FIFO returns, the way kbd_poll() does on hardware.
#include "check.h"
#include "kbd_event_queue.h"

static kbd_input_t in;
static kbd_buttons_t btn;

static void reset(void) {
  kbd_input_clear(&in);
  memset(&btn, 0, sizeof(btn));
}

static void begin_poll(void) {
  kbd_buttons_begin_poll(&btn);
  in.ev_pushed = 0;
  in.char_pushed = 0;
}

static uint32_t pressed(void) { return btn.curr & ~btn.prev; }
static uint32_t released(void) { return ~btn.curr & btn.prev; }

static kbd_event_t pop(void) {
  kbd_event_t e = {0, 0, 0, 0};
  kbd_evq_pop(&in.q, &e);
  return e;
}

#define CHECK_EV(e, t, k, c, f) do { kbd_event_t _e = (e); \
  CHECK_EQ_INT(_e.type, t); CHECK_EQ_INT(_e.key, k); \
  CHECK_EQ_INT(_e.ch, c); CHECK_EQ_INT(_e.flags, f); } while (0)

// Two chars in one poll: both are queued in order, and getChar's backlog
// hands them out one per poll instead of keeping only the last.
static void test_two_chars_in_one_poll(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'a');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'b');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'a');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'b');
  CHECK_EQ_INT(in.q.count, 6);
  CHECK_EV(pop(), KBD_EV_DOWN, 'a', 0, 0);
  CHECK_EV(pop(), KBD_EV_CHAR, 'a', 'a', 0);
  CHECK_EV(pop(), KBD_EV_DOWN, 'b', 0, 0);
  CHECK_EV(pop(), KBD_EV_CHAR, 'b', 'b', 0);
  CHECK_EV(pop(), KBD_EV_UP, 'a', 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'b', 0, 0);
  CHECK(!kbd_evq_pop(&in.q, NULL));
  CHECK_EQ_INT(kbd_chars_pop(&in), 'a');
  CHECK_EQ_INT(kbd_chars_pop(&in), 'b');
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
}

// Letters report key-up: isKeyDown follows press and release.
static void test_letter_key_down_and_up(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'q');
  CHECK(kbd_keyset_test(&in.down, 'q'));
  CHECK(!kbd_keyset_test(&in.down, 'w'));
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'q');
  CHECK(!kbd_keyset_test(&in.down, 'q'));
}

// Shift can come up before the letter, so the release names 'a' after a
// press of 'A': the key-down set is case-folded and the key does not stick.
static void test_shifted_letter_release(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_MOD_SHL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'A');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_MOD_SHL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'a');
  CHECK(!kbd_keyset_test(&in.down, 'a'));
  CHECK(!kbd_keyset_test(&in.down, 'A'));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_MOD_SHL, 0, KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_DOWN, 'A', 0, KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_CHAR, 'A', 'A', KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_UP, KEY_MOD_SHL, 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'a', 0, 0);
}

// A HOLD for a key whose press was seen is a repeat: flagged events and a
// repeated char, and no fresh press edge.
static void test_hold_is_a_repeat(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  kbd_evq_clear(&in.q);
  kbd_chars_pop(&in);
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), KEY_ENTER);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER);
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_ENTER, 0, KBD_EVF_REPEAT);
  CHECK_EV(pop(), KBD_EV_CHAR, KEY_ENTER, '\n', KBD_EVF_REPEAT);
  CHECK_EQ_INT(kbd_chars_pop(&in), '\n');
}

// A HOLD for a key whose press was not seen (it was pressed before
// kbd_clear_state / kbd_discard_pending, e.g. before ui_confirm opened) is
// not a press: no press edge, no event, no char. Its release is still an up
// event, and a later real press is a normal press.
static void test_hold_after_clear_is_not_a_press(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  reset(); // kbd_discard_pending
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), 0);
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 0);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER); // held, truthfully
  CHECK_EQ_INT(in.q.count, 0);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
  CHECK(kbd_keyset_test(&in.down, 'y'));
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_ENTER);
  CHECK_EQ_U32(released(), BTN_ENTER);
  CHECK_EV(pop(), KBD_EV_UP, KEY_ENTER, 0, 0);
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
}

// A button pressed and released inside one 50 ms poll used to produce no
// edge at all. It now reads as held (with a press edge) for that poll and is
// released at the next.
// The STM32 may repeat HOLD. A key held across a clear stays quiet on every
// HOLD (not just the first) until it is released and pressed again: a 'y'
// held from typing when ui_confirm opens can never answer it.
static void test_repeated_hold_after_clear_stays_quiet(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  reset(); // kbd_discard_pending
  for (int i = 0; i < 3; i++) {
    begin_poll();
    CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 0);
    CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), 0);
    CHECK_EQ_U32(pressed(), 0);
  }
  CHECK_EQ_INT(in.q.count, 0);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
  CHECK(kbd_keyset_test(&in.down, 'y')); // physically held, truthfully
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'y');
  kbd_evq_clear(&in.q);
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  kbd_chars_pop(&in);
  kbd_evq_clear(&in.q);
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 'y');
  CHECK_EV(pop(), KBD_EV_DOWN, 'y', 0, KBD_EVF_REPEAT);
  CHECK_EQ_INT(kbd_chars_pop(&in), 'y'); // a seen press repeats again
}

static void test_tap_inside_one_poll(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_UP);
  CHECK_EQ_U32(pressed(), BTN_UP);
  CHECK_EQ_U32(btn.curr, BTN_UP);
  CHECK(!kbd_keyset_test(&in.down, KEY_UP));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_UP, 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, KEY_UP, 0, 0);
  begin_poll();
  CHECK_EQ_U32(btn.curr, 0);
  CHECK_EQ_U32(released(), BTN_UP);
  begin_poll();
  CHECK_EQ_U32(released(), 0);
}

// Press, release, press again inside one poll: the key ends up held.
static void test_tap_then_press_again(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  begin_poll();
  CHECK_EQ_U32(btn.curr, BTN_UP);
}

static void test_ctrl_letter(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_MOD_CTRL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 's');
  kbd_evq_pop(&in.q, NULL); // ctrl down
  CHECK_EV(pop(), KBD_EV_DOWN, 's', 0, KBD_MOD_CTRL);
  CHECK_EV(pop(), KBD_EV_CHAR, 's', 0x13, KBD_MOD_CTRL);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0x13);
}

// Menu (F10) and Brk are the OS's: they drive the button mask / raw key but
// never enter the queue.
static void test_os_keys_not_queued(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_F10);
  CHECK_EQ_U32(btn.curr, BTN_MENU);
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_BRK), KEY_BRK);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_F10);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_BRK);
  CHECK_EQ_INT(in.q.count, 0);
}

static void test_queue_drops_oldest(void) {
  reset();
  for (int i = 0; i < KBD_EVENT_QUEUE_LEN + 3; i++)
    kbd_input_push(&in, KBD_EV_DOWN, (uint8_t)(0x20 + i), 0, 0);
  CHECK_EQ_INT(in.q.count, KBD_EVENT_QUEUE_LEN);
  CHECK_EQ_INT(pop().key, 0x23);
  for (int i = 1; i < KBD_EVENT_QUEUE_LEN; i++)
    CHECK_EQ_INT(pop().key, 0x23 + i);
  CHECK(!kbd_evq_pop(&in.q, NULL));
}

static void test_char_backlog_drops_oldest(void) {
  reset();
  for (int i = 0; i < KBD_CHAR_BACKLOG + 2; i++)
    kbd_chars_push(&in, (char)('a' + i));
  CHECK_EQ_INT(kbd_chars_pop(&in), 'c');
  kbd_chars_drop_newest(&in, 1);
  for (int i = 1; i < KBD_CHAR_BACKLOG - 1; i++)
    CHECK_EQ_INT(kbd_chars_pop(&in), 'c' + i);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
}

// The idle-dim wake swallow drops the waking poll's presses and chars but
// keeps its releases, so a key the app saw go down still comes up.
// ...and drops the ups of keys pressed in the waking poll (their downs are
// dropped too, so the ups would be orphans).
static void test_drop_newest_keeps_ups(void) {
  reset();
  kbd_keyset_t before;
  memset(&before, 0, sizeof(before));
  kbd_keyset_set(&before, 'x');
  kbd_input_push(&in, KBD_EV_DOWN, 'x', 0, 0);  // an earlier poll
  kbd_input_push(&in, KBD_EV_UP, 'x', 0, 0);    // this poll ...
  kbd_input_push(&in, KBD_EV_DOWN, 'y', 0, 0);
  kbd_input_push(&in, KBD_EV_CHAR, 'y', 'y', 0);
  kbd_input_push(&in, KBD_EV_UP, 'y', 0, 0);
  kbd_evq_drop_newest_keep_ups(&in.q, 4, &before);
  CHECK_EQ_INT(in.q.count, 2);
  CHECK_EV(pop(), KBD_EV_DOWN, 'x', 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'x', 0, 0);
}

static void test_injected_button_events(void) {
  reset();
  kbd_input_button_events(&in, BTN_ESC | BTN_CTRL | BTN_MENU, KBD_EV_DOWN,
                          BTN_ESC | BTN_CTRL);
  CHECK(kbd_keyset_test(&in.down, KEY_ESC));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_ESC, 0, KBD_MOD_CTRL);
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_MOD_CTRL, 0, KBD_MOD_CTRL);
  CHECK(!kbd_evq_pop(&in.q, NULL)); // MENU is the OS's
  kbd_input_button_events(&in, BTN_ESC | BTN_UP, KBD_EV_UP, 0);
  CHECK_EV(pop(), KBD_EV_UP, KEY_ESC, 0, 0);
  CHECK(!kbd_evq_pop(&in.q, NULL)); // UP was never down
}

// Delete is its own key (Shift+Delete sends End): a press and release are
// BTN_DEL edges, with down/up events under its keycode.
static void test_delete_key_is_btn_del(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_DEL);
  CHECK_EQ_U32(pressed(), BTN_DEL);
  CHECK(kbd_keyset_test(&in.down, KEY_DEL));
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_DEL);
  CHECK_EQ_U32(released(), BTN_DEL);
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_DEL, 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, KEY_DEL, 0, 0);
  CHECK(!kbd_evq_pop(&in.q, NULL));
}

static void test_keycode_button_map(void) {
  CHECK_EQ_U32(kbd_keycode_to_button(KEY_MOD_SHR), BTN_SHIFT);
  CHECK_EQ_INT(kbd_button_to_keycode(BTN_SHIFT), KEY_MOD_SHL);
  CHECK_EQ_INT(kbd_button_to_keycode(BTN_F9), KEY_F9);
  CHECK_EQ_INT(kbd_button_to_keycode(BTN_DEL), KEY_DEL); // injected "del"
  CHECK_EQ_U32(kbd_keycode_to_button('a'), 0);
  CHECK_EQ_U32(kbd_buttons_from_mods(kbd_mods_from_buttons(0xFFFFFFFFu)),
               KBD_MOD_BUTTONS);
}

// ── Background polls (sys.sleep) then the app's poll ─────────────────────────
// kbd_poll_begin as keyboard.c runs it; raw is getRawKey's value.
static uint8_t raw;

static void poll_start(bool bg) {
  kbd_poll_begin(&btn, bg, &raw);
  in.ev_pushed = 0;
  in.char_pushed = 0;
}

static void apply(uint8_t state, uint8_t key) {
  uint8_t r = kbd_fifo_apply(&in, &btn, state, key);
  if (r)
    raw = r;
}

// A key tapped (pressed and released) during the sleep: the app's next poll
// sees the press edge and the raw key, the one after the release edge.
static void test_bg_tap_reaches_next_foreground_poll(void) {
  reset();
  raw = 0;
  poll_start(false);                       // the app's poll before the sleep
  poll_start(true);                        // sys.sleep's background polls
  apply(KBD_FIFO_PRESSED, KEY_ENTER);
  poll_start(true);
  apply(KBD_FIFO_RELEASED, KEY_ENTER);
  poll_start(true);
  poll_start(false);                       // input.update() after the sleep
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_INT(raw, KEY_ENTER);
  poll_start(false);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), BTN_ENTER);
  CHECK_EQ_INT(raw, 0);
}

// A key tapped inside a single background poll behaves the same.
static void test_bg_tap_inside_one_background_poll(void) {
  reset();
  raw = 0;
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_UP);
  apply(KBD_FIFO_RELEASED, KEY_UP);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(pressed(), BTN_UP);
  poll_start(false);
  CHECK_EQ_U32(released(), BTN_UP);
}

// A key pressed during the sleep and still held: a press edge, then held.
static void test_bg_press_still_held(void) {
  reset();
  raw = 0;
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_ENTER);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  CHECK_EQ_INT(raw, KEY_ENTER);
  poll_start(false);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_ENTER);
  CHECK_EQ_U32(released(), BTN_ENTER);
}

// A key held before the sleep and released during it: a release edge only.
static void test_bg_release_of_key_held_before(void) {
  reset();
  raw = 0;
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_LEFT);
  poll_start(false);                       // held, edge consumed
  CHECK_EQ_U32(pressed(), 0);
  poll_start(true);
  apply(KBD_FIFO_RELEASED, KEY_LEFT);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), BTN_LEFT);
  CHECK_EQ_INT(raw, 0);
}

// A tap in the app's last poll before the sleep is released by the first
// background poll: after the sleep it reads as released, not as held.
static void test_tap_before_sleep_released(void) {
  reset();
  raw = 0;
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_DOWN);
  apply(KBD_FIFO_RELEASED, KEY_DOWN);
  CHECK_EQ_U32(pressed(), BTN_DOWN);
  poll_start(true);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(btn.curr, 0);
  CHECK_EQ_U32(released(), BTN_DOWN);
  CHECK_EQ_U32(pressed(), 0);
}

// A key held through the whole sleep (STM32 HOLD reports meanwhile): no new
// press edge and no release edge afterwards; it is still held.
static void test_bg_key_held_through_sleep(void) {
  reset();
  raw = 0;
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_RIGHT);
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_HOLD, KEY_RIGHT);
  poll_start(true);
  apply(KBD_FIFO_HOLD, KEY_RIGHT);
  poll_start(false);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_U32(btn.curr, BTN_RIGHT);
}

// Two different keys in separate background polls: both press edges.
static void test_bg_two_keys_in_separate_polls(void) {
  reset();
  raw = 0;
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_UP);
  apply(KBD_FIFO_RELEASED, KEY_UP);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_ENTER);
  poll_start(true);
  poll_start(false);
  CHECK_EQ_U32(pressed(), BTN_UP | BTN_ENTER);
  CHECK_EQ_INT(raw, KEY_ENTER);
  poll_start(false);
  CHECK_EQ_U32(released(), BTN_UP);
  CHECK_EQ_U32(btn.curr, BTN_ENTER);
}

// Enter tapped, then Sym, in one sleep: the system menu clears the keyboard
// state on entry (system_menu.c menu_loop), which also ends the background
// run, so its first poll sees no Enter edge to activate an item with.
static void test_menu_after_bg_sees_no_earlier_edge(void) {
  reset();
  raw = 0;
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_ENTER);
  apply(KBD_FIFO_RELEASED, KEY_ENTER);
  poll_start(true);
  // menu_loop entry: kbd_clear_state()
  kbd_input_clear(&in);
  memset(&btn, 0, sizeof(btn));
  raw = 0;
  CHECK(!btn.in_bg);
  poll_start(false);                       // the menu's first kbd_poll
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
  CHECK_EQ_INT(raw, 0);
}

// A key that wakes the dimmed screen inside a background run is swallowed,
// but a key gathered by an earlier poll of the run still reaches the app.
static void test_bg_wake_swallow_keeps_earlier_edges(void) {
  reset();
  raw = 0;
  poll_start(false);
  poll_start(true);
  apply(KBD_FIFO_PRESSED, KEY_ENTER);      // earlier poll of the run
  apply(KBD_FIFO_RELEASED, KEY_ENTER);
  poll_start(true);
  uint32_t cb = btn.curr, pb = btn.prev;
  uint8_t raw_before = raw;                // s_last_raw_key before this poll's own decode
  apply(KBD_FIFO_PRESSED, KEY_UP);         // this poll's key wakes the screen
  kbd_buttons_swallow(&btn, true, cb, pb);
  raw = raw_before;                        // keyboard.c:412 — bg_run ? raw_before : 0
  poll_start(false);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  CHECK_EQ_INT(raw, KEY_ENTER);            // the earlier key (A) survives, not the waking key (B) or 0
  poll_start(false);
  CHECK_EQ_U32(released(), BTN_ENTER);
  CHECK_EQ_U32(btn.curr, 0);               // the swallowed Up never appears
}

// ── Injected input across kbd_clear_state ────────────────────────────────────
// keyboard.c's kbd_clear_state: zero the masks and the input state, then
// kbd_inject_after_clear. The injection state itself survives. No pad map
// here: the gamepad side is tests/unit/test_gamepad.c's.
static kbd_inject_t inj;
#define HOLD_MS 80

static void clear_state(void) {
  memset(&btn, 0, sizeof(btn));
  kbd_input_clear(&in);
  raw = 0;
  kbd_inject_after_clear(&inj, &btn, NULL);
}

static void fg_poll(uint32_t now) {
  poll_start(false);
  kbd_inject_poll(&inj, &btn, &in, NULL, false, now, HOLD_MS);
}

// (a) An injected Enter opens a modal, which clears the keyboard while the
// one-shot is still active: the modal's first poll sees Enter held with no
// press edge, and its retire gives a release edge.
static void test_inject_active_across_clear(void) {
  reset();
  memset(&inj, 0, sizeof(inj));
  inj.pending |= BTN_ENTER;
  fg_poll(1000);
  CHECK_EQ_U32(pressed(), BTN_ENTER);      // the launcher/menu sees it
  clear_state();                           // the modal opens
  fg_poll(1010);
  CHECK_EQ_U32(pressed(), 0);              // no fresh press to answer with
  CHECK_EQ_U32(btn.curr, BTN_ENTER);       // still reads as held
  fg_poll(1020);
  CHECK_EQ_U32(pressed(), 0);
  fg_poll(1000 + HOLD_MS);                 // hold time over: retired
  CHECK_EQ_U32(released(), BTN_ENTER);
  CHECK_EQ_U32(btn.curr, 0);
  fg_poll(1200);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(released(), 0);
}

// A keydown latch across a clear: held without an edge, keyup releases it.
static void test_inject_held_across_clear(void) {
  reset();
  memset(&inj, 0, sizeof(inj));
  fg_poll(0);
  kbd_inject_hold(&inj, &btn, &in, BTN_ENTER);
  fg_poll(10);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  clear_state();
  fg_poll(20);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER);
  kbd_inject_release(&inj, &btn, &in, NULL, BTN_ENTER);  // at once
  CHECK_EQ_U32(btn.curr, 0);
  CHECK_EQ_U32(released(), BTN_ENTER);
  fg_poll(30);
  CHECK_EQ_U32(btn.curr, 0);                        // not resurrected
  CHECK_EQ_U32(pressed(), 0);
}

// (b) An injection queued after the modal was requested (still pending at
// the clear) is meant for the modal: its first poll publishes it with a
// press edge.
static void test_inject_pending_across_clear(void) {
  reset();
  memset(&inj, 0, sizeof(inj));
  fg_poll(0);
  inj.pending |= BTN_ENTER;
  clear_state();
  fg_poll(10);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  kbd_event_t e = pop();
  CHECK_EQ_INT(e.type, KBD_EV_DOWN);
  CHECK_EQ_INT(e.key, KEY_ENTER);
}

// (c) A char injected before the clear and not read yet is not typed into
// whatever the clear is for.
static void test_inject_char_dropped_by_clear(void) {
  reset();
  memset(&inj, 0, sizeof(inj));
  inj.ch = 'x';
  clear_state();
  CHECK_EQ_INT(inj.ch, 0);
}

int main(void) {
  test_two_chars_in_one_poll();
  test_letter_key_down_and_up();
  test_shifted_letter_release();
  test_hold_is_a_repeat();
  test_hold_after_clear_is_not_a_press();
  test_repeated_hold_after_clear_stays_quiet();
  test_tap_inside_one_poll();
  test_tap_then_press_again();
  test_ctrl_letter();
  test_os_keys_not_queued();
  test_queue_drops_oldest();
  test_char_backlog_drops_oldest();
  test_drop_newest_keeps_ups();
  test_injected_button_events();
  test_delete_key_is_btn_del();
  test_keycode_button_map();
  test_bg_tap_reaches_next_foreground_poll();
  test_bg_tap_inside_one_background_poll();
  test_bg_press_still_held();
  test_bg_release_of_key_held_before();
  test_tap_before_sleep_released();
  test_bg_key_held_through_sleep();
  test_bg_two_keys_in_separate_polls();
  test_menu_after_bg_sees_no_earlier_edge();
  test_bg_wake_swallow_keeps_earlier_edges();
  test_inject_active_across_clear();
  test_inject_held_across_clear();
  test_inject_pending_across_clear();
  test_inject_char_dropped_by_clear();
  return check_report("test_kbd_event_queue");
}
