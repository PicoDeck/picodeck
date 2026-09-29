#include "gamepad_map.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "flat_json.h"
#include "sd_atomic.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

// ── Key names ────────────────────────────────────────────────────────────────
// The one table of bindable keys (letters are generated below). Short names:
// they label on-screen hints and the Settings page's cells. F6-F9 are not
// here: the keyboard sends them for Shift+F1..F4 (see gamepad_map.h).

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

void gamepad_map_defaults(kbd_padmap_t *m) {
  const kbd_padmap_t defaults = KBD_PAD_DEFAULT_MAP;
  *m = defaults;
}

static uint8_t key_fold(uint8_t key) {
  return (key >= 'A' && key <= 'Z') ? (uint8_t)(key | 0x20) : key;
}

bool gamepad_map_bind(kbd_padmap_t *m, int button, int slot, uint8_t key,
                      int *from_button, int *from_slot) {
  if (from_button)
    *from_button = -1;
  if (from_slot)
    *from_slot = -1;
  if (button < 0 || button >= KBD_PAD_BUTTONS || slot < 0 ||
      slot >= KBD_PAD_SLOTS || (key && !gamepad_key_bindable(key)))
    return false;
  key = key_fold(key);
  for (int b = 0; key && b < KBD_PAD_BUTTONS; b++)
    for (int s = 0; s < KBD_PAD_SLOTS; s++) {
      if ((b == button && s == slot) || key_fold(m->key[b][s]) != key)
        continue;
      m->key[b][s] = 0;
      if (from_button)
        *from_button = b;
      if (from_slot)
        *from_slot = s;
    }
  m->key[button][slot] = key;
  return true;
}

void gamepad_map_merge(kbd_padmap_t *out, const kbd_padmap_t *global,
                       const kbd_padmap_t *ovr, uint16_t ovr_mask) {
  kbd_padmap_t r = *global;
  for (int b = 0; b < KBD_PAD_BUTTONS; b++)
    if (ovr_mask & (1u << b))
      for (int s = 0; s < KBD_PAD_SLOTS; s++)
        r.key[b][s] = ovr->key[b][s];
  for (int b = 0; b < KBD_PAD_BUTTONS; b++) {
    if (ovr_mask & (1u << b))
      continue;
    for (int s = 0; s < KBD_PAD_SLOTS; s++) {
      uint8_t key = key_fold(r.key[b][s]);
      for (int o = 0; key && o < KBD_PAD_BUTTONS; o++)
        if ((ovr_mask & (1u << o)) && (key_fold(r.key[o][0]) == key ||
                                       key_fold(r.key[o][1]) == key))
          r.key[b][s] = key = 0;
    }
  }
  *out = r;
}

// ── Bindings files ───────────────────────────────────────────────────────────
// A small recursive-descent reader over flat_json_read_string (the string
// reader the config stores share): an object of arrays of strings is all a
// bindings file holds, so there is no general JSON parser behind it.

#define NAME_MAX_LEN 16 // longer names are truncated, and so unknown

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;
  return p;
}

// A string or null at p: its text in out ("" for null). NULL on anything
// else. An unterminated string stops at the NUL, which no caller accepts next.
static const char *read_value(const char *p, char *out) {
  if (*p == '"')
    return flat_json_read_string(p + 1, out, NAME_MAX_LEN);
  if (strncmp(p, "null", 4) == 0) {
    out[0] = '\0';
    return p + 4;
  }
  return NULL;
}

// The keycode a slot's name stands for; 0 for "" / null and (logged) for a
// name that is not a bindable key.
static uint8_t slot_key(const char *name, const char *what) {
  if (!name[0])
    return 0;
  uint8_t key = gamepad_key_from_name(name);
  if (!key)
    printf("[GAMEPAD] %s: unknown key \"%s\" ignored\n", what, name);
  return key;
}

bool gamepad_map_parse(const char *json, kbd_padmap_t *m, uint16_t *listed,
                       const char *what) {
  kbd_padmap_t w = *m;
  uint16_t l = 0;
  const char *p = skip_ws(json);
  if (*p != '{')
    return false;
  p = skip_ws(p + 1);
  if (*p == '}')
    goto done;
  for (;;) {
    char bname[NAME_MAX_LEN], kname[NAME_MAX_LEN];
    uint8_t keys[KBD_PAD_SLOTS] = {0, 0};
    if (*p != '"')
      return false;
    p = skip_ws(flat_json_read_string(p + 1, bname, sizeof(bname)));
    if (*p != ':')
      return false;
    p = skip_ws(p + 1);
    if (*p == '[') {
      p = skip_ws(p + 1);
      for (int n = 0; *p != ']'; n++) {
        if (!(p = read_value(p, kname)))
          return false;
        if (n < KBD_PAD_SLOTS)
          keys[n] = slot_key(kname, what);
        else
          printf("[GAMEPAD] %s: \"%s\": more than %d keys, \"%s\" ignored\n",
                 what, bname, KBD_PAD_SLOTS, kname);
        p = skip_ws(p);
        if (*p == ',') {
          p = skip_ws(p + 1);
          if (*p == ']')
            return false; // a trailing comma
        } else if (*p != ']') {
          return false;
        }
      }
      p++;
    } else {
      if (!(p = read_value(p, kname)))
        return false;
      keys[0] = slot_key(kname, what);
    }
    int b = gamepad_button_from_name(bname);
    if (b < 0) {
      printf("[GAMEPAD] %s: unknown button \"%s\" ignored\n", what, bname);
    } else {
      w.key[b][0] = w.key[b][1] = 0;
      for (int s = 0; s < KBD_PAD_SLOTS; s++)
        gamepad_map_bind(&w, b, s, keys[s], NULL, NULL);
      l |= (uint16_t)(1u << b);
    }
    p = skip_ws(p);
    if (*p == '}')
      break;
    if (*p != ',')
      return false;
    p = skip_ws(p + 1);
  }
done:
  if (*skip_ws(p + 1))
    return false; // something after the closing brace
  *m = w;
  *listed |= l;
  return true;
}

