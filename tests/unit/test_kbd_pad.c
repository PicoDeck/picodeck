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
  apply(KBD_FIFO_PRESSED, KEY_MOD_CTRL);
  apply(KBD_FIFO_PRESSED, KEY_F10);
  apply(KBD_FIFO_PRESSED, KEY_BRK);
  apply(KBD_FIFO_PRESSED, KEY_ESC);  // Esc is not a gamepad button by default
  CHECK_EQ_U32(btn.pad.curr, 0);
  apply(KBD_FIFO_PRESSED, KEY_F6);   // only ever Shift+F1: Start
  CHECK_EQ_U32(btn.pad.curr, PAD_START);
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
// F-keys (and Esc, the modifiers) repeat as HOLD; arrows, letters, Enter,
// Tab, Backspace and Delete repeat as PRESSED (keyboard.ino transition_to),
// so they come back with a press edge, as their BTN_* bits do.
static void test_hold_after_clear_is_quiet(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F2);
  clear_state();
  poll_start(false);
  CHECK_EQ_U32(btn.pad.curr, 0);           // nothing until the STM32 reports
  apply(KBD_FIFO_HOLD, KEY_F2);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_L);
  apply(KBD_FIFO_HOLD, KEY_F2);            // HOLD repeats every 100 ms
  poll_start(false);
  CHECK_EQ_U32(pad_pressed(), 0);
  apply(KBD_FIFO_RELEASED, KEY_F2);
  CHECK_EQ_U32(pad_released(), PAD_L);
  // A HOLD of a key whose press was seen is only a repeat.
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F3);
  poll_start(false);
  apply(KBD_FIFO_HOLD, KEY_F3);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_R);
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
  kbd_pad_t p;
  memset(&p, 0, sizeof(p));
  kbd_event_t down = {KBD_EV_DOWN, KEY_UP, 0, 0};
  kbd_event_t up = {KBD_EV_UP, KEY_UP, 0, 0};
  kbd_event_t ch = {KBD_EV_CHAR, 'w', 'w', 0};
  kbd_pad_begin_poll(&p);
  kbd_pad_event(&p, &map, down);
  kbd_pad_event(&p, &map, up);            // a tap inside one poll
  CHECK_EQ_U32(p.curr & ~p.prev, PAD_UP);
  kbd_pad_begin_poll(&p);
  CHECK_EQ_U32(p.prev & ~p.curr, PAD_UP);
  map.key[0][1] = 'w';
  kbd_pad_event(&p, &map, ch);            // chars change nothing
  CHECK_EQ_U32(p.curr, 0);
  // A repeat-flagged down (SDL auto-repeat after a clear) is a quiet hold.
  kbd_event_t rep = {KBD_EV_DOWN, KEY_UP, 0, KBD_EVF_REPEAT};
  kbd_pad_begin_poll(&p);
  kbd_pad_event(&p, &map, rep);
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


// ── Shift and Alt going down mid-hold ────────────────────────────────────────
// The keyboard controller (picocalc_keyboard keyboard.ino, transition_to)
// recomputes a key's code at every transition from the modifiers held then:
// under Shift, F1-F5 -> F6-F10, Del -> End, Tab -> Home, Enter -> Insert,
// Esc -> Brk, Up/Down -> PgUp/PgDn, and Left, Right, Backspace and Space send
// nothing; under Alt, B and Space send nothing and I sends Insert. Its
// repeats are HOLD for the F-keys, Esc and the shifted codes, PRESSED for
// arrows, letters, Enter, Tab, Backspace and Delete. These replay what it
// sends; no gamepad button may stay held, and the OS keys F10 (menu) and Brk
// (screenshot) never press one.

// Three idle polls, then nothing may be held.
static void check_nothing_held(void) {
  for (int i = 0; i < 3; i++)
    poll_start(false);
  CHECK_EQ_U32(btn.pad.curr, 0);
  CHECK_EQ_U32(btn.pad.down, 0);
}

static uint32_t menu_edge(void) { return btn_pressed() & BTN_MENU; }

typedef struct {
  uint8_t key, shifted;
  uint32_t pad;
  bool repeat_pressed; // unshifted repeats come as PRESSED, not HOLD
} twin_t;

