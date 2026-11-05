// Host unit tests for src/os/appconfig.c (/data/<APP_ID>/config.json, the
// per-app store behind picocalc.config), over the in-memory SD fake.
// Limits: APPCONFIG_MAX_ENTRIES keys, keys 31 bytes, values 255. Over a limit
// nothing is stored and appconfig_put says why.
#include "check.h"
#include "appconfig.h"
#include "fakes/sdcard_fake.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void key_name(char *k, size_t n, int i) { snprintf(k, n, "k%d", i); }

static void test_limits(void) {
  sdfake_reset();
  appconfig_load("com.test.a");
  char k[8];
  for (int i = 0; i < APPCONFIG_MAX_ENTRIES; i++) {
    key_name(k, sizeof(k), i);
    CHECK_EQ_INT(appconfig_put(k, "v"), APPCONFIG_OK);
  }
  CHECK(APPCONFIG_MAX_ENTRIES >= 32);
  CHECK_STR(appconfig_get("k3", NULL), "v");
  CHECK_STR(appconfig_get("k4", NULL), "v");  // the old store's 5th key
  key_name(k, sizeof(k), APPCONFIG_MAX_ENTRIES - 1);
  CHECK_STR(appconfig_get(k, NULL), "v");

  // One more does not fit, and says so; replacing an existing key still works.
  CHECK_EQ_INT(appconfig_put("extra", "v"), APPCONFIG_FULL);
  CHECK(appconfig_get("extra", NULL) == NULL);
  CHECK_STR(appconfig_get("extra", "dflt"), "dflt");
  CHECK_EQ_INT(appconfig_put("k0", "changed"), APPCONFIG_OK);
  CHECK_STR(appconfig_get("k0", NULL), "changed");
  CHECK_STR(appconfig_get(NULL, "dflt"), "dflt");

  // A delete makes room again.
  CHECK_EQ_INT(appconfig_put("k1", ""), APPCONFIG_OK);
  CHECK(appconfig_get("k1", NULL) == NULL);
  CHECK_EQ_INT(appconfig_put("extra", "v"), APPCONFIG_OK);

  appconfig_clear();
  CHECK(appconfig_get("k0", NULL) == NULL);
  appconfig_unbind();
}

// The native table's set() returns nothing: the same rules, no return value.
static void test_native_set(void) {
  sdfake_reset();
  appconfig_load("com.test.n");
  appconfig_set("k", "v");
  CHECK_STR(appconfig_get("k", NULL), "v");
  appconfig_set("k", NULL);
  CHECK(appconfig_get("k", NULL) == NULL);
  appconfig_unbind();
}

// Too long is refused whole, never cut short, and the old value stays.
static void test_too_long(void) {
  sdfake_reset();
  appconfig_load("com.test.l");
  char key[APPCONFIG_KEY_MAX + 1], val[APPCONFIG_VAL_MAX + 1];

  memset(key, 'k', sizeof(key));
  key[APPCONFIG_KEY_MAX - 1] = '\0';  // the longest allowed
  CHECK_EQ_INT(appconfig_put(key, "v"), APPCONFIG_OK);
  CHECK_STR(appconfig_get(key, NULL), "v");
  memset(key, 'k', sizeof(key));
  key[APPCONFIG_KEY_MAX] = '\0';  // one more
  CHECK_EQ_INT(appconfig_put(key, "v"), APPCONFIG_KEY_TOO_LONG);
  CHECK(appconfig_get(key, NULL) == NULL);
  CHECK_EQ_INT(appconfig_put(key, ""), APPCONFIG_KEY_TOO_LONG);

  memset(val, 'v', sizeof(val));
  val[APPCONFIG_VAL_MAX - 1] = '\0';  // the longest allowed
  CHECK_EQ_INT(appconfig_put("x", val), APPCONFIG_OK);
  CHECK_EQ_INT(strlen(appconfig_get("x", "")), APPCONFIG_VAL_MAX - 1);
  CHECK_EQ_INT(appconfig_put("y", "old"), APPCONFIG_OK);
  memset(val, 'v', sizeof(val));
  val[APPCONFIG_VAL_MAX] = '\0';  // one more
  CHECK_EQ_INT(appconfig_put("y", val), APPCONFIG_VALUE_TOO_LONG);
  CHECK_STR(appconfig_get("y", NULL), "old");
  CHECK_EQ_INT(appconfig_put("z", val), APPCONFIG_VALUE_TOO_LONG);
  CHECK(appconfig_get("z", NULL) == NULL);

  CHECK_EQ_INT(appconfig_put("", "v"), APPCONFIG_BAD_KEY);
  CHECK_EQ_INT(appconfig_put(NULL, "v"), APPCONFIG_BAD_KEY);
  CHECK(appconfig_status_text(APPCONFIG_FULL) != NULL);
  CHECK_STR(appconfig_status_text(APPCONFIG_FULL), "config full");
  CHECK_STR(appconfig_status_text(APPCONFIG_KEY_TOO_LONG), "key too long");
  CHECK_STR(appconfig_status_text(APPCONFIG_VALUE_TOO_LONG), "value too long");
  appconfig_unbind();
}