int gamepad_map_format(char *buf, size_t cap, const kbd_padmap_t *m,
                       uint16_t mask) {
  size_t pos = 0;
  bool first = true;
#define PUT(...)                                                               \
  do {                                                                         \
    int n_ = snprintf(buf + pos, cap - pos, __VA_ARGS__);                      \
    if (n_ < 0 || (size_t)n_ >= cap - pos)                                     \
      return -1;                                                               \
    pos += (size_t)n_;                                                         \
  } while (0)
  if (!cap)
    return -1;
  PUT("{");
  for (int b = 0; b < KBD_PAD_BUTTONS; b++) {
    if (!(mask & (1u << b)))
      continue;
    const char *k0 = m->key[b][0] ? gamepad_key_name(m->key[b][0]) : NULL;
    const char *k1 = m->key[b][1] ? gamepad_key_name(m->key[b][1]) : NULL;
    PUT("%s\n  \"%s\": [", first ? "" : ",", k_button_names[b]);
    if (k1)
      PUT("\"%s\", \"%s\"]", k0 ? k0 : "", k1);
    else if (k0)
      PUT("\"%s\"]", k0);
    else
      PUT("]");
    first = false;
  }
  PUT("%s}\n", first ? "" : "\n");
#undef PUT
  return (int)pos;
}

bool gamepad_app_path(char *out, size_t n, const char *app_id) {
  int len = snprintf(out, n, "/data/%s/gamepad.json", app_id);
  return len > 0 && (size_t)len < n;
}

gamepad_file_t gamepad_load_file(const char *path, kbd_padmap_t *m,
                                 uint16_t *listed) {
  sd_atomic_recover(path);
  int size = sdcard_fsize(path);
  if (size < 0)
    return GAMEPAD_FILE_MISSING; // no file: the defaults / no override
  if (size > GAMEPAD_FILE_MAX) {
    printf("[GAMEPAD] %s: %d bytes, larger than %d; ignored\n", path, size,
           GAMEPAD_FILE_MAX);
    return GAMEPAD_FILE_IGNORED;
  }
  char *json = sdcard_read_file(path, NULL);
  if (!json) {
    printf("[GAMEPAD] %s: unreadable; ignored\n", path);
    return GAMEPAD_FILE_UNREADABLE;
  }
  bool ok = gamepad_map_parse(json, m, listed, path);
  umm_free(json);
  if (!ok) {
    printf("[GAMEPAD] %s: not a bindings file; ignored\n", path);
    return GAMEPAD_FILE_IGNORED;
  }
  return GAMEPAD_FILE_LOADED;
}

bool gamepad_load(const char *path, kbd_padmap_t *m, uint16_t *listed) {
  return gamepad_load_file(path, m, listed) == GAMEPAD_FILE_LOADED;
}

// The directory holding path (/data/<app_id>), created if missing.
static bool ensure_parent_dir(const char *path) {
  const char *slash = strrchr(path, '/');
  char dir[GAMEPAD_PATH_MAX];
  size_t n = slash ? (size_t)(slash - path) : 0;
  if (!n)
    return true;
  if (n >= sizeof(dir))
    return false;
  memcpy(dir, path, n);
  dir[n] = '\0';
  return sdcard_fexists(dir) || sdcard_mkdir(dir);
}

bool gamepad_save(const char *path, const kbd_padmap_t *m, uint16_t mask) {
  char *buf = (char *)umm_malloc(GAMEPAD_FILE_MAX);
  if (!buf) {
    printf("[GAMEPAD] %s: out of memory; not saved\n", path);
    return false;
  }
  int len = gamepad_map_format(buf, GAMEPAD_FILE_MAX, m, mask);
  bool ok = len >= 0 && ensure_parent_dir(path) &&
            sd_atomic_write(path, buf, len);
  umm_free(buf);
  if (!ok)
    printf("[GAMEPAD] save to %s failed; previous file kept\n", path);
  return ok;
}

bool gamepad_remove(const char *path) {
  char tmp[SD_ATOMIC_PATH_MAX], bak[SD_ATOMIC_PATH_MAX];
  if (sdcard_fexists(path))
    sdcard_delete(path);
  if (!sd_atomic_names(path, tmp, bak))
    return !sdcard_fexists(path);
  if (sdcard_fexists(bak))
    sdcard_delete(bak);
  return !sdcard_fexists(path) && !sdcard_fexists(bak);
}

void gamepad_load_effective(kbd_padmap_t *out, const char *app_id) {
  uint16_t listed = 0;
  gamepad_map_defaults(out);
  gamepad_load(GAMEPAD_GLOBAL_PATH, out, &listed);
  char path[GAMEPAD_PATH_MAX];
  if (!app_id || !gamepad_app_path(path, sizeof(path), app_id))
    return;
  kbd_padmap_t ovr;
  uint16_t mask = 0;
  memset(&ovr, 0, sizeof(ovr));
  if (gamepad_load(path, &ovr, &mask) && mask)
    gamepad_map_merge(out, out, &ovr, mask);
}