static const twin_t k_twins[] = {
    {KEY_F1, KEY_F6, PAD_START, false}, {KEY_F2, KEY_F7, PAD_L, false},
    {KEY_F3, KEY_F8, PAD_R, false},     {KEY_F4, KEY_F9, PAD_A, false},
    {KEY_F5, KEY_F10, PAD_B, false},    {KEY_DEL, KEY_END, PAD_X, true},
    {KEY_TAB, KEY_HOME, PAD_SELECT, true}, {KEY_UP, KEY_PGUP, PAD_UP, true},
    {KEY_DOWN, KEY_PGDN, PAD_DOWN, true},
    {KEY_ENTER, KEY_INSERT, PAD_Y, true},  // Enter bound to Y's alternate
    {KEY_ESC, KEY_BRK, PAD_X, false},      // Esc bound to X's alternate
};

static void bind_enter_esc(void) {
  map.key[7][1] = KEY_ENTER;  // Y
  map.key[6][1] = KEY_ESC;    // X
}

// Key held, Shift pressed, key repeated and released under its shifted
// code, Shift released: the button is released with the key.
static void test_shift_pressed_mid_hold(void) {
  for (size_t i = 0; i < sizeof(k_twins) / sizeof(k_twins[0]); i++) {
    const twin_t *t = &k_twins[i];
    reset();
    bind_enter_esc();
    poll_start(false);
    apply(KBD_FIFO_PRESSED, t->key);
    CHECK_EQ_U32(pad_pressed(), t->pad);
    poll_start(false);
    apply(t->repeat_pressed ? KBD_FIFO_PRESSED : KBD_FIFO_HOLD, t->key);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
    poll_start(false);
    apply(KBD_FIFO_HOLD, t->shifted);
    apply(KBD_FIFO_HOLD, t->shifted);
    CHECK_EQ_U32(pad_pressed(), 0);
    CHECK_EQ_U32(btn.pad.curr, t->pad);     // still held
    CHECK_EQ_U32(menu_edge(), 0);           // a held F5 never opens the menu
    poll_start(false);
    apply(KBD_FIFO_RELEASED, t->shifted);
    CHECK_EQ_U32(pad_released(), t->pad);
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
    check_nothing_held();
  }
}

// Shift held first: a fresh press sends the shifted code. It presses the
// key's button, except F10 and Brk, which are the menu and screenshot keys.
// Shift released before the key: the key finishes under its own code.
static void test_shift_held_before_press(void) {
  for (size_t i = 0; i < sizeof(k_twins) / sizeof(k_twins[0]); i++) {
    const twin_t *t = &k_twins[i];
    bool os_key = t->shifted == KEY_F10 || t->shifted == KEY_BRK;
    reset();
    bind_enter_esc();
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHR);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, t->shifted);
    CHECK_EQ_U32(pad_pressed(), os_key ? 0 : t->pad);
    if (t->shifted == KEY_F10)
      CHECK_EQ_U32(menu_edge(), BTN_MENU);  // the OS's, as before
    poll_start(false);
    apply(KBD_FIFO_HOLD, t->shifted);
    CHECK_EQ_U32(btn.pad.curr, os_key ? 0 : t->pad);
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHR);  // Shift up first
    poll_start(false);
    apply(t->repeat_pressed ? KBD_FIFO_PRESSED : KBD_FIFO_HOLD, t->key);
    apply(KBD_FIFO_RELEASED, t->key);
    if (!os_key)
      CHECK_EQ_U32(pad_released(), t->pad);
    check_nothing_held();
  }
}

// The controller clears its own Shift flag one scan (>= 17 ms) after it
// reports Shift up, and scans the matrix keys before the side buttons. So a
// key released just after Shift (or an arrow released in the same scan)
// still arrives under its shifted code after the Shift RELEASED item. Codes
// only Shift produces fold back whatever the tracked state: nothing sticks.
static void test_shift_release_lag(void) {
  for (size_t i = 0; i < sizeof(k_twins) / sizeof(k_twins[0]); i++) {
    const twin_t *t = &k_twins[i];
    reset();
    bind_enter_esc();
    poll_start(false);
    apply(KBD_FIFO_PRESSED, t->key);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
    poll_start(false);
    apply(KBD_FIFO_HOLD, t->shifted);
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);  // Shift up first...
    poll_start(false);
    apply(KBD_FIFO_HOLD, t->shifted);       // ...the flag lags one scan
    apply(KBD_FIFO_RELEASED, t->shifted);
    CHECK_EQ_U32(pad_released(), t->pad);
    check_nothing_held();
  }
  // A key pressed in that window: the codes only Shift sends still press
  // their key's button; F10 stays the menu key.
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, KEY_F9);
  apply(KBD_FIFO_PRESSED, KEY_F10);
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_F4);         // the flag has caught up
  apply(KBD_FIFO_RELEASED, KEY_F5);
  CHECK_EQ_U32(pad_released(), PAD_A);
  check_nothing_held();
  // A HOLD of a shifted code after a clear (Shift not tracked yet) holds its
  // key's button quietly, as the key's own HOLD would.
  reset();
  clear_state();
  poll_start(false);
  apply(KBD_FIFO_HOLD, KEY_END);
  CHECK_EQ_U32(pad_pressed(), 0);
  CHECK_EQ_U32(btn.pad.curr, PAD_X);
  apply(KBD_FIFO_RELEASED, KEY_END);
  check_nothing_held();
}

