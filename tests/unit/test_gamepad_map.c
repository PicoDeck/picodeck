// Host unit tests for src/os/gamepad_map.c: the gamepad's key-name table
// (getLabel, the bindings files), button names and label lookup.
#include "check.h"
#include "gamepad_map.h"
#include "os.h"

#include <string.h>

// Every bindable key's name maps back to it, in any case; letters are
// case-folded and named in upper case.
static void test_key_names_round_trip(void) {
  static const uint8_t keys[] = {KEY_UP,    KEY_DOWN,  KEY_LEFT, KEY_RIGHT,
                                 KEY_ENTER, KEY_ESC,   KEY_TAB,  KEY_BKSPC,
                                 KEY_DEL,   ' ',       KEY_F1,   KEY_F5,
                                 KEY_F9,    'a',       'w',      'z',
                                 KEY_MOD_SHL, KEY_MOD_SHR, KEY_MOD_CTRL};
  for (size_t i = 0; i < sizeof(keys); i++) {
    const char *name = gamepad_key_name(keys[i]);
    CHECK(name != NULL);
    CHECK_EQ_INT(gamepad_key_from_name(name), keys[i]);
    CHECK(gamepad_key_bindable(keys[i]));
  }
  CHECK_STR(gamepad_key_name(KEY_F4), "F4");
  CHECK_STR(gamepad_key_name(KEY_DEL), "Del");
  CHECK_STR(gamepad_key_name(KEY_BKSPC), "Bksp");
  CHECK_STR(gamepad_key_name('w'), "W");
  CHECK_STR(gamepad_key_name('W'), "W");
  CHECK_EQ_INT(gamepad_key_from_name("w"), 'w');
  CHECK_EQ_INT(gamepad_key_from_name("W"), 'w');
  CHECK_EQ_INT(gamepad_key_from_name("up"), KEY_UP);
  CHECK_EQ_INT(gamepad_key_from_name("SPACE"), ' ');
  CHECK_EQ_INT(gamepad_key_from_name("f4"), KEY_F4);
}

// Keys that cannot be bound: the OS's (menu key, Brk), layer keys, digits
// and symbols (their keycode changes with Shift), and unknown names.
static void test_unbindable_keys(void) {
  static const uint8_t keys[] = {KEY_F10, KEY_BRK, KEY_MOD_SYM, KEY_MOD_ALT,
                                 '1', '0', '!', ';', 0};
  for (size_t i = 0; i < sizeof(keys); i++) {
    CHECK(gamepad_key_name(keys[i]) == NULL);
    CHECK(!gamepad_key_bindable(keys[i]));
  }
  CHECK_EQ_INT(gamepad_key_from_name("F10"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("Brk"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("1"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("Fn"), 0);
  CHECK_EQ_INT(gamepad_key_from_name(""), 0);
  CHECK_EQ_INT(gamepad_key_from_name(NULL), 0);
  CHECK_EQ_INT(gamepad_key_from_name("Upp"), 0);
}

static void test_button_names(void) {
  CHECK_EQ_INT(gamepad_button_index(PAD_UP), 0);
  CHECK_EQ_INT(gamepad_button_index(PAD_A), 4);
  CHECK_EQ_INT(gamepad_button_index(PAD_SELECT), 11);
  CHECK_EQ_INT(gamepad_button_index(PAD_A | PAD_B), -1);
  CHECK_EQ_INT(gamepad_button_index(0), -1);
  CHECK_EQ_INT(gamepad_button_index(1u << 12), -1);
  CHECK_STR(gamepad_button_name(4), "a");
  CHECK_STR(gamepad_button_label(10), "Start");
  CHECK(gamepad_button_name(12) == NULL);
  CHECK(gamepad_button_label(-1) == NULL);
  CHECK_EQ_INT(gamepad_button_from_name("Select"), 11);
  CHECK_EQ_INT(gamepad_button_from_name("up"), 0);
  CHECK_EQ_INT(gamepad_button_from_name("z"), -1);
  CHECK_EQ_INT(gamepad_button_from_name(NULL), -1);
}

// getLabel over a map: the key's name, NULL when unbound or out of range.
static void test_map_label(void) {
  kbd_padmap_t m = KBD_PAD_DEFAULT_MAP;
  m.key[0][1] = 'w';
  CHECK_STR(gamepad_map_label(&m, PAD_A, 0), "F4");
  CHECK_STR(gamepad_map_label(&m, PAD_X, 0), "Del");
  CHECK_STR(gamepad_map_label(&m, PAD_UP, 1), "W");
  CHECK_STR(gamepad_map_label(&m, PAD_SELECT, 0), "Tab");
  CHECK(gamepad_map_label(&m, PAD_A, 1) == NULL);
  CHECK(gamepad_map_label(&m, PAD_A, 2) == NULL);
  CHECK(gamepad_map_label(&m, PAD_A, -1) == NULL);
  CHECK(gamepad_map_label(&m, PAD_A | PAD_B, 0) == NULL);
  CHECK(gamepad_map_label(NULL, PAD_A, 0) == NULL);
}

int main(void) {
  test_key_names_round_trip();
  test_unbindable_keys();
  test_button_names();
  test_map_label();
  return check_report("test_gamepad_map");
}
