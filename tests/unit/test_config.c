// Host unit tests for src/os/config.c (/system/config.json, the system-wide
// key/value store), over the in-memory SD fake.  Limits: keys 31 chars,
// values 127, all pairs in one CONFIG_POOL_SIZE-byte pool.
#include "check.h"
#include "config.h"
#include "fakes/sdcard_fake.h"

#include <string.h>

#define PATH "/system/config.json"

static void reset(void) {
  sdfake_reset();
  config_load();  // missing file: empty store
}

static void test_set_get_delete(void) {
  reset();
  CHECK(config_get("a") == NULL);
  config_set("a", "1");
  CHECK_STR(config_get("a"), "1");
  config_set("a", "2");  // overwrite
  CHECK_STR(config_get("a"), "2");
  config_set("a", "");   // empty value deletes
  CHECK(config_get("a") == NULL);
  config_set("b", "x");
  config_set("b", NULL);  // NULL deletes
  CHECK(config_get("b") == NULL);
  config_set("", "ignored");
  config_set(NULL, "ignored");
  CHECK(config_get("") == NULL);
}

// Fill the pool with pairs of `vlen`-char values ("k<i>"); returns how many
// were stored.
static int fill(int vlen) {
  char k[8], v[CONFIG_VAL_MAX];
  memset(v, 'v', (size_t)vlen);
  v[vlen] = '\0';
  int n = 0;
  for (;; n++) {
    snprintf(k, sizeof(k), "k%d", n);
    config_set(k, v);
    if (!config_get(k)) return n;
  }
}

static void test_pool_limit(void) {
  reset();
  // Each pair costs its key and value plus two NULs.
  int n = fill(100);
  CHECK_EQ_INT(n, CONFIG_POOL_SIZE / (2 + 1 + 100 + 1));
  char k[8], over[8];
  snprintf(over, sizeof(over), "k%d", n);
  CHECK(config_get(over) == NULL);          // one past the end is dropped
  CHECK_EQ_INT(strlen(config_get("k0")), 100);
  // Take all but one byte of what is left.
  char pad[CONFIG_VAL_MAX];
  int left = CONFIG_POOL_SIZE - n * (2 + 1 + 100 + 1);
  memset(pad, 'p', (size_t)(left - 4));
  pad[left - 4] = '\0';
  config_set("f", pad);
  CHECK_STR(config_get("f"), pad);
  // A longer value that does not fit keeps the old one; a shorter one fits.
  char longer[CONFIG_VAL_MAX];
  memset(longer, 'L', sizeof(longer) - 1);
  longer[sizeof(longer) - 1] = '\0';
  config_set("k1", longer);
  CHECK_EQ_INT(strlen(config_get("k1")), 100);
  config_set("k1", "short");
  CHECK_STR(config_get("k1"), "short");
  // The same length is rewritten in place.
  config_set("k1", "SHORT");
  CHECK_STR(config_get("k1"), "SHORT");
  // Deleting frees its bytes: an absent key costs nothing.
  config_set("k0", NULL);
  CHECK(config_get("k0") == NULL);
  config_set(over, "v");
  CHECK_STR(config_get(over), "v");
  for (int i = 2; i < n; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    CHECK_EQ_INT(strlen(config_get(k)), 100);
  }
}

