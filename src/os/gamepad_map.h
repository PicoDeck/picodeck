#pragma once

// =============================================================================
// Gamepad bindings (picocalc.gamepad, g_api.gamepad)
//
// The map itself (kbd_padmap_t, keyboard.h) is 12 buttons x 2 slots of
// keycodes; the keyboard decode resolves keys through it. This module holds
// the one key-name table (getLabel, the bindings files, the Settings page),
// the button names, the one-button-per-key rule, and the bindings files:
//   /system/gamepad.json          the global map (over the defaults)
//   /data/<app_id>/gamepad.json   one app's override: only the buttons it
//                                 lists; the others come from the global map
// A missing or corrupt file means the defaults (no override), never a failed
// launch. gamepad.c builds the effective map at every app launch.
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

// The default bindings (KBD_PAD_DEFAULT_MAP).
void gamepad_map_defaults(kbd_padmap_t *m);

// The one-button-per-key rule: put `key` in (button, slot) of *m and take it
// off every other slot of *m. key 0 clears the slot. Letters are stored lower
// case. Returns false, *m unchanged, for a key that cannot be bound or a
// button/slot out of range. *from_button / *from_slot (either may be NULL)
// get the slot the key was taken from, or -1 when it was not bound elsewhere
// (for a "F4 moved from A to B" notice).
bool gamepad_map_bind(kbd_padmap_t *m, int button, int slot, uint8_t key,
                      int *from_button, int *from_slot);

// The effective map: *global with the buttons in ovr_mask replaced by *ovr's,
// and every key an overridden button uses taken off the inherited buttons
// (one button per key). out may be global.
void gamepad_map_merge(kbd_padmap_t *out, const kbd_padmap_t *global,
                       const kbd_padmap_t *ovr, uint16_t ovr_mask);

// ── Bindings files ───────────────────────────────────────────────────────────
// Human-editable JSON: {"a": ["F4"], "up": ["Up", "W"], "x": []}. Button names
// as gamepad_button_name, key names as gamepad_key_name, both in any case;
// the array holds the primary then the alternate key, and [], "" or null
// leave a slot unbound. A bare string is a primary alone.

#define GAMEPAD_GLOBAL_PATH "/system/gamepad.json"
// "/data/" + a 79-character app id + "/gamepad.json" + NUL.
#define GAMEPAD_PATH_MAX 100
// A bigger file is refused as corrupt; a full map is ~350 bytes.
#define GAMEPAD_FILE_MAX 2048

// Apply a bindings file's text to *m: each listed button's two slots are
// replaced, key by key through gamepad_map_bind (so a key listed twice ends on
// the later button, and a key taken from an unlisted button leaves it), and
// its bit is set in *listed. Unknown button names are skipped, unknown or
// unbindable key names leave their slot unbound; both are logged, naming
// `what`. Returns false, with *m and *listed untouched, when the text is not
// such an object.
bool gamepad_map_parse(const char *json, kbd_padmap_t *m, uint16_t *listed,
                       const char *what);

// The buttons in `mask` of *m as a bindings file, one button per line, into
// buf (NUL-terminated). Returns the length, or -1 when cap is too small
// (GAMEPAD_FILE_MAX always fits).
int gamepad_map_format(char *buf, size_t cap, const kbd_padmap_t *m,
                       uint16_t mask);

// "/data/<app_id>/gamepad.json" into out; false when it does not fit.
bool gamepad_app_path(char *out, size_t n, const char *app_id);

// Read a bindings file onto *m (gamepad_map_parse; start from the defaults
// for the global file, from an empty map for an override). Returns false and
// leaves *m and *listed alone when the file is missing (quietly) or too big,
// unreadable or corrupt (logged). Finishes a save a power cut interrupted
// first (sd_atomic_recover).
bool gamepad_load(const char *path, kbd_padmap_t *m, uint16_t *listed);

// Write the buttons in `mask` of *m to path through a .tmp file and a rename
// (sd_atomic_write, as the config stores), creating the file's directory
// (/data/<app_id>) when missing. False (logged) keeps the previous file. The
// scratch buffer is umm_malloc'd.
bool gamepad_save(const char *path, const kbd_padmap_t *m, uint16_t mask);

// Delete a bindings file (and a .bak an interrupted save left, which a later
// load would restore). True when neither is left.
bool gamepad_remove(const char *path);

// The effective map for app_id (NULL: no app, the launcher): the defaults,
// then /system/gamepad.json, then the app's override for the buttons it lists
// (gamepad_map_merge). Reads the SD card; its frames stay small, but the SD
// stack below it is deep: run it on an app or OS stack (gamepad_apply does).
void gamepad_load_effective(kbd_padmap_t *out, const char *app_id);
