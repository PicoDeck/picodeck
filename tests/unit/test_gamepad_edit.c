// Host unit tests for src/os/gamepad_edit.c: the Settings -> Controls
// page's state. Cursor movement and the scope row, key capture from keyboard
// events (the Enter that starts it, repeats, refused keys, the menu key),
// the one-button-per-key rule with its "moved from" notice, the This game
// override (inherited -> overridden, including a key moved off an inherited
// button), Clear, Reset, and what the global file has to list.
#include "check.h"
#include "gamepad_edit.h"
#include "os.h"

#include <string.h>

enum { UP, DOWN, LEFT, RIGHT, A, B, X, Y, L, R, START, SELECT };

static kbd_padmap_t defaults(void) {
  kbd_padmap_t m;
  gamepad_map_defaults(&m);
  return m;
}

static bool map_eq(const kbd_padmap_t *a, const kbd_padmap_t *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

// A key tapped: down then up.
static void tap(gamepad_edit_t *e, uint8_t key) {
  gamepad_edit_key(e, KBD_EV_DOWN, key, 0);
  gamepad_edit_key(e, KBD_EV_UP, key, 0);
}

static void down(gamepad_edit_t *e, uint8_t key) {
  gamepad_edit_key(e, KBD_EV_DOWN, key, 0);
}

static void up(gamepad_edit_t *e, uint8_t key) {
  gamepad_edit_key(e, KBD_EV_UP, key, 0);
}

// Put the cursor on (button, slot), from wherever it is.
static void go(gamepad_edit_t *e, int button, int slot) {
  while (e->row != button)
    gamepad_edit_move(e, 1, 0);
  if (e->slot != slot)
    gamepad_edit_move(e, 0, 1);
}

// Enter on the focused cell (released at once), then a tap of key.
static void bind(gamepad_edit_t *e, uint8_t key) {
  gamepad_edit_enter(e, KEY_ENTER);
  up(e, KEY_ENTER);
  tap(e, key);
}

// A global-only page (the launcher) with the defaults.
static gamepad_edit_t launcher(void) {
  gamepad_edit_t e;
  gamepad_edit_init(&e, false);
  return e;
}

// ── Cursor ───────────────────────────────────────────────────────────────────

static void test_init_and_cursor(void) {
  gamepad_edit_t e = launcher();
  kbd_padmap_t d = defaults();
  CHECK(map_eq(&e.global, &d));
  CHECK_EQ_U32(e.game_mask, 0);
  CHECK(!e.this_game);
  CHECK(!e.capturing);
  CHECK(!e.global_dirty && !e.game_dirty);
  CHECK_EQ_INT(e.row, UP);
  CHECK_EQ_INT(e.slot, 0);
  CHECK_STR(e.notice, "");
  // No scope row at the launcher: Up wraps to Select and back.
  gamepad_edit_move(&e, -1, 0);
  CHECK_EQ_INT(e.row, SELECT);
  gamepad_edit_move(&e, 1, 0);
  CHECK_EQ_INT(e.row, UP);
  // Left / Right swap the slots.
  gamepad_edit_move(&e, 0, 1);
  CHECK_EQ_INT(e.slot, 1);
  gamepad_edit_move(&e, 0, 1);
  CHECK_EQ_INT(e.slot, 0);
  gamepad_edit_move(&e, 0, -1);
  CHECK_EQ_INT(e.slot, 1);

  // In a game: This game first, and the scope row above Up.
  gamepad_edit_init(&e, true);
  CHECK(e.this_game);
  CHECK_EQ_INT(e.row, UP);
  gamepad_edit_move(&e, -1, 0);
  CHECK_EQ_INT(e.row, GAMEPAD_EDIT_ROW_SCOPE);
  gamepad_edit_move(&e, -1, 0);
  CHECK_EQ_INT(e.row, SELECT);
  gamepad_edit_move(&e, 1, 0);
  CHECK_EQ_INT(e.row, GAMEPAD_EDIT_ROW_SCOPE);
  // Left / Right, and Enter, flip the scope there; Enter does not capture.
  gamepad_edit_move(&e, 0, 1);
  CHECK(!e.this_game);
  gamepad_edit_move(&e, 0, -1);
  CHECK(e.this_game);
  gamepad_edit_enter(&e, KEY_ENTER);
  CHECK(!e.this_game);
  CHECK(!e.capturing);
  gamepad_edit_enter(&e, KEY_ENTER);
  CHECK(e.this_game);
}

// ── Capture ──────────────────────────────────────────────────────────────────

// The Enter that starts capture is not the binding, however long it is held
// (the keyboard repeats Enter as fresh presses): the next key is. Once
// Enter is up, Enter itself can be bound.
static void test_capture_skips_the_starting_enter(void) {
  gamepad_edit_t e = launcher();
  go(&e, A, 0);
  gamepad_edit_enter(&e, KEY_ENTER);
  CHECK(e.capturing);
  down(&e, KEY_ENTER);                     // a repeat, not flagged
  down(&e, KEY_ENTER);
  CHECK(e.capturing);
  CHECK_EQ_INT(e.global.key[A][0], KEY_F4);
  up(&e, KEY_ENTER);
  down(&e, 'Z');                           // letters bind, lower case
  CHECK(!e.capturing);
  CHECK_EQ_INT(e.global.key[A][0], 'z');
  CHECK(e.global_dirty);
  CHECK_STR(e.notice, "");
  // F4 is gone from the map: nothing else held it.
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    CHECK(e.global.key[b][0] != KEY_F4 && e.global.key[b][1] != KEY_F4);

  // Enter bound to B once it has been released.
  go(&e, B, 0);
  gamepad_edit_enter(&e, KEY_ENTER);
  up(&e, KEY_ENTER);
  down(&e, KEY_ENTER);
  CHECK(!e.capturing);
  CHECK_EQ_INT(e.global.key[B][0], KEY_ENTER);

  // Enter released before capture started (held = 0): the next Enter binds.
  go(&e, X, 1);
  gamepad_edit_enter(&e, 0);
  down(&e, KEY_ENTER);
  CHECK(!e.capturing);
  CHECK_EQ_INT(e.global.key[X][1], KEY_ENTER);
  CHECK_EQ_INT(e.global.key[B][0], 0);     // moved from B
}

// Repeats never bind; ups and chars are not key presses.
static void test_capture_ignores_repeats_ups_and_chars(void) {
  gamepad_edit_t e = launcher();
  go(&e, A, 1);
  gamepad_edit_enter(&e, 0);
  gamepad_edit_key(&e, KBD_EV_DOWN, KEY_F1, KBD_EVF_REPEAT);
  gamepad_edit_key(&e, KBD_EV_UP, 'w', 0);
  gamepad_edit_key(&e, KBD_EV_CHAR, 'w', 0);
  CHECK(e.capturing);
  CHECK_EQ_INT(e.global.key[A][1], 0);
  down(&e, 'w');
  CHECK_EQ_INT(e.global.key[A][1], 'w');
}

// A key on another slot moves, with a notice naming both.
static void test_capture_moves_the_key(void) {
  gamepad_edit_t e = launcher();
  go(&e, A, 0);
  bind(&e, KEY_F5);
  CHECK_EQ_INT(e.global.key[A][0], KEY_F5);
  CHECK_EQ_INT(e.global.key[B][0], 0);
  CHECK_STR(e.notice, "F5 moved from B to A");
  uint8_t seq = e.notice_seq;
  // Between the slots of one button, and from an alternate slot.
  go(&e, A, 1);
  bind(&e, KEY_F5);
  CHECK_STR(e.notice, "F5 moved from A to A Alt");
  CHECK(e.notice_seq != seq);
  go(&e, SELECT, 1);
  bind(&e, KEY_F5);
  CHECK_STR(e.notice, "F5 moved from A Alt to Select Alt");
  CHECK_EQ_INT(e.global.key[A][1], 0);
  // Into the slot that already has it: nothing moves.
  bind(&e, KEY_F5);
  CHECK_STR(e.notice, "");
  CHECK_EQ_INT(e.global.key[SELECT][1], KEY_F5);
  // Moving the cursor clears a notice.
  bind(&e, KEY_TAB);                       // Tab was Select's primary
  CHECK_STR(e.notice, "Tab moved from Select to Select Alt");
  gamepad_edit_move(&e, 1, 0);
  CHECK_STR(e.notice, "");
}

// Every key gamepad_key_bindable refuses is refused by name; capture goes
// on, the map is unchanged, and the next bindable key binds. Shift, Alt and
// Sym are refused without a notice (they start chords: the menu key is
// Shift+F5). Esc and Ctrl bind.
static void test_capture_refuses_unbindable_keys(void) {
  static const struct {
    uint8_t key;
    const char *notice;
  } refused[] = {
      {KEY_F6, "F6 can't be bound"},         {KEY_F9, "F9 can't be bound"},
      {KEY_PGUP, "PgUp can't be bound"},     {KEY_INSERT, "Insert can't be bound"},
      {KEY_END, "End can't be bound"},       {KEY_HOME, "Home can't be bound"},
      {'1', "1 can't be bound"},             {'!', "! can't be bound"},
      {';', "; can't be bound"},             {0x9A, "Key 9A can't be bound"},
  };
  gamepad_edit_t e = launcher();
  kbd_padmap_t before = e.global;
  go(&e, A, 0);
  gamepad_edit_enter(&e, 0);
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
    CHECK(!gamepad_key_bindable(refused[i].key));
    uint8_t seq = e.notice_seq;
    CHECK(gamepad_edit_key(&e, KBD_EV_DOWN, refused[i].key, 0));
    CHECK_STR(e.notice, refused[i].notice);
    CHECK(e.notice_seq != seq);
    CHECK(e.capturing);
    up(&e, refused[i].key);
  }
  static const uint8_t mods[] = {KEY_MOD_SHL, KEY_MOD_SHR, KEY_MOD_ALT,
                                 KEY_MOD_SYM};
  for (size_t i = 0; i < sizeof(mods); i++) {
    CHECK(!gamepad_key_bindable(mods[i]));
    uint8_t seq = e.notice_seq;
    CHECK(!gamepad_edit_key(&e, KBD_EV_DOWN, mods[i], 0));
    CHECK(e.notice_seq == seq);
    CHECK(e.capturing);
    up(&e, mods[i]);
  }
  CHECK(map_eq(&e.global, &before));
  CHECK(!e.global_dirty);
  down(&e, KEY_ESC);
  CHECK(!e.capturing);
  CHECK_EQ_INT(e.global.key[A][0], KEY_ESC);
  CHECK_STR(e.notice, "");
  // Ctrl is a key like any other.
  go(&e, B, 1);
  bind(&e, KEY_MOD_CTRL);
  CHECK_EQ_INT(e.global.key[B][1], KEY_MOD_CTRL);
}