// A pointer from get() stays good while other keys are set (the native API
// has always worked this way): the store never moves.
static void test_get_pointer_stable(void) {
  sdfake_reset();
  appconfig_load("com.test.p");
  CHECK_EQ_INT(appconfig_put("a", "first"), APPCONFIG_OK);
  const char *a = appconfig_get("a", NULL);
  char k[8];
  for (int i = 0; i < APPCONFIG_MAX_ENTRIES - 1; i++) {
    key_name(k, sizeof(k), i);
    CHECK_EQ_INT(appconfig_put(k, "v"), APPCONFIG_OK);
  }
  CHECK(a == appconfig_get("a", NULL));
  CHECK_STR(a, "first");
  // Setting a key to its own value is harmless.
  CHECK_EQ_INT(appconfig_put("a", a), APPCONFIG_OK);
  CHECK_STR(appconfig_get("a", NULL), "first");
  appconfig_unbind();
}

// Everything in the file the store cannot hold survives load + save: more keys
// than the cap, over-long keys and values, numbers, booleans, nested values.
static void test_load_save_keeps_the_rest(void) {
  const char *P = "/data/com.test.k/config.json";
  sdfake_reset();
  char big[400];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  char *file = malloc(8192);
  int n = snprintf(file, 8192, "{ \"long\" : \"%s\", \"num\":7, \"flag\":true,"
                   "\"nothing\":null,\"list\":[1,\"two,]\",{\"a\":\"b\"}],"
                   "\"obj\":{\"x\":{\"y\":\"}\"}},"
                   "\"a_key_that_is_much_longer_than_the_limit\":\"lk\","
                   "\"plain\":\"p\"", big);
  for (int i = 0; i < APPCONFIG_MAX_ENTRIES + 5; i++)
    n += snprintf(file + n, (size_t)(8192 - n), ",\"f%d\":\"v%d\"", i, i);
  n += snprintf(file + n, (size_t)(8192 - n), "}\n");
  sdfake_put(P, file, (size_t)n);

  CHECK(appconfig_load("com.test.k"));
  CHECK_STR(appconfig_get("plain", NULL), "p");
  CHECK_STR(appconfig_get("f0", NULL), "v0");
  CHECK(appconfig_get("long", NULL) == NULL);  // never a truncated copy
  CHECK(appconfig_get("num", NULL) == NULL);
  CHECK_EQ_INT(appconfig_put("plain", "changed"), APPCONFIG_OK);
  CHECK(appconfig_save());

  const char *out = sdfake_get(P, NULL);
  CHECK(out != NULL);
  CHECK(strstr(out, "\"plain\":\"changed\"") != NULL);
  CHECK(strstr(out, big) != NULL);
  CHECK(strstr(out, "\"num\":7") != NULL);
  CHECK(strstr(out, "\"flag\":true") != NULL);
  CHECK(strstr(out, "\"nothing\":null") != NULL);
  CHECK(strstr(out, "\"list\":[1,\"two,]\",{\"a\":\"b\"}]") != NULL);
  CHECK(strstr(out, "\"obj\":{\"x\":{\"y\":\"}\"}}") != NULL);
  CHECK(strstr(out, "\"a_key_that_is_much_longer_than_the_limit\":\"lk\"") != NULL);
  char want[32];
  for (int i = 0; i < APPCONFIG_MAX_ENTRIES + 5; i++) {
    snprintf(want, sizeof(want), "\"f%d\":\"v%d\"", i, i);
    CHECK(strstr(out, want) != NULL);
  }

  // A second round trip changes nothing more.
  char *first = strdup(out);
  CHECK(appconfig_load("com.test.k"));
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), first);
  free(first);

  // set and delete reach what was only kept.
  CHECK(appconfig_load("com.test.k"));
  CHECK_EQ_INT(appconfig_put("plain", ""), APPCONFIG_OK);  // room for one
  CHECK_EQ_INT(appconfig_put("num", "8"), APPCONFIG_OK);
  CHECK_STR(appconfig_get("num", NULL), "8");
  CHECK_EQ_INT(appconfig_put("long", ""), APPCONFIG_OK);
  CHECK(appconfig_save());
  out = sdfake_get(P, NULL);
  CHECK(strstr(out, "\"num\":\"8\"") != NULL);
  CHECK(strstr(out, "\"num\":7") == NULL);
  CHECK(strstr(out, big) == NULL);
  CHECK(strstr(out, "\"flag\":true") != NULL);

  // clear and reset mean it: nothing comes back.
  CHECK(appconfig_load("com.test.k"));
  appconfig_clear();
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{}");
  sdfake_put(P, file, (size_t)n);
  CHECK(appconfig_load("com.test.k"));
  CHECK(appconfig_reset());
  CHECK(sdfake_get(P, NULL) == NULL);
  CHECK_EQ_INT(appconfig_put("a", "b"), APPCONFIG_OK);
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"a\":\"b\"}");
  free(file);
  appconfig_unbind();
}

