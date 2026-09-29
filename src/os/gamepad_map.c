#include "gamepad_map.h"

#include <string.h>
#include <strings.h>

// ── Key names ────────────────────────────────────────────────────────────────
// The one table of bindable keys (letters are generated below). Short names:
// they label on-screen hints and the Settings page's cells.

typedef struct {
  uint8_t key;
  char name[7];
} gamepad_key_t;

static const gamepad_key_t k_keys[] = {
    {KEY_UP, "Up"},        {KEY_DOWN, "Down"},    {KEY_LEFT, "Left"},
    {KEY_RIGHT, "Right"},  {KEY_ENTER, "Enter"},  {KEY_ESC, "Esc"},
    {KEY_TAB, "Tab"},      {KEY_BKSPC, "Bksp"},   {KEY_DEL, "Del"},
    {' ', "Space"},        {KEY_F1, "F1"},        {KEY_F2, "F2"},
    {KEY_F3, "F3"},        {KEY_F4, "F4"},        {KEY_F5, "F5"},
    {KEY_F6, "F6"},        {KEY_F7, "F7"},        {KEY_F8, "F8"},
    {KEY_F9, "F9"},        {KEY_MOD_SHL, "LShift"}, {KEY_MOD_SHR, "RShift"},
    {KEY_MOD_CTRL, "Ctrl"},
};

#define N_KEYS (sizeof(k_keys) / sizeof(k_keys[0]))

// "A" .. "Z", two bytes each.
static const char k_letters[] = "A\0B\0C\0D\0E\0F\0G\0H\0I\0J\0K\0L\0M\0"
                                "N\0O\0P\0Q\0R\0S\0T\0U\0V\0W\0X\0Y\0Z";

const char *gamepad_key_name(uint8_t key) {
  if (key >= 'A' && key <= 'Z')
    key = (uint8_t)(key | 0x20);
  if (key >= 'a' && key <= 'z')
    return &k_letters[(key - 'a') * 2];
  for (size_t i = 0; i < N_KEYS; i++)
    if (k_keys[i].key == key)
      return k_keys[i].name;
  return NULL;
}

uint8_t gamepad_key_from_name(const char *name) {
  if (!name || !name[0])
    return 0;
  if (!name[1]) {
    char c = name[0];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c | 0x20);
    if (c >= 'a' && c <= 'z')
      return (uint8_t)c;
  }
  for (size_t i = 0; i < N_KEYS; i++)
    if (strcasecmp(k_keys[i].name, name) == 0)
      return k_keys[i].key;
  return 0;
}

// ── Button names ─────────────────────────────────────────────────────────────

static const char k_button_names[KBD_PAD_BUTTONS][7] = {
    "up", "down", "left", "right", "a", "b",
    "x", "y", "l", "r", "start", "select"};

static const char k_button_labels[KBD_PAD_BUTTONS][7] = {
    "Up", "Down", "Left", "Right", "A", "B",
    "X", "Y", "L", "R", "Start", "Select"};

int gamepad_button_index(uint32_t pad_button) {
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    if (pad_button == (1u << b))
      return b;
  return -1;
}

const char *gamepad_button_name(int button) {
  return (button >= 0 && button < KBD_PAD_BUTTONS) ? k_button_names[button]
                                                   : NULL;
}

const char *gamepad_button_label(int button) {
  return (button >= 0 && button < KBD_PAD_BUTTONS) ? k_button_labels[button]
                                                   : NULL;
}

int gamepad_button_from_name(const char *name) {
  for (int b = 0; name && b < KBD_PAD_BUTTONS; b++)
    if (strcasecmp(k_button_names[b], name) == 0)
      return b;
  return -1;
}

// ── Maps ─────────────────────────────────────────────────────────────────────

const char *gamepad_map_label(const kbd_padmap_t *m, uint32_t pad_button,
                              int slot) {
  int b = gamepad_button_index(pad_button);
  if (!m || b < 0 || slot < 0 || slot >= KBD_PAD_SLOTS || !m->key[b][slot])
    return NULL;
  return gamepad_key_name(m->key[b][slot]);
}