// The menu key ends capture with nothing bound.
static void test_cancel(void) {
  gamepad_edit_t e = launcher();
  go(&e, A, 0);
  gamepad_edit_enter(&e, KEY_ENTER);
  down(&e, KEY_MOD_SHL);                   // Shift (the menu key is Shift+F5)
  CHECK_STR(e.notice, "");                 // no notice for it
  CHECK(e.capturing);
  gamepad_edit_cancel(&e);
  CHECK(!e.capturing);
  CHECK_STR(e.notice, "");
  CHECK_EQ_INT(e.global.key[A][0], KEY_F4);
  CHECK(!e.global_dirty);
  tap(&e, 'z');                            // not capturing: binds nothing
  CHECK_EQ_INT(e.global.key[A][0], KEY_F4);
}

// ── Commands outside capture ─────────────────────────────────────────────────

static void test_clear(void) {
  gamepad_edit_t e = launcher();
  go(&e, UP, 0);
  CHECK(gamepad_edit_clear(&e));
  CHECK_EQ_INT(e.global.key[UP][0], 0);
  CHECK(e.global_dirty);
  CHECK(!gamepad_edit_clear(&e));          // already unbound
  // C (either case) clears the focused cell; Backspace does not.
  go(&e, Y, 0);
  tap(&e, KEY_BKSPC);
  CHECK_EQ_INT(e.global.key[Y][0], KEY_BKSPC);
  tap(&e, 'C');
  CHECK_EQ_INT(e.global.key[Y][0], 0);
  // The scope row has nothing to clear.
  gamepad_edit_init(&e, true);
  gamepad_edit_move(&e, -1, 0);
  CHECK(!gamepad_edit_clear(&e));
  CHECK(!e.game_dirty && !e.global_dirty);
}