// A file that exists but cannot be read must not be replaced by what the app
// sets next: that would delete every key in it.
static void test_unreadable_file_is_not_overwritten(void) {
  const char *P = "/data/com.test.u2/config.json";
  sdfake_reset();
  sdfake_put(P, "{\"k\":\"v\"}", 9);
  sdfake_fail_reads(true);
  CHECK(!appconfig_load("com.test.u2"));
  sdfake_fail_reads(false);
  CHECK_STR(appconfig_get_app_id(), "com.test.u2");
  CHECK_EQ_INT(appconfig_put("new", "x"), APPCONFIG_OK);
  CHECK(!appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"v\"}");
  // A load that works, or a deliberate reset, ends it.
  CHECK(appconfig_load("com.test.u2"));
  CHECK_EQ_INT(appconfig_put("new", "x"), APPCONFIG_OK);
  CHECK(appconfig_save());
  CHECK(strstr(sdfake_get(P, NULL), "\"k\":\"v\"") != NULL);
  appconfig_unbind();
}

// A malformed file loads what precedes the damage and never crashes.
static void test_malformed_files(void) {
  static const char *const k_files[] = {
      "", "   ", "not json", "[1,2]", "{", "{\"", "{\"a", "{\"a\"", "{\"a\":",
      "{\"a\":\"", "{\"a\":\"v", "{\"a\":[", "{\"a\":{\"b\":", "{\"a\":\\",
      "{\"a\":\"v\\", "{\"a\":1 2}", ",,,", "{,}", "{\"a\":\"v\",}",
  };
  const char *P = "/data/com.test.m/config.json";
  for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++) {
    sdfake_reset();
    sdfake_put(P, k_files[i], strlen(k_files[i]));
    appconfig_load("com.test.m");
    appconfig_get("a", NULL);
    CHECK_EQ_INT(appconfig_put("n", "v"), APPCONFIG_OK);
    CHECK(appconfig_save());
    CHECK(strstr(sdfake_get(P, NULL), "\"n\":\"v\"") != NULL);
    appconfig_unbind();
  }
  sdfake_reset();
  sdfake_put(P, "{\"a\":\"1\",\"b\":\"2\" junk", 17);
  CHECK(appconfig_load("com.test.m"));
  CHECK_STR(appconfig_get("a", NULL), "1");
  CHECK_STR(appconfig_get("b", NULL), "2");
  appconfig_unbind();
}

