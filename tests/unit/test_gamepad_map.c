// Host unit tests for src/os/gamepad_map.c: the gamepad's key-name table
// (getLabel, the bindings files), button names and label lookup, the
// one-button-per-key rule, the global map + per-app override merge, and the
// bindings files (/system/gamepad.json, /data/<id>/gamepad.json) over the
// in-memory SD fake: missing, corrupt and unknown-name cases.
#include "check.h"
#include "gamepad_map.h"
#include "os.h"
#include "fakes/sdcard_fake.h"

#include <string.h>

enum { UP, DOWN, LEFT, RIGHT, A, B, X, Y, L, R, START, SELECT };

static bool map_eq(const kbd_padmap_t *a, const kbd_padmap_t *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

static kbd_padmap_t defaults(void) {
  kbd_padmap_t m;
  gamepad_map_defaults(&m);
  return m;
}

// Every bindable key's name maps back to it, in any case; letters are
// case-folded and named in upper case.
static void test_key_names_round_trip(void) {
  static const uint8_t keys[] = {KEY_UP,    KEY_DOWN, KEY_LEFT, KEY_RIGHT,
                                 KEY_ENTER, KEY_ESC,  KEY_TAB,  KEY_BKSPC,
                                 KEY_DEL,   ' ',      KEY_F1,   KEY_F2,
                                 KEY_F3,    KEY_F4,   KEY_F5,   'a',
                                 'w',       'z',      KEY_MOD_CTRL};
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

// Keys that cannot be bound: the OS's (menu key, Brk), the codes keys take
// under Shift (F6-F9, End, Home, Insert, PgUp, PgDn), the modifiers that
// change other keys (Shift, Alt, Sym), digits and symbols (their keycode
// changes with Shift), and unknown names.
static void test_unbindable_keys(void) {
  static const uint8_t keys[] = {KEY_F10,     KEY_BRK,     KEY_F6,   KEY_F7,
                                 KEY_F8,      KEY_F9,      KEY_END,  KEY_HOME,
                                 KEY_INSERT,  KEY_PGUP,    KEY_PGDN, KEY_MOD_SHL,
                                 KEY_MOD_SHR, KEY_MOD_SYM, KEY_MOD_ALT,
                                 '1',         '0',         '!',      ';',
                                 0};
  for (size_t i = 0; i < sizeof(keys); i++) {
    CHECK(gamepad_key_name(keys[i]) == NULL);
    CHECK(!gamepad_key_bindable(keys[i]));
  }
  CHECK_EQ_INT(gamepad_key_from_name("F10"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("Brk"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("1"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("Fn"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("F6"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("LShift"), 0);
  CHECK_EQ_INT(gamepad_key_from_name("RShift"), 0);
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


// ── The one-button-per-key rule ──────────────────────────────────────────────

static void test_bind_moves_the_key(void) {
  kbd_padmap_t m = defaults();
  int fb, fs;
  CHECK(gamepad_map_bind(&m, A, 1, 'Z', &fb, &fs));
  CHECK_EQ_INT(m.key[A][1], 'z');          // stored lower case
  CHECK_EQ_INT(fb, -1);
  CHECK_EQ_INT(fs, -1);
  // F4 moves from A to B: A's primary is left unbound.
  CHECK(gamepad_map_bind(&m, B, 0, KEY_F4, &fb, &fs));
  CHECK_EQ_INT(m.key[B][0], KEY_F4);
  CHECK_EQ_INT(m.key[A][0], 0);
  CHECK_EQ_INT(fb, A);
  CHECK_EQ_INT(fs, 0);
  // Between the slots of one button.
  CHECK(gamepad_map_bind(&m, A, 0, 'z', &fb, &fs));
  CHECK_EQ_INT(m.key[A][0], 'z');
  CHECK_EQ_INT(m.key[A][1], 0);
  CHECK_EQ_INT(fb, A);
  CHECK_EQ_INT(fs, 1);
  // Again into the same slot: nothing moves.
  CHECK(gamepad_map_bind(&m, A, 0, 'z', &fb, NULL));
  CHECK_EQ_INT(fb, -1);
  // Clearing a slot.
  CHECK(gamepad_map_bind(&m, A, 0, 0, NULL, NULL));
  CHECK_EQ_INT(m.key[A][0], 0);
  // Refused, map unchanged: the OS's keys, unnamed keys, bad button/slot.
  kbd_padmap_t before = m;
  CHECK(!gamepad_map_bind(&m, A, 0, KEY_F10, &fb, &fs));
  CHECK(!gamepad_map_bind(&m, A, 0, KEY_BRK, NULL, NULL));
  CHECK(!gamepad_map_bind(&m, A, 0, '1', NULL, NULL));
  CHECK(!gamepad_map_bind(&m, A, 0, KEY_F6, NULL, NULL));
  CHECK(!gamepad_map_bind(&m, A, 0, KEY_MOD_SHL, NULL, NULL));
  CHECK(!gamepad_map_bind(&m, 12, 0, KEY_F2, NULL, NULL));
  CHECK(!gamepad_map_bind(&m, A, 2, KEY_F2, NULL, NULL));
  CHECK(!gamepad_map_bind(&m, -1, 0, KEY_F2, NULL, NULL));
  CHECK(map_eq(&m, &before));
  CHECK_EQ_INT(fb, -1);
}

// A per-game override replaces only the buttons it lists; the others come
// from the global map, minus any key an overridden button took.
static void test_merge_override_fallback(void) {
  kbd_padmap_t global = defaults(), ovr, out;
  global.key[UP][1] = 'w';
  memset(&ovr, 0, sizeof(ovr));
  ovr.key[A][0] = 'z';
  gamepad_map_merge(&out, &global, &ovr, 1u << A);
  CHECK_EQ_INT(out.key[A][0], 'z');
  CHECK_EQ_INT(out.key[A][1], 0);
  CHECK_EQ_INT(out.key[UP][0], KEY_UP);    // inherited
  CHECK_EQ_INT(out.key[UP][1], 'w');
  CHECK_EQ_INT(out.key[B][0], KEY_F5);
  // An override that takes an inherited button's key: one button per key.
  memset(&ovr, 0, sizeof(ovr));
  ovr.key[B][0] = KEY_F4;
  ovr.key[B][1] = 'w';
  gamepad_map_merge(&global, &global, &ovr, 1u << B);  // out aliases global
  CHECK_EQ_INT(global.key[B][0], KEY_F4);
  CHECK_EQ_INT(global.key[B][1], 'w');
  CHECK_EQ_INT(global.key[A][0], 0);
  CHECK_EQ_INT(global.key[UP][0], KEY_UP);
  CHECK_EQ_INT(global.key[UP][1], 0);
  // An override that lists a button as unbound.
  kbd_padmap_t g2 = defaults();
  memset(&ovr, 0, sizeof(ovr));
  gamepad_map_merge(&out, &g2, &ovr, 1u << SELECT);
  CHECK_EQ_INT(out.key[SELECT][0], 0);
  // No override: the global map.
  gamepad_map_merge(&out, &g2, &ovr, 0);
  CHECK(map_eq(&out, &g2));
}

// ── Bindings file text ───────────────────────────────────────────────────────

static void test_parse(void) {
  kbd_padmap_t m = defaults();
  uint16_t listed = 0;
  CHECK(gamepad_map_parse("{\"a\":[\"Z\"],\"up\":[\"Up\",\"W\"]}", &m,
                          &listed, "t"));
  CHECK_EQ_U32(listed, (1u << A) | (1u << UP));
  CHECK_EQ_INT(m.key[A][0], 'z');
  CHECK_EQ_INT(m.key[A][1], 0);
  CHECK_EQ_INT(m.key[UP][0], KEY_UP);
  CHECK_EQ_INT(m.key[UP][1], 'w');
  CHECK_EQ_INT(m.key[B][0], KEY_F5);       // not listed: untouched

  // Whitespace, any case, a bare string, [], null and "".
  m = defaults();
  listed = 0;
  CHECK(gamepad_map_parse(" {\n  \"A\" : [ \"f2\" , \"x\" ],\r\n"
                          "  \"Start\": \"Enter\",\t\"select\": [],\n"
                          "  \"l\": [null, \"Q\"], \"r\": [\"\"] }\n",
                          &m, &listed, "t"));
  CHECK_EQ_U32(listed, (1u << A) | (1u << START) | (1u << SELECT) |
                           (1u << L) | (1u << R));
  CHECK_EQ_INT(m.key[A][0], KEY_F2);
  CHECK_EQ_INT(m.key[A][1], 'x');
  CHECK_EQ_INT(m.key[START][0], KEY_ENTER);
  CHECK_EQ_INT(m.key[START][1], 0);
  CHECK_EQ_INT(m.key[SELECT][0], 0);
  CHECK_EQ_INT(m.key[L][0], 0);
  CHECK_EQ_INT(m.key[L][1], 'q');
  CHECK_EQ_INT(m.key[R][0], 0);

  // Empty object: nothing listed, nothing changed.
  m = defaults();
  listed = 0;
  CHECK(gamepad_map_parse("{}", &m, &listed, "t"));
  CHECK_EQ_U32(listed, 0);
  kbd_padmap_t d = defaults();
  CHECK(map_eq(&m, &d));
}

// Unknown names are skipped (and logged); the rest of the file applies. An
// unknown button is not listed; an unknown or unbindable key leaves its slot
// unbound; a third key is ignored.
static void test_parse_unknown_names(void) {
  kbd_padmap_t m = defaults();
  uint16_t listed = 0;
  CHECK(gamepad_map_parse("{\"turbo\":[\"F1\"],\"a\":[\"F44\",\"Z\"],"
                          "\"b\":[\"F10\"],\"x\":[\"1\",\"Brk\"],"
                          "\"y\":[\"F2\",\"F3\",\"F1\"],"
                          "\"select\":[\"F6\",\"LShift\"],"
                          "\"averyveryverylongbuttonname\":[\"F1\"]}",
                          &m, &listed, "t"));
  CHECK_EQ_U32(listed, (1u << A) | (1u << B) | (1u << X) | (1u << Y) |
                           (1u << SELECT));
  CHECK_EQ_INT(m.key[A][0], 0);
  CHECK_EQ_INT(m.key[A][1], 'z');
  CHECK_EQ_INT(m.key[B][0], 0);
  CHECK_EQ_INT(m.key[X][0], 0);
  CHECK_EQ_INT(m.key[X][1], 0);
  CHECK_EQ_INT(m.key[Y][0], KEY_F2);
  CHECK_EQ_INT(m.key[Y][1], KEY_F3);
  CHECK_EQ_INT(m.key[SELECT][0], 0);
  CHECK_EQ_INT(m.key[SELECT][1], 0);
  CHECK_EQ_INT(m.key[START][0], KEY_F1);   // the ignored F1s moved nothing
}

// One button per key inside a file: a key listed twice ends on the later
// button, and a key taken from an unlisted (default) button leaves it.
static void test_parse_one_button_per_key(void) {
  kbd_padmap_t m = defaults();
  uint16_t listed = 0;
  CHECK(gamepad_map_parse("{\"a\":[\"Z\"],\"b\":[\"Z\",\"F4\"]}", &m,
                          &listed, "t"));
  CHECK_EQ_INT(m.key[A][0], 0);
  CHECK_EQ_INT(m.key[B][0], 'z');
  CHECK_EQ_INT(m.key[B][1], KEY_F4);
  m = defaults();
  CHECK(gamepad_map_parse("{\"start\":[\"Tab\"]}", &m, &listed, "t"));
  CHECK_EQ_INT(m.key[START][0], KEY_TAB);
  CHECK_EQ_INT(m.key[SELECT][0], 0);       // Tab left Select
}

// Anything that is not an object of strings / arrays of strings is corrupt:
// false, and the map and listed mask are left as they were.
static void test_parse_corrupt(void) {
  static const char *bad[] = {
      "",
      "   ",
      "{",
      "[]",
      "null",
      "{\"a\":[\"F4\"]",
      "{\"a\":[\"F4\"]} x",
      "{\"a\":[\"F4\"]}}",
      "{\"a\":F4}",
      "{\"a\":[1]}",
      "{\"a\":[\"F4\" \"F5\"]}",
      "{\"a\" [\"F4\"]}",
      "{\"a\":[\"F4\"],}",
      "{\"a\":[\"F4\",]}",
      "{\"a\":[,\"F4\"]}",
      "{\"a\":[\"F4\",",
      "{\"a\":[\"F4",
      "{\"a",
      "{a:[\"F4\"]}",
      "{\"a\":{\"k\":\"F4\"}}",
      "{\"a\":true}",
      "{\"a\":[\"F4\"] \"b\":[\"F5\"]}",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    kbd_padmap_t m = defaults(), d = defaults();
    uint16_t listed = 0x8000;
    if (gamepad_map_parse(bad[i], &m, &listed, "t"))
      printf("FAIL accepted: %s\n", bad[i]);
    CHECK(!gamepad_map_parse(bad[i], &m, &listed, "t"));
    CHECK(map_eq(&m, &d));
    CHECK_EQ_U32(listed, 0x8000);
  }
}

// The writer's output reads back as the same map.
static void test_format_round_trip(void) {
  kbd_padmap_t m = defaults(), back;
  m.key[UP][1] = 'w';
  m.key[Y][0] = 0;
  m.key[Y][1] = 'z';
  m.key[X][0] = 0;
  char buf[GAMEPAD_FILE_MAX];
  int len = gamepad_map_format(buf, sizeof(buf), &m, 0x0FFF);
  CHECK(len > 0 && (size_t)len == strlen(buf));
  CHECK(strstr(buf, "\"up\": [\"Up\", \"W\"]") != NULL);
  CHECK(strstr(buf, "\"a\": [\"F4\"]") != NULL);
  CHECK(strstr(buf, "\"x\": []") != NULL);
  CHECK(strstr(buf, "\"y\": [\"\", \"Z\"]") != NULL);
  memset(&back, 0, sizeof(back));
  uint16_t listed = 0;
  CHECK(gamepad_map_parse(buf, &back, &listed, "t"));
  CHECK_EQ_U32(listed, 0x0FFF);
  CHECK(map_eq(&back, &m));
  // Only the buttons in the mask (an override).
  len = gamepad_map_format(buf, sizeof(buf), &m, (1u << A) | (1u << UP));
  memset(&back, 0, sizeof(back));
  listed = 0;
  CHECK(gamepad_map_parse(buf, &back, &listed, "t"));
  CHECK_EQ_U32(listed, (1u << A) | (1u << UP));
  CHECK_EQ_INT(back.key[A][0], KEY_F4);
  CHECK_EQ_INT(back.key[B][0], 0);
  // Nothing listed; too small a buffer.
  CHECK_EQ_INT(gamepad_map_format(buf, sizeof(buf), &m, 0), 3);
  CHECK_STR(buf, "{}\n");
  CHECK_EQ_INT(gamepad_map_format(buf, 20, &m, 0x0FFF), -1);
  CHECK_EQ_INT(gamepad_map_format(buf, 0, &m, 0x0FFF), -1);
}

// ── Bindings files on the SD card ────────────────────────────────────────────

#define GLOBAL GAMEPAD_GLOBAL_PATH
#define OVR "/data/com.test.game/gamepad.json"

static void put(const char *path, const char *text) {
  sdfake_put(path, text, strlen(text));
}

static void test_load_missing_corrupt_unknown(void) {
  kbd_padmap_t eff, d = defaults();
  // No files at all: the defaults, for the launcher and for an app.
  sdfake_reset();
  gamepad_load_effective(&eff, NULL);
  CHECK(map_eq(&eff, &d));
  gamepad_load_effective(&eff, "com.test.game");
  CHECK(map_eq(&eff, &d));
  // A corrupt global file: the defaults; gamepad_load says so.
  put(GLOBAL, "{\"a\":[\"Z\"");
  gamepad_load_effective(&eff, "com.test.game");
  CHECK(map_eq(&eff, &d));
  kbd_padmap_t m = d;
  uint16_t listed = 0;
  CHECK(!gamepad_load(GLOBAL, &m, &listed));
  CHECK(map_eq(&m, &d));
  CHECK_EQ_U32(listed, 0);
  CHECK(!gamepad_load("/system/none.json", &m, &listed));
  // Too big: refused unread.
  static char big[GAMEPAD_FILE_MAX + 16];
  memset(big, ' ', sizeof(big) - 1);
  big[0] = '{';
  big[sizeof(big) - 2] = '}';
  big[sizeof(big) - 1] = '\0';
  put(GLOBAL, big);
  CHECK(!gamepad_load(GLOBAL, &m, &listed));
  // Unknown names in the global file: the rest applies.
  put(GLOBAL, "{\"up\":[\"Up\",\"W\"],\"turbo\":[\"F6\"],\"b\":[\"Nope\"]}");
  gamepad_load_effective(&eff, NULL);
  CHECK_EQ_INT(eff.key[UP][1], 'w');
  CHECK_EQ_INT(eff.key[B][0], 0);
  CHECK_EQ_INT(eff.key[A][0], KEY_F4);
}

// gamepad_load_file says why nothing loaded: the Settings page must not
// overwrite a file that exists but could not be read (it may read next
// time), while a corrupt or oversized one is ignored for good.
static void test_load_file_status(void) {
  kbd_padmap_t m, d = defaults();
  uint16_t listed = 0;
  sdfake_reset();
  m = d;
  CHECK_EQ_INT(gamepad_load_file(GLOBAL, &m, &listed), GAMEPAD_FILE_MISSING);
  put(GLOBAL, "{\"a\":[\"Z\"]}");
  sdfake_fail_reads(true);
  CHECK_EQ_INT(gamepad_load_file(GLOBAL, &m, &listed),
               GAMEPAD_FILE_UNREADABLE);
  CHECK(!gamepad_load(GLOBAL, &m, &listed));
  CHECK(map_eq(&m, &d));
  CHECK_EQ_U32(listed, 0);
  sdfake_fail_reads(false);
  CHECK_EQ_INT(gamepad_load_file(GLOBAL, &m, &listed), GAMEPAD_FILE_LOADED);
  CHECK_EQ_INT(m.key[A][0], 'z');
  CHECK_EQ_U32(listed, 1u << A);
  put(GLOBAL, "not json");
  m = d;
  listed = 0;
  CHECK_EQ_INT(gamepad_load_file(GLOBAL, &m, &listed), GAMEPAD_FILE_IGNORED);
  CHECK(map_eq(&m, &d));
  static char big[GAMEPAD_FILE_MAX + 16];
  memset(big, ' ', sizeof(big) - 1);
  big[0] = '{';
  big[sizeof(big) - 2] = '}';
  big[sizeof(big) - 1] = '\0';
  put(GLOBAL, big);
  CHECK_EQ_INT(gamepad_load_file(GLOBAL, &m, &listed), GAMEPAD_FILE_IGNORED);
  CHECK_EQ_U32(listed, 0);
}

// The per-game file overrides only the buttons it lists, for that app only;
// a corrupt override falls back to the global map.
static void test_load_override(void) {
  kbd_padmap_t eff;
  sdfake_reset();
  put(GLOBAL, "{\"up\":[\"Up\",\"W\"]}");
  put(OVR, "{\"a\":[\"Z\"]}");
  gamepad_load_effective(&eff, "com.test.game");
  CHECK_EQ_INT(eff.key[A][0], 'z');
  CHECK_EQ_INT(eff.key[UP][1], 'w');        // from the global map
  CHECK_EQ_INT(eff.key[B][0], KEY_F5);      // from the defaults
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    CHECK(eff.key[b][0] != KEY_F4 && eff.key[b][1] != KEY_F4);
  // Another app, and the launcher: no override.
  gamepad_load_effective(&eff, "com.test.other");
  CHECK_EQ_INT(eff.key[A][0], KEY_F4);
  CHECK_EQ_INT(eff.key[UP][1], 'w');
  gamepad_load_effective(&eff, NULL);
  CHECK_EQ_INT(eff.key[A][0], KEY_F4);
  // An override taking a global key: it leaves the inherited button.
  put(OVR, "{\"b\":[\"W\"]}");
  gamepad_load_effective(&eff, "com.test.game");
  CHECK_EQ_INT(eff.key[B][0], 'w');
  CHECK_EQ_INT(eff.key[UP][0], KEY_UP);
  CHECK_EQ_INT(eff.key[UP][1], 0);
  // A corrupt override: the global map.
  put(OVR, "not json");
  gamepad_load_effective(&eff, "com.test.game");
  CHECK_EQ_INT(eff.key[A][0], KEY_F4);
  CHECK_EQ_INT(eff.key[B][0], KEY_F5);
  CHECK_EQ_INT(eff.key[UP][1], 'w');
}

static void test_save_load_remove(void) {
  sdfake_reset();
  kbd_padmap_t m = defaults(), back;
  CHECK(gamepad_map_bind(&m, A, 1, 'z', NULL, NULL));
  // An override: only A, into /data/<id>/, created on the way.
  CHECK(gamepad_save(OVR, &m, 1u << A));
  CHECK(sdfake_dir_exists("/data/com.test.game"));
  size_t len = 0;
  const char *text = sdfake_get(OVR, &len);
  CHECK(text != NULL);
  CHECK(text && strstr(text, "\"a\": [\"F4\", \"Z\"]") != NULL);
  CHECK(text && strstr(text, "\"up\"") == NULL);
  memset(&back, 0, sizeof(back));
  uint16_t listed = 0;
  CHECK(gamepad_load(OVR, &back, &listed));
  CHECK_EQ_U32(listed, 1u << A);
  CHECK_EQ_INT(back.key[A][0], KEY_F4);
  CHECK_EQ_INT(back.key[A][1], 'z');
  // Saving again replaces the file (through .tmp and a rename).
  CHECK(gamepad_save(OVR, &m, (1u << A) | (1u << B)));
  CHECK(sdfake_get(OVR ".tmp", NULL) == NULL);
  CHECK(sdfake_get(OVR ".bak", NULL) == NULL);
  // A failed save keeps the previous file.
  const char *before = sdfake_get(OVR, NULL);
  char kept[256];
  snprintf(kept, sizeof(kept), "%s", before ? before : "");
  sdfake_fail_rename_after(0);
  CHECK(!gamepad_save(OVR, &m, 0x0FFF));
  CHECK_STR(sdfake_get(OVR, NULL), kept);
  // The global map, whole.
  CHECK(gamepad_save(GLOBAL, &m, 0x0FFF));
  kbd_padmap_t eff;
  gamepad_load_effective(&eff, NULL);
  CHECK(map_eq(&eff, &m));
  // Remove: gone, with a .bak an interrupted save left.
  put(OVR ".bak", "{}");
  CHECK(gamepad_remove(OVR));
  CHECK(sdfake_get(OVR, NULL) == NULL);
  CHECK(sdfake_get(OVR ".bak", NULL) == NULL);
  CHECK(gamepad_remove(OVR));               // nothing there: still true
}

static void test_app_path(void) {
  char path[GAMEPAD_PATH_MAX];
  char id[80];
  memset(id, 'a', 79);
  id[79] = '\0';
  CHECK(gamepad_app_path(path, sizeof(path), "com.test.game"));
  CHECK_STR(path, OVR);
  CHECK(gamepad_app_path(path, sizeof(path), id));  // the longest id fits
  CHECK(!gamepad_app_path(path, 20, "com.test.game"));
}

int main(void) {
  test_key_names_round_trip();
  test_unbindable_keys();
  test_button_names();
  test_map_label();
  test_bind_moves_the_key();
  test_merge_override_fallback();
  test_parse();
  test_parse_unknown_names();
  test_parse_one_button_per_key();
  test_parse_corrupt();
  test_format_round_trip();
  test_load_missing_corrupt_unknown();
  test_load_file_status();
  test_load_override();
  test_save_load_remove();
  test_app_path();
  return check_report("test_gamepad_map");
}