// R arms, a second R resets; anything else in between disarms. A held R
// (repeated as fresh presses) resets nothing, and neither does a held C
// clear twice.
static void test_reset_global(void) {
  gamepad_edit_t e = launcher();
  kbd_padmap_t d = defaults();
  go(&e, A, 0);
  bind(&e, 'z');
  e.global_dirty = false;
  tap(&e, 'r');
  CHECK(e.reset_armed);
  CHECK_STR(e.notice, "Press R again to reset to defaults");
  CHECK_EQ_INT(e.global.key[A][0], 'z');
  gamepad_edit_move(&e, 1, 0);             // disarms
  CHECK(!e.reset_armed);
  tap(&e, 'r');
  tap(&e, 'x');                            // any other key disarms too
  CHECK(!e.reset_armed);
  down(&e, 'r');
  down(&e, 'r');                           // held: one press
  CHECK(e.reset_armed);
  CHECK_EQ_INT(e.global.key[A][0], 'z');
  up(&e, 'r');
  tap(&e, 'R');
  CHECK(map_eq(&e.global, &d));
  CHECK(e.global_dirty);
  CHECK(!e.reset_armed);
  CHECK_STR(e.notice, "Reset to defaults");
  CHECK_EQ_U32(gamepad_edit_global_mask(&e), 0);
}

