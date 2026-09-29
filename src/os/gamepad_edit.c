#include "gamepad_edit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint8_t fold(uint8_t key) {
  return (key >= 'A' && key <= 'Z') ? (uint8_t)(key | 0x20) : key;
}

static bool is_modifier(uint8_t key) {
  return key == KEY_MOD_SHL || key == KEY_MOD_SHR || key == KEY_MOD_ALT ||
         key == KEY_MOD_SYM || key == KEY_MOD_CTRL;
}

static bool on_cell(const gamepad_edit_t *e) {
  return e->row >= 0 && e->row < KBD_PAD_BUTTONS;
}

static void notice_clear(gamepad_edit_t *e) { e->notice[0] = '\0'; }

__attribute__((format(printf, 2, 3))) static void
notice_set(gamepad_edit_t *e, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e->notice, sizeof(e->notice), fmt, ap);
  va_end(ap);
  e->notice_seq++;
}

void gamepad_edit_init(gamepad_edit_t *e, bool has_game) {
  memset(e, 0, sizeof(*e));
  gamepad_map_defaults(&e->global);
  e->has_game = has_game;
  e->this_game = has_game;
}

// The map the cells show: the effective map in This game, else the global.
static void view(const gamepad_edit_t *e, kbd_padmap_t *out) {
  if (e->this_game)
    gamepad_map_merge(out, &e->global, &e->game, e->game_mask);
  else
    *out = e->global;
}

// This game: the button now holds its effective slots in the override.
static void adopt(gamepad_edit_t *e, const kbd_padmap_t *v, int button) {
  for (int s = 0; s < KBD_PAD_SLOTS; s++)
    e->game.key[button][s] = v->key[button][s];
  e->game_mask |= (uint16_t)(1u << button);
  e->game_dirty = true;
}

void gamepad_edit_move(gamepad_edit_t *e, int dy, int dx) {
  int top = e->has_game ? GAMEPAD_EDIT_ROW_SCOPE : 0;
  int rows = KBD_PAD_BUTTONS - top;
  e->reset_armed = false;
  notice_clear(e);
  if (dy)
    e->row = (int8_t)(top + ((e->row - top + dy) % rows + rows) % rows);
  if (!dx)
    return;
  if (on_cell(e))
    e->slot = (int8_t)(e->slot ^ 1);
  else
    e->this_game = !e->this_game;
}

void gamepad_edit_enter(gamepad_edit_t *e, uint8_t held) {
  e->reset_armed = false;
  notice_clear(e);
  if (!on_cell(e)) {
    e->this_game = !e->this_game;
    return;
  }
  e->capturing = true;
  e->skip_key = fold(held);
}

void gamepad_edit_cancel(gamepad_edit_t *e) {
  e->capturing = false;
  e->reset_armed = false;
  notice_clear(e);
}

// Bind key to the focused cell (capture). The key is bindable.
static void bind_focused(gamepad_edit_t *e, uint8_t key) {
  int b = e->row, s = e->slot, fb = -1, fs = -1;
  if (e->this_game) {
    kbd_padmap_t v;
    view(e, &v);
    gamepad_map_bind(&v, b, s, key, &fb, &fs);
    adopt(e, &v, b);
    if (fb >= 0)
      adopt(e, &v, fb);
  } else {
    gamepad_map_bind(&e->global, b, s, key, &fb, &fs);
    e->global_dirty = true;
  }
  if (fb < 0) {
    notice_clear(e);
    return;
  }
  notice_set(e, "%s moved from %s%s to %s%s", gamepad_key_name(key),
             gamepad_button_label(fb), fs ? " Alt" : "",
             gamepad_button_label(b), s ? " Alt" : "");
}