// Left, Right, Backspace and Space send nothing under Shift: their button is
// released when Shift goes down, even though the key is still held. Held on,
// it comes back at the key's next repeat (PRESSED) once Shift is up.
static void test_shift_silences_keys(void) {
  static const struct {
    uint8_t key;
    uint32_t pad;
  } c[] = {{KEY_LEFT, PAD_LEFT}, {KEY_RIGHT, PAD_RIGHT},
           {KEY_BKSPC, PAD_Y}, {' ', PAD_B}};
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    // Released while Shift is held.
    reset();
    map.key[5][1] = ' ';  // B's alternate
    poll_start(false);
    apply(KBD_FIFO_PRESSED, c[i].key);
    CHECK_EQ_U32(pad_pressed(), c[i].pad);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
    CHECK_EQ_U32(pad_released(), c[i].pad);
    poll_start(false);                      // (the key's release: nothing)
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
    check_nothing_held();
    // Held through Shift.
    reset();
    map.key[5][1] = ' ';
    poll_start(false);
    apply(KBD_FIFO_PRESSED, c[i].key);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHR);
    poll_start(false);
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHR);
    apply(KBD_FIFO_PRESSED, c[i].key);      // its next repeat
    CHECK_EQ_U32(pad_pressed(), c[i].pad);
    poll_start(false);
    apply(KBD_FIFO_RELEASED, c[i].key);
    CHECK_EQ_U32(pad_released(), c[i].pad);
    check_nothing_held();
    // Pressed under Shift: silent, then reported once Shift is up.
    reset();
    map.key[5][1] = ' ';
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
    poll_start(false);
    apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
    apply(KBD_FIFO_PRESSED, c[i].key);
    CHECK_EQ_U32(pad_pressed(), c[i].pad);
    apply(KBD_FIFO_RELEASED, c[i].key);
    check_nothing_held();
  }
}

// Alt going down: B and Space then send nothing, I sends Insert. Their
// buttons are released at once. Insert without Shift is not Shift+Enter, so a
// held Enter keeps its button.
static void test_alt_pressed_mid_hold(void) {
  static const uint8_t keys[] = {'b', 'i', ' '};
  for (size_t i = 0; i < sizeof(keys); i++) {
    reset();
    map.key[4][1] = keys[i];     // A's alternate
    map.key[7][1] = KEY_ENTER;   // Y's alternate
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_ENTER);
    apply(KBD_FIFO_PRESSED, keys[i]);
    CHECK_EQ_U32(pad_pressed(), PAD_A | PAD_Y);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_ALT);
    CHECK_EQ_U32(pad_released(), PAD_A);
    poll_start(false);
    if (keys[i] == 'i')
      apply(KBD_FIFO_RELEASED, KEY_INSERT);  // I's release under Alt
    CHECK_EQ_U32(btn.pad.curr, PAD_Y);       // Enter still holds Y
    apply(KBD_FIFO_RELEASED, KEY_MOD_ALT);
    apply(KBD_FIFO_RELEASED, KEY_ENTER);
    check_nothing_held();
  }
  // Other letters only change case under Alt.
  reset();
  map.key[0][1] = 'w';
  poll_start(false);
  apply(KBD_FIFO_PRESSED, 'w');
  apply(KBD_FIFO_PRESSED, KEY_MOD_ALT);
  CHECK_EQ_U32(btn.pad.curr, PAD_UP);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, 'W');
  CHECK_EQ_U32(pad_released(), PAD_UP);
  apply(KBD_FIFO_RELEASED, KEY_MOD_ALT);
  check_nothing_held();
}