// After binding a letter, its own repeats are not commands.
static void test_bound_key_repeat_is_not_a_command(void) {
  gamepad_edit_t e = launcher();
  go(&e, A, 0);
  gamepad_edit_enter(&e, 0);
  down(&e, 'c');
  down(&e, 'c');                           // still held: not Clear
  CHECK_EQ_INT(e.global.key[A][0], 'c');
  up(&e, 'c');
  tap(&e, 'c');                            // pressed again: Clear
  CHECK_EQ_INT(e.global.key[A][0], 0);
}

// ── This game ────────────────────────────────────────────────────────────────

// Global: the defaults with W on Up's alternate. Override: A = Z.
static gamepad_edit_t in_game(void) {
  gamepad_edit_t e;
  gamepad_edit_init(&e, true);
  e.global.key[UP][1] = 'w';
  memset(&e.game, 0, sizeof(e.game));
  e.game.key[A][0] = 'z';
  e.game_mask = 1u << A;
  return e;
}

static void test_this_game_cells(void) {
  gamepad_edit_t e = in_game();
  bool inh = false;
  CHECK_EQ_INT(gamepad_edit_cell(&e, A, 0, &inh), 'z');
  CHECK(!inh);
  CHECK_EQ_INT(gamepad_edit_cell(&e, A, 1, &inh), 0);
  CHECK(!inh);
  CHECK_EQ_INT(gamepad_edit_cell(&e, B, 0, &inh), KEY_F5);
  CHECK(inh);
  CHECK_EQ_INT(gamepad_edit_cell(&e, UP, 1, &inh), 'w');
  CHECK(inh);
  // All games shows the global map, nothing inherited.
  e.this_game = false;
  CHECK_EQ_INT(gamepad_edit_cell(&e, A, 0, &inh), KEY_F4);
  CHECK(!inh);
  CHECK_EQ_INT(gamepad_edit_cell(&e, UP, 1, &inh), 'w');
  CHECK(!inh);
}