// Every well-known key at its longest, and an 11th-plus key or three, fit
// with room to spare (the fixed 10-slot store was exactly full with them).
static void test_well_known_keys_fit(void) {
  reset();
  char url[CONFIG_VAL_MAX], ssid[33], pass[64];
  memset(url, 'u', sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';
  memset(ssid, 's', sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';
  memset(pass, 'p', sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';
  const char *keys[][2] = {
      {"wifi_ssid", ssid}, {"wifi_pass", pass}, {"brightness", "255"},
      {"dim_timeout_s", "86400"}, {"tz_offset", "-720"}, {"dev_mode", "1"},
      {"wifi_auto_disconnect", "0"}, {"battery_pct", "1"}, {"show_fps", "br"},
      {"editor_font", "2"}, {"store_url", url}, {"terminal_font", "1"},
      {"harness_key", "42"},
  };
  size_t nkeys = sizeof(keys) / sizeof(keys[0]);
  for (size_t i = 0; i < nkeys; i++)
    config_set(keys[i][0], keys[i][1]);
  for (size_t i = 0; i < nkeys; i++)
    CHECK_STR(config_get(keys[i][0]), keys[i][1]);
  // At least as much again of room left.
  CHECK(fill(1) >= 40);
  CHECK(config_save());
  CHECK(config_load());
  CHECK_STR(config_get("store_url"), url);
  CHECK_STR(config_get("wifi_pass"), pass);
}

// Arguments that point into the pool itself (another key's value, or the
// key's own) survive the pool moving under them.
static void test_set_from_pool(void) {
  reset();
  config_set("a", "alpha");
  config_set("b", "bravo-longer");
  config_set("c", "charlie");
  config_set("a", config_get("c"));  // grows: a moves to the end
  CHECK_STR(config_get("a"), "charlie");
  config_set("b", config_get("b"));
  CHECK_STR(config_get("b"), "bravo-longer");
  config_set("c", config_get("b"));
  CHECK_STR(config_get("c"), "bravo-longer");
  CHECK_STR(config_get("a"), "charlie");
}

static void test_length_limits(void) {
  reset();
  char val[300];
  memset(val, 'v', sizeof(val) - 1);
  val[sizeof(val) - 1] = '\0';
  config_set("long", val);
  CHECK_EQ_INT(strlen(config_get("long")), CONFIG_VAL_MAX - 1);

  char key[64];
  memset(key, 'k', sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';
  config_set(key, "x");
  // The key is stored truncated to 31 chars, so the full key no longer
  // finds it (documented limit), but the truncated one does.
  CHECK(config_get(key) == NULL);
  key[CONFIG_KEY_MAX - 1] = '\0';
  CHECK_STR(config_get(key), "x");
}

static void test_round_trip_special_chars(void) {
  reset();
  config_set("quote", "say \"hi\"");
  config_set("slash", "a\\b\\");
  config_set("nl", "line1\nline2\ttab");
  config_set("json", "}{,:\"");
  config_set("wifi_pass", "p@ss w0rd!");
  CHECK(config_save());
  const char *file = sdfake_get(PATH, NULL);
  CHECK(file != NULL);

  config_set("quote", NULL);  // drop in-memory state
  CHECK(config_load());
  CHECK_STR(config_get("quote"), "say \"hi\"");
  CHECK_STR(config_get("slash"), "a\\b\\");
  CHECK_STR(config_get("nl"), "line1\nline2\ttab");
  CHECK_STR(config_get("json"), "}{,:\"");
  CHECK_STR(config_get("wifi_pass"), "p@ss w0rd!");
}

static void test_round_trip_full_store(void) {
  reset();
  char k[8], v[CONFIG_VAL_MAX];
  memset(v, '"', sizeof(v) - 1);  // worst case: every char escaped
  v[sizeof(v) - 1] = '\0';
  int n = 0;
  for (;; n++) {
    snprintf(k, sizeof(k), "k%d", n);
    config_set(k, v);
    if (!config_get(k)) break;
  }
  CHECK(n >= 2);
  CHECK(config_save());
  CHECK(config_load());
  for (int i = 0; i < n; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    CHECK_STR(config_get(k), v);
  }
}

// A key containing '"' or '\' is escaped by config_save, so config_load
// must unescape keys as well as values.
static void test_key_escapes_round_trip(void) {
  reset();
  config_set("a\"b", "1");
  config_set("c\\d", "2");
  config_set("e", "3");
  CHECK(config_save());
  CHECK(config_load());
  CHECK_STR(config_get("a\"b"), "1");
  CHECK_STR(config_get("c\\d"), "2");
  CHECK_STR(config_get("e"), "3");
}

// A hand-edited file with an over-long value or key must not desynchronise
// the parser: the following entries still load.
static void test_overlong_file_entries(void) {
  reset();
  char file[1024];
  char big[300];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  snprintf(file, sizeof(file), "{\"long\":\"%s\",\"after\":\"ok\"}", big);
  sdfake_put(PATH, file, strlen(file));
  CHECK(config_load());
  CHECK_EQ_INT(strlen(config_get("long") ? config_get("long") : ""), CONFIG_VAL_MAX - 1);
  CHECK_STR(config_get("after"), "ok");

  big[60] = '\0';
  snprintf(file, sizeof(file), "{\"%s\":\"v\",\"after\":\"ok\"}", big);
  sdfake_put(PATH, file, strlen(file));
  CHECK(config_load());
  CHECK_STR(config_get("after"), "ok");
}

static void test_load_tolerates(void) {
  reset();
  // Whitespace, a non-string value (skipped), empty key (skipped).
  const char *f = "{ \"a\" : \"1\", \"n\": 42, \"\": \"z\", \"b\":\"2\" }";
  sdfake_put(PATH, f, strlen(f));
  CHECK(config_load());
  CHECK_STR(config_get("a"), "1");
  CHECK_STR(config_get("b"), "2");
  CHECK(config_get("n") == NULL);
  // Empty / truncated files.
  sdfake_put(PATH, "", 0);
  CHECK(config_load());
  CHECK(config_get("a") == NULL);
  sdfake_put(PATH, "{\"a\":\"unterminated", 18);
  CHECK(config_load());
  CHECK_STR(config_get("a"), "unterminated");
  sdfake_put(PATH, "{\"a", 3);
  CHECK(config_load());
  CHECK(config_get("a") == NULL);
  // More in the file than the pool holds: the pairs that fit load, one
  // that does not is dropped, and a smaller one after it still loads.
  static char many[4096];
  char e[160], big[101];
  memset(big, 'b', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  strcpy(many, "{");
  int nbig = CONFIG_POOL_SIZE / (2 + 1 + 100 + 1) + 1;
  for (int i = 0; i < nbig; i++) {
    snprintf(e, sizeof(e), "%s\"%d\":\"%s\"", i ? "," : "", i, big);
    strcat(many, e);
  }
  strcat(many, ",\"z\":\"small\"}");
  sdfake_put(PATH, many, strlen(many));
  CHECK(config_load());
  char last[8], over[8];
  snprintf(last, sizeof(last), "%d", nbig - 2);
  snprintf(over, sizeof(over), "%d", nbig - 1);
  CHECK_STR(config_get(last), big);
  CHECK(config_get(over) == NULL);
  CHECK_STR(config_get("z"), "small");
  // Missing file.
  sdfake_reset();
  CHECK(!config_load());
}

static void test_save_failures(void) {
  reset();
  config_set("a", "1");
  sdfake_limit_writes(3);
  CHECK(!config_save());  // truncated write reported
  sdfake_limit_writes(-1);
  CHECK(config_save());
  size_t n;
  CHECK_STR(sdfake_get(PATH, &n), "{\"a\":\"1\"}");
}

static void test_brightness_parse(void) {
  CHECK_EQ_INT(config_parse_brightness(NULL), 128);
  CHECK_EQ_INT(config_parse_brightness(""), 128);
  CHECK_EQ_INT(config_parse_brightness("0"), 16);
  CHECK_EQ_INT(config_parse_brightness("200"), 200);
  CHECK_EQ_INT(config_parse_brightness("999"), 255);
  CHECK_EQ_INT(config_parse_brightness("junk"), 16);
}

// Saves go through config.json.tmp and a rename, so a power loss or a failed
// step never leaves a truncated /system/config.json.
static void test_atomic_save(void) {
  reset();
  config_set("wifi_ssid", "home");
  CHECK(config_save());
  CHECK_STR(sdfake_get(PATH, NULL), "{\"wifi_ssid\":\"home\"}");
  CHECK(sdfake_get(PATH ".tmp", NULL) == NULL);
  CHECK(sdfake_get(PATH ".bak", NULL) == NULL);
  // Second save replaces an existing file (FatFS will not rename over it).
  config_set("wifi_ssid", "work");
  CHECK(config_save());
  CHECK_STR(sdfake_get(PATH, NULL), "{\"wifi_ssid\":\"work\"}");
  CHECK(sdfake_get(PATH ".tmp", NULL) == NULL);
  CHECK(sdfake_get(PATH ".bak", NULL) == NULL);
}

static void test_atomic_save_failures_keep_old_file(void) {
  const char *old = "{\"wifi_ssid\":\"work\"}";
  // Truncated write: the target is untouched, the partial .tmp removed.
  reset();
  sdfake_put(PATH, old, strlen(old));
  config_set("wifi_ssid", "new");
  sdfake_limit_writes(3);
  CHECK(!config_save());
  sdfake_limit_writes(-1);
  CHECK_STR(sdfake_get(PATH, NULL), old);
  CHECK(sdfake_get(PATH ".tmp", NULL) == NULL);
  // Every rename of the sequence failing in turn: false, old contents kept.
  for (int n = 0; n < 3; n++) {
    reset();
    sdfake_put(PATH, old, strlen(old));
    config_set("wifi_ssid", "new");
    sdfake_fail_rename_after(n);
    bool ok = config_save();
    sdfake_fail_rename_after(-1);
    if (n < 2) {                      // moving old aside, or the new file in
      CHECK(!ok);
      CHECK_STR(sdfake_get(PATH, NULL), old);
      CHECK(sdfake_get(PATH ".tmp", NULL) == NULL);
    } else {                          // only the restore rename left: unused
      CHECK(ok);
      CHECK_STR(sdfake_get(PATH, NULL), "{\"wifi_ssid\":\"new\"}");
    }
  }
  // Power lost between moving the old file aside and moving the new one in:
  // the next load recovers the old file.
  reset();
  sdfake_put(PATH ".bak", old, strlen(old));
  sdfake_put(PATH ".tmp", "{\"x\"", 5);
  CHECK(config_load());
  CHECK_STR(config_get("wifi_ssid"), "work");
  CHECK_STR(sdfake_get(PATH, NULL), old);
}

int main(void) {
  test_set_get_delete();
  test_pool_limit();
  test_well_known_keys_fit();
  test_set_from_pool();
  test_length_limits();
  test_round_trip_special_chars();
  test_round_trip_full_store();
  test_key_escapes_round_trip();
  test_overlong_file_entries();
  test_load_tolerates();
  test_save_failures();
  test_brightness_parse();
  test_atomic_save();
  test_atomic_save_failures_keep_old_file();
  sdfake_reset();
  return check_report("test_config");
}