bool gamepad_edit_clear(gamepad_edit_t *e) {
  e->reset_armed = false;
  notice_clear(e);
  if (!on_cell(e))
    return false;
  int b = e->row, s = e->slot;
  if (e->this_game) {
    kbd_padmap_t v;
    view(e, &v);
    if (!v.key[b][s])
      return false;
    v.key[b][s] = 0;
    adopt(e, &v, b);
  } else {
    if (!e->global.key[b][s])
      return false;
    e->global.key[b][s] = 0;
    e->global_dirty = true;
  }
  return true;
}

void gamepad_edit_reset(gamepad_edit_t *e) {
  const char *what = e->this_game ? "this game" : "to defaults";
  if (!e->reset_armed) {
    e->reset_armed = true;
    notice_set(e, "Press R again to reset %s", what);
    return;
  }
  e->reset_armed = false;
  if (e->this_game) {
    memset(&e->game, 0, sizeof(e->game));
    e->game_mask = 0;
    e->game_dirty = true;
    notice_set(e, "This game reset to All games");
  } else {
    gamepad_map_defaults(&e->global);
    e->global_dirty = true;
    notice_set(e, "Reset to defaults");
  }
}

bool gamepad_edit_key(gamepad_edit_t *e, uint8_t type, uint8_t key,
                      uint8_t flags) {
  uint8_t k = fold(key);
  if (type == KBD_EV_UP && k && k == e->skip_key) {
    e->skip_key = 0;
    return false;
  }
  if (type != KBD_EV_DOWN || (flags & KBD_EVF_REPEAT) || !k ||
      k == e->skip_key)
    return false;
  e->skip_key = k;  // its repeats (fresh presses on this keyboard) are not
  if (e->capturing) {
    if (!gamepad_key_bindable(key)) {
      char buf[8];
      notice_set(e, "%s can't be bound",
                 gamepad_edit_key_label(key, buf, sizeof(buf)));
      return true;
    }
    e->capturing = false;
    bind_focused(e, key);
    return true;
  }
  if (k == 'r') {
    gamepad_edit_reset(e);
    return true;
  }
  if (k == 'c') {
    gamepad_edit_clear(e);
    return true;
  }
  if (!is_modifier(key) && e->reset_armed) {
    e->reset_armed = false;
    notice_clear(e);
    return true;
  }
  return false;
}

uint8_t gamepad_edit_cell(const gamepad_edit_t *e, int button, int slot,
                          bool *inherited) {
  if (inherited)
    *inherited = e->this_game && !(e->game_mask & (1u << button));
  if (button < 0 || button >= KBD_PAD_BUTTONS || slot < 0 ||
      slot >= KBD_PAD_SLOTS)
    return 0;
  kbd_padmap_t v;
  view(e, &v);
  return v.key[button][slot];
}

uint16_t gamepad_edit_global_mask(const gamepad_edit_t *e) {
  kbd_padmap_t d;
  uint16_t mask = 0;
  gamepad_map_defaults(&d);
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    if (memcmp(e->global.key[b], d.key[b], sizeof(d.key[b])) != 0)
      mask |= (uint16_t)(1u << b);
  return mask;
}

const char *gamepad_edit_key_label(uint8_t key, char *buf, size_t n) {
  const char *name = gamepad_key_name(key);
  if (name)
    return name;
  switch (key) {
  case KEY_MOD_SHL:
  case KEY_MOD_SHR:
    return "Shift";
  case KEY_MOD_ALT:
    return "Alt";
  case KEY_MOD_SYM:
    return "Sym";
  case KEY_F10:
    return "F10";
  case KEY_BRK:
    return "Brk";
  case KEY_INSERT:
    return "Insert";
  case KEY_HOME:
    return "Home";
  case KEY_END:
    return "End";
  case KEY_PGUP:
    return "PgUp";
  case KEY_PGDN:
    return "PgDn";
  default:
    break;
  }
  if (key >= KEY_F6 && key <= KEY_F9)
    snprintf(buf, n, "F%d", key - KEY_F1 + 1);
  else if (key > ' ' && key < 0x7F)
    snprintf(buf, n, "%c", key);
  else
    snprintf(buf, n, "Key %02X", key);
  return buf;
}