// Binding a slot of an inherited button makes it an override holding its
// effective slots; a key moved off an inherited button makes that one an
// override too. The global map is untouched.
static void test_this_game_bind_inherited_to_overridden(void) {
  gamepad_edit_t e = in_game();
  kbd_padmap_t global = e.global;
  go(&e, UP, 1);
  bind(&e, 'q');
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << UP));
  CHECK_EQ_INT(e.game.key[UP][0], KEY_UP);   // the inherited primary kept
  CHECK_EQ_INT(e.game.key[UP][1], 'q');
  CHECK(e.game_dirty);
  CHECK(!e.global_dirty);
  CHECK(map_eq(&e.global, &global));

  // F5 moves off inherited B onto inherited X: both become overrides.
  go(&e, X, 0);
  bind(&e, KEY_F5);
  CHECK_STR(e.notice, "F5 moved from B to X");
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << UP) | (1u << B) | (1u << X));
  CHECK_EQ_INT(e.game.key[X][0], KEY_F5);
  CHECK_EQ_INT(e.game.key[X][1], 0);
  CHECK_EQ_INT(e.game.key[B][0], 0);
  CHECK_EQ_INT(e.game.key[B][1], 0);
  bool inh = true;
  CHECK_EQ_INT(gamepad_edit_cell(&e, B, 0, &inh), 0);
  CHECK(!inh);
  CHECK(map_eq(&e.global, &global));

  // A key moved between overridden buttons stays within the override.
  go(&e, B, 1);
  bind(&e, 'z');
  CHECK_STR(e.notice, "Z moved from A to B Alt");
  CHECK_EQ_INT(e.game.key[A][0], 0);
  CHECK_EQ_INT(e.game.key[B][1], 'z');

  // The effective map keeps one button per key.
  kbd_padmap_t eff;
  gamepad_map_merge(&eff, &e.global, &e.game, e.game_mask);
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    for (int s = 0; s < KBD_PAD_SLOTS; s++)
      for (int b2 = 0; b2 < KBD_PAD_BUTTONS; b2++)
        for (int s2 = 0; s2 < KBD_PAD_SLOTS; s2++)
          if ((b != b2 || s != s2) && eff.key[b][s])
            CHECK(eff.key[b][s] != eff.key[b2][s2]);
}

// A global key the override hides (A's F4 under A = Z) is free in this game.
static void test_this_game_hidden_global_key(void) {
  gamepad_edit_t e = in_game();
  go(&e, B, 0);
  bind(&e, KEY_F4);
  CHECK_STR(e.notice, "");
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << B));
  CHECK_EQ_INT(e.game.key[A][0], 'z');
  CHECK_EQ_INT(e.game.key[B][0], KEY_F4);
}

static void test_this_game_clear_and_reset(void) {
  gamepad_edit_t e = in_game();
  kbd_padmap_t global = e.global;
  // Clearing an inherited slot overrides its button, the other slot kept.
  go(&e, UP, 1);
  CHECK(gamepad_edit_clear(&e));
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << UP));
  CHECK_EQ_INT(e.game.key[UP][0], KEY_UP);
  CHECK_EQ_INT(e.game.key[UP][1], 0);
  // An unbound inherited slot: nothing to clear, no override made.
  go(&e, B, 1);
  CHECK(!gamepad_edit_clear(&e));
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << UP));
  // Reset this game: every button inherited again, the global map kept.
  e.game_dirty = false;
  gamepad_edit_reset(&e);
  CHECK_STR(e.notice, "Press R again to reset this game");
  CHECK_EQ_U32(e.game_mask, (1u << A) | (1u << UP));
  gamepad_edit_reset(&e);
  CHECK_EQ_U32(e.game_mask, 0);
  CHECK(e.game_dirty);
  CHECK(!e.global_dirty);
  CHECK_STR(e.notice, "This game reset to All games");
  CHECK(map_eq(&e.global, &global));
  bool inh = false;
  CHECK_EQ_INT(gamepad_edit_cell(&e, A, 0, &inh), KEY_F4);
  CHECK(inh);
  // With no override at all, Reset this game still deletes the file (a
  // corrupt one, say).
  gamepad_edit_t f = in_game();
  f.game_mask = 0;
  gamepad_edit_reset(&f);
  gamepad_edit_reset(&f);
  CHECK(f.game_dirty);
}

