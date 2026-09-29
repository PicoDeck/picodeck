#pragma once

// =============================================================================
// Settings -> Controls: the page's state (system_menu.c loads, draws, saves)
//
// The page edits two maps: the global one (/system/gamepad.json over the
// defaults) and, inside a running game, that game's override
// (/data/<app_id>/gamepad.json: the buttons in game_mask; the rest of `game`
// is unused). The scope row picks which one the cells edit:
//   - All games: the cells show and edit the global map.
//   - This game: the cells show the effective map (gamepad_map_merge); a
//     button not in the override is inherited (drawn dimmed). Binding or
//     clearing a slot copies its button's effective slots into the override
//     first, and a key moved off another button makes that button an
//     override too, so what the override changes is always explicit.
// One button per key (gamepad_map_bind) within the map being edited.
//
// Keys reach the page as keyboard events (kbd_poll_event: keycodes, so
// letters bind), and the rule for which keys bind is gamepad_key_bindable()
// alone. Pure: no SD card, no display. Host-tested in
// tests/unit/test_gamepad_edit.c.
// =============================================================================

#include "gamepad_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GAMEPAD_EDIT_ROW_SCOPE (-1) // the This game / All games row
#define GAMEPAD_EDIT_NOTICE_MAX 48

typedef struct {
  kbd_padmap_t global;  // the defaults + /system/gamepad.json
  kbd_padmap_t game;    // the override's buttons (game_mask)
  uint16_t game_mask;   // PAD_* bits the override lists
  bool has_game;        // opened inside a running game: the scope row exists
  bool this_game;       // the cells edit the override, not the global map
  int8_t row;           // GAMEPAD_EDIT_ROW_SCOPE or a button, 0-11
  int8_t slot;          // 0 primary, 1 alternate
  bool capturing;       // the focused cell waits for a key
  bool reset_armed;     // R pressed once: the next R resets
  uint8_t skip_key;     // its key-downs are ignored until it goes up
  bool global_dirty;    // save the global map on leaving the page
  bool game_dirty;      // save (or, with no buttons left, delete) the override
  uint8_t notice_seq;   // bumped whenever notice is set
  char notice[GAMEPAD_EDIT_NOTICE_MAX]; // one line under the rows; "" = none
} gamepad_edit_t;

// The defaults and no override, the cursor on Up / Primary, This game when
// has_game. The caller then reads the files into global, game and game_mask.
void gamepad_edit_init(gamepad_edit_t *e, bool has_game);

// Arrow keys: dy rows (wrapping; the scope row sits above Up when has_game),
// dx slots (Primary <-> Alt; on the scope row, dx flips the scope).
void gamepad_edit_move(gamepad_edit_t *e, int dy, int dx);

// Enter: flips the scope on the scope row; on a cell starts capture. `held`
// is the key still down that started it (Enter), or 0: its key-downs (the
// keyboard repeats Enter as fresh presses) are ignored until it goes up.
void gamepad_edit_enter(gamepad_edit_t *e, uint8_t held);

// One keyboard event (KBD_EV_* type, keycode, KBD_EVF_* flags). Repeats and
// the downs of skip_key are ignored.
//   Capturing: a bindable key is bound to the focused cell, ending capture
//   ("F4 moved from A to B" when it leaves another slot). Any other key is
//   refused ("1 can't be bound") and capture goes on; Shift, Alt and Sym
//   are ignored without a notice, as they start chords (the menu key is
//   Shift+F5).
//   Otherwise: C clears the focused cell, R resets (gamepad_edit_reset).
// Returns true when the page has to be redrawn.
bool gamepad_edit_key(gamepad_edit_t *e, uint8_t type, uint8_t key,
                      uint8_t flags);

// The menu key while capturing: capture ends, nothing is bound.
void gamepad_edit_cancel(gamepad_edit_t *e);

// Unbind the focused cell. False (nothing changes) when it is already
// unbound or the scope row is focused.
bool gamepad_edit_clear(gamepad_edit_t *e);

// The first call arms (a notice asks for R again), the second resets: All
// games, the global map to the defaults; This game, no override at all (the
// file is deleted on save). Any other action disarms.
void gamepad_edit_reset(gamepad_edit_t *e);

// The key a cell shows (0 unbound) and, in This game, whether its button is
// inherited from the global map.
uint8_t gamepad_edit_cell(const gamepad_edit_t *e, int button, int slot,
                          bool *inherited);

// The global map's buttons that differ from the defaults: what the global
// file has to list (0: the file can go).
uint16_t gamepad_edit_global_mask(const gamepad_edit_t *e);

// A name for any keycode, for the notices: gamepad_key_name for the bindable
// keys, else "Shift", "Alt", "F6", "1", ... (into buf, n >= 8, if needed).
const char *gamepad_edit_key_label(uint8_t key, char *buf, size_t n);
