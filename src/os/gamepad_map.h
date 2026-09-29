#pragma once

// =============================================================================
// Gamepad bindings (picocalc.gamepad, g_api.gamepad)
//
// The map itself (kbd_padmap_t, keyboard.h) is 12 buttons x 2 slots of
// keycodes; the keyboard decode resolves keys through it. This module is the
// pure side: the one key-name table (getLabel, the JSON files, the Settings
// page) and the button names. gamepad.c installs the map at app launch.
// =============================================================================

#include "../drivers/keyboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Keys ─────────────────────────────────────────────────────────────────────
// Only keys whose release the keyboard reports under the keycode they were
// pressed with can be bound: arrows, Enter, Esc, Tab, Backspace, Delete,
// Space, F1-F9, letters (case-folded: Shift may come up first) and the Shift
// and Ctrl modifiers. Not bindable: the system menu key (F10) and Brk (the
// OS's), Sym/Fn and Alt (layer keys), digits and symbols (Shift changes
// their keycode while held, so their release could be missed).

// The key's name ("F4", "Del", "W"), or NULL when it cannot be bound.
const char *gamepad_key_name(uint8_t key);

// The keycode named `name` (any case; letters come back lower case), or 0.
uint8_t gamepad_key_from_name(const char *name);

// True when the key can be bound (it has a name).
static inline bool gamepad_key_bindable(uint8_t key) {
  return gamepad_key_name(key) != NULL;
}

// ── Buttons ──────────────────────────────────────────────────────────────────
// Button i is PAD_* bit i: up, down, left, right, a, b, x, y, l, r, start,
// select.

// The button index of a single PAD_* bit, or -1.
int gamepad_button_index(uint32_t pad_button);

// The name used in the bindings files ("up", "a", "select"), or NULL.
const char *gamepad_button_name(int button);

// The name to show ("Up", "A", "Select"), or NULL.
const char *gamepad_button_label(int button);

// The button named `name` (any case), or -1.
int gamepad_button_from_name(const char *name);

// ── Maps ─────────────────────────────────────────────────────────────────────

// The name of the key in `slot` (0 primary, 1 alternate) of the button
// `pad_button` (a single PAD_* bit) in *m; NULL if unbound or out of range.
const char *gamepad_map_label(const kbd_padmap_t *m, uint32_t pad_button,
                              int slot);