// Members with no comma between them stop the load: what is kept must fit the
// block sized for the file, and the commas it adds are the file's own.
static void test_members_without_commas(void) {
  const char *P = "/data/com.test.c/config.json";
  const char *file = "{\"a\":[1]\"b\":[2]\"c\":[3]\"d\":[4]\"e\":[5]\"f\":[6]}";
  sdfake_reset();
  sdfake_put(P, file, strlen(file));
  CHECK(appconfig_load("com.test.c"));
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"a\":[1]}");
  appconfig_unbind();
}

// Any bytes at all load, save and load again without a memory error (ASan's job
// here) and without losing what the first load held.
static void test_random_files(void) {
  static const char k_alphabet[] = "{}[]\",:\\ \n\tab19.-e";
  const char *P = "/data/com.test.r/config.json";
  uint32_t x = 0x12345678u;
  for (int round = 0; round < 3000; round++) {
    char file[300];
    int n = 0;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    int len = (int)(x % sizeof(file));
    if (round % 2)
      file[n++] = '{';  // half start like the real thing
    while (n < len) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      file[n++] = k_alphabet[x % (sizeof(k_alphabet) - 1)];
    }
    sdfake_reset();
    sdfake_put(P, file, (size_t)n);
    appconfig_load("com.test.r");
    appconfig_put("a", "x");
    appconfig_put("b", "");
    if (!appconfig_save())
      continue;
    appconfig_load("com.test.r");
    CHECK_STR(appconfig_get("a", NULL), "x");
    CHECK(appconfig_save());
  }
  appconfig_unbind();
}

// A repeated key keeps its last copy, as JSON readers do.
static void test_repeated_key(void) {
  const char *P = "/data/com.test.d/config.json";
  sdfake_reset();
  sdfake_put(P, "{\"a\":\"1\",\"a\":\"2\"}", 17);
  CHECK(appconfig_load("com.test.d"));
  CHECK_STR(appconfig_get("a", NULL), "2");
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"a\":\"2\"}");
  appconfig_unbind();
}

static void test_round_trip(void) {
  sdfake_reset();
  CHECK(!appconfig_load("com.test.rt"));  // first run: no file
  appconfig_set("q", "say \"hi\" \\o/");
  appconfig_set("nl", "a\nb\tc");
  appconfig_set("json", "}{,:");
  appconfig_set("k\"ey", "escaped key");
  CHECK(appconfig_save());
  CHECK(sdfake_dir_exists("/data/com.test.rt"));
  CHECK(sdfake_get("/data/com.test.rt/config.json", NULL) != NULL);

  appconfig_clear();
  CHECK(appconfig_load("com.test.rt"));
  CHECK_STR(appconfig_get("q", NULL), "say \"hi\" \\o/");
  CHECK_STR(appconfig_get("nl", NULL), "a\nb\tc");
  CHECK_STR(appconfig_get("json", NULL), "}{,:");
  CHECK_STR(appconfig_get("k\"ey", NULL), "escaped key");
}

// Loading app B's store replaces app A's (the cross-app bleed in the review
// is the Lua bridge not calling load; see Task 5).
static void test_switch_app(void) {
  sdfake_reset();
  appconfig_load("com.test.a");
  appconfig_set("k", "A");
  CHECK(appconfig_save());
  appconfig_load("com.test.b");
  CHECK(appconfig_get("k", NULL) == NULL);
  CHECK_STR(appconfig_get_app_id(), "com.test.b");
  appconfig_set("k", "B");
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get("/data/com.test.a/config.json", NULL), "{\"k\":\"A\"}");
  CHECK_STR(sdfake_get("/data/com.test.b/config.json", NULL), "{\"k\":\"B\"}");
}