// Held while Alt is held: the controller rewrites only the press and the
// release of B, I and Space under Alt (B and Space silent, I as Insert); its
// repeats come as plain PRESSED 'B' / 'I' / ' '. They must not press the
// button (the release would never come); once Alt is up, the next repeat
// presses it and the real release releases it.
static void test_alt_held_key_repeats(void) {
  static const struct {
    uint8_t key, pressed, repeat, released; // what the controller sends
  } c[] = {{'b', 0, 'B', 0}, {'i', KEY_INSERT, 'I', KEY_INSERT},
           {' ', 0, ' ', 0}};
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    reset();
    map.key[4][1] = c[i].key;   // A's alternate
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_ALT);
    if (c[i].pressed)
      apply(KBD_FIFO_PRESSED, c[i].pressed);
    for (int r = 0; r < 4; r++) {           // 300 ms, then every 100 ms
      poll_start(false);
      apply(KBD_FIFO_PRESSED, c[i].repeat);
      CHECK_EQ_U32(btn.pad.curr, 0);
    }
    poll_start(false);
    if (c[i].released)
      apply(KBD_FIFO_RELEASED, c[i].released);
    apply(KBD_FIFO_RELEASED, KEY_MOD_ALT);
    check_nothing_held();
    // Held on after Alt is up: the next repeat presses, the release releases.
    reset();
    map.key[4][1] = c[i].key;
    poll_start(false);
    apply(KBD_FIFO_PRESSED, KEY_MOD_ALT);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, c[i].repeat);
    apply(KBD_FIFO_RELEASED, KEY_MOD_ALT);
    poll_start(false);
    apply(KBD_FIFO_PRESSED, c[i].key);      // Alt's flag has cleared
    CHECK_EQ_U32(pad_pressed(), PAD_A);
    poll_start(false);
    apply(KBD_FIFO_RELEASED, c[i].key);
    CHECK_EQ_U32(pad_released(), PAD_A);
    check_nothing_held();
  }
}

// I pressed within a scan of Alt going up while Shift is held: the
// controller's Alt flag lags a scan too, so the press still comes as Insert,
// which Shift would make Shift+Enter. Enter's button is only tapped by it,
// so nothing sticks when I's own release follows.
static void test_alt_release_lag_insert(void) {
  reset();
  map.key[7][1] = KEY_ENTER;   // Y's alternate
  map.key[4][1] = 'i';         // A's alternate
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, KEY_MOD_ALT);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_MOD_ALT);
  apply(KBD_FIFO_PRESSED, KEY_INSERT);   // I, Alt's flag not yet clear
  CHECK_EQ_U32(pad_pressed(), PAD_Y);    // a tap at most
  poll_start(false);
  CHECK_EQ_U32(pad_released(), PAD_Y);
  apply(KBD_FIFO_PRESSED, 'I');          // I's repeats, under Shift
  CHECK_EQ_U32(pad_pressed(), PAD_A);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, 'I');
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  check_nothing_held();
  // Shift+Enter held: tapped by its press, held from its first repeat on,
  // released with it.
  reset();
  map.key[7][1] = KEY_ENTER;
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, KEY_INSERT);
  CHECK_EQ_U32(pad_pressed(), PAD_Y);
  poll_start(false);
  apply(KBD_FIFO_HOLD, KEY_INSERT);
  CHECK_EQ_U32(btn.pad.curr, PAD_Y);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_INSERT);
  CHECK_EQ_U32(pad_released(), PAD_Y);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  check_nothing_held();
}

// Both Shift keys: the controller stays shifted while either is down.
static void test_two_shift_keys(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_F4);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHR);
  poll_start(false);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  apply(KBD_FIFO_RELEASED, KEY_F9);        // still shifted by the right one
  CHECK_EQ_U32(pad_released(), PAD_A);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHR);
  check_nothing_held();
}

// Shift and the menu key held across a clear (the system menu that Shift+F5
// opened closing): the menu key's HOLD reports never hold B.
static void test_menu_key_held_across_clear(void) {
  reset();
  poll_start(false);
  apply(KBD_FIFO_PRESSED, KEY_MOD_SHL);
  apply(KBD_FIFO_PRESSED, KEY_F10);
  CHECK_EQ_U32(btn.pad.curr, 0);
  clear_state();
  poll_start(false);
  apply(KBD_FIFO_HOLD, KEY_MOD_SHL);
  apply(KBD_FIFO_HOLD, KEY_F10);
  CHECK_EQ_U32(btn.pad.curr, 0);
  CHECK_EQ_U32(menu_edge(), 0);
  apply(KBD_FIFO_RELEASED, KEY_F10);
  apply(KBD_FIFO_RELEASED, KEY_MOD_SHL);
  check_nothing_held();
  CHECK_EQ_U32(pad_released(), 0);
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
  test_shift_pressed_mid_hold();
  test_shift_held_before_press();
  test_shift_release_lag();
  test_shift_silences_keys();
  test_alt_pressed_mid_hold();
  test_alt_held_key_repeats();
  test_alt_release_lag_insert();
  test_two_shift_keys();
  test_menu_key_held_across_clear();
  return check_report("test_kbd_pad");
}