// All games inside a game edits the global map only.
static void test_all_games_in_game(void) {
  gamepad_edit_t e = in_game();
  kbd_padmap_t game = e.game;
  gamepad_edit_move(&e, -1, 0);
  gamepad_edit_enter(&e, KEY_ENTER);         // flip to All games
  CHECK(!e.this_game);
  up(&e, KEY_ENTER);
  go(&e, A, 0);
  bind(&e, 'k');
  CHECK_EQ_INT(e.global.key[A][0], 'k');
  CHECK(e.global_dirty);
  CHECK(!e.game_dirty);
  CHECK_EQ_U32(e.game_mask, 1u << A);
  CHECK(map_eq(&e.game, &game));
  gamepad_edit_reset(&e);
  CHECK_STR(e.notice, "Press R again to reset to defaults");
  gamepad_edit_reset(&e);
  CHECK_EQ_INT(e.global.key[A][0], KEY_F4);
  CHECK_EQ_U32(e.game_mask, 1u << A);
}

// ── What the global file lists ───────────────────────────────────────────────

static void test_global_mask(void) {
  gamepad_edit_t e = launcher();
  CHECK_EQ_U32(gamepad_edit_global_mask(&e), 0);
  go(&e, A, 0);
  bind(&e, 'z');
  CHECK_EQ_U32(gamepad_edit_global_mask(&e), 1u << A);
  bind(&e, KEY_F5);                          // F5 moved from B
  CHECK_EQ_U32(gamepad_edit_global_mask(&e), (1u << A) | (1u << B));
  // The file those buttons make reads back, over the defaults, as the map.
  char buf[GAMEPAD_FILE_MAX];
  CHECK(gamepad_map_format(buf, sizeof(buf), &e.global,
                           gamepad_edit_global_mask(&e)) > 0);
  kbd_padmap_t back = defaults();
  uint16_t listed = 0;
  CHECK(gamepad_map_parse(buf, &back, &listed, "t"));
  CHECK(map_eq(&back, &e.global));
}

static void test_key_labels(void) {
  char buf[8];
  CHECK_STR(gamepad_edit_key_label(KEY_F4, buf, sizeof(buf)), "F4");
  CHECK_STR(gamepad_edit_key_label('w', buf, sizeof(buf)), "W");
  CHECK_STR(gamepad_edit_key_label(KEY_MOD_SHR, buf, sizeof(buf)), "Shift");
  CHECK_STR(gamepad_edit_key_label(KEY_F10, buf, sizeof(buf)), "F10");
  CHECK_STR(gamepad_edit_key_label(KEY_BRK, buf, sizeof(buf)), "Brk");
  CHECK_STR(gamepad_edit_key_label(KEY_PGDN, buf, sizeof(buf)), "PgDn");
  CHECK_STR(gamepad_edit_key_label('0', buf, sizeof(buf)), "0");
  CHECK_STR(gamepad_edit_key_label(0x01, buf, sizeof(buf)), "Key 01");
}

int main(void) {
  test_init_and_cursor();
  test_capture_skips_the_starting_enter();
  test_capture_ignores_repeats_ups_and_chars();
  test_capture_moves_the_key();
  test_capture_refuses_unbindable_keys();
  test_cancel();
  test_clear();
  test_reset_global();
  test_bound_key_repeat_is_not_a_command();
  test_this_game_cells();
  test_this_game_bind_inherited_to_overridden();
  test_this_game_hidden_global_key();
  test_this_game_clear_and_reset();
  test_all_games_in_game();
  test_global_mask();
  test_key_labels();
  return check_report("test_gamepad_edit");
}