// The file path must hold any id the launcher accepts (app_entry_t.id is 80
// bytes): a long id used to be truncated into "/data/<id>/confi...".
static void test_long_app_id(void) {
  sdfake_reset();
  char id[80];
  memset(id, 'a', sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';  // 79 chars
  appconfig_load(id);
  CHECK_STR(appconfig_get_app_id(), id);
  appconfig_set("k", "v");
  CHECK(appconfig_save());
  char path[160];
  snprintf(path, sizeof(path), "/data/%s/config.json", id);
  CHECK_STR(sdfake_get(path, NULL), "{\"k\":\"v\"}");
  appconfig_clear();
  CHECK(appconfig_load(id));
  CHECK_STR(appconfig_get("k", NULL), "v");
  CHECK(appconfig_reset());
  CHECK(sdfake_get(path, NULL) == NULL);
}

static void test_overlong_file_entries(void) {
  sdfake_reset();
  char file[1024], big[400];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  snprintf(file, sizeof(file), "{\"long\":\"%s\",\"after\":\"ok\"}", big);
  sdfake_put("/data/com.t/config.json", file, strlen(file));
  CHECK(appconfig_load("com.t"));
  CHECK_STR(appconfig_get("after", NULL), "ok");
}

static void test_no_app_id(void) {
  sdfake_reset();
  CHECK(!appconfig_load(NULL));
  CHECK(!appconfig_load(""));
  // Longer than any launcher id: refused, never truncated into a path.
  char id[100];
  memset(id, 'b', sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';
  CHECK(!appconfig_load(id));
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(!appconfig_save());
}

static void test_reset(void) {
  sdfake_reset();
  appconfig_load("com.test.r");
  appconfig_set("k", "v");
  CHECK(appconfig_save());
  CHECK(appconfig_reset());
  CHECK(sdfake_get("/data/com.test.r/config.json", NULL) == NULL);
  CHECK(appconfig_get("k", NULL) == NULL);
}

// Between apps the store forgets its app: the next app's first get/set must
// not see (or save over) the previous app's entries, even unsaved ones.
static void test_unbind(void) {
  sdfake_reset();
  appconfig_load("com.test.u");
  appconfig_set("k", "unsaved");
  appconfig_unbind();
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(appconfig_get("k", NULL) == NULL);
  CHECK(!appconfig_save());
  CHECK(sdfake_get("/data/com.test.u/config.json", NULL) == NULL);
}

static void test_save_failure(void) {
  sdfake_reset();
  appconfig_load("com.test.f");
  appconfig_set("k", "v");
  sdfake_limit_writes(2);
  CHECK(!appconfig_save());
  sdfake_limit_writes(-1);
}

static void test_atomic_save(void) {
  const char *P = "/data/com.test.a/config.json";
  sdfake_reset();
  appconfig_load("com.test.a");
  appconfig_set("k", "1");
  CHECK(appconfig_save());
  appconfig_set("k", "2");
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
  CHECK(sdfake_get("/data/com.test.a/config.json.tmp", NULL) == NULL);
  CHECK(sdfake_get("/data/com.test.a/config.json.bak", NULL) == NULL);
  appconfig_set("k", "3");
  sdfake_fail_rename_after(1);            // moving the new file in fails
  CHECK(!appconfig_save());
  sdfake_fail_rename_after(-1);
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
  appconfig_set("k", "4");
  sdfake_limit_writes(2);
  CHECK(!appconfig_save());
  sdfake_limit_writes(-1);
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
}

int main(void) {
  // No app loaded yet: save/reset refuse.
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(!appconfig_save());
  CHECK(!appconfig_reset());
  test_limits();
  test_native_set();
  test_too_long();
  test_get_pointer_stable();
  test_load_save_keeps_the_rest();
  test_unreadable_file_is_not_overwritten();
  test_malformed_files();
  test_members_without_commas();
  test_random_files();
  test_repeated_key();
  test_round_trip();
  test_switch_app();
  test_long_app_id();
  test_overlong_file_entries();
  test_no_app_id();
  test_reset();
  test_unbind();
  test_save_failure();
  test_atomic_save();
  appconfig_unbind();
  sdfake_reset();
  return check_report("test_appconfig");
}
