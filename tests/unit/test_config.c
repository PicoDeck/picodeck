// Host unit tests for src/os/config.c (/system/config.json, the system-wide
// key/value store), over the in-memory SD fake.  Limits per
// specs/test-audit-2026-09-24.md §3.9: 8 entries, keys 31 chars, values 127.
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

static void test_entry_limit(void) {
  reset();
  char k[8], last[8], over[8];
  for (int i = 0; i < CONFIG_MAX_ENTRIES + 1; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    config_set(k, "v");
  }
  snprintf(last, sizeof(last), "k%d", CONFIG_MAX_ENTRIES - 1);
  snprintf(over, sizeof(over), "k%d", CONFIG_MAX_ENTRIES);
  CHECK_STR(config_get("k0"), "v");
  CHECK_STR(config_get(last), "v");
  CHECK(config_get(over) == NULL);  // one past the cap is silently dropped
  // Deleting frees a slot.
  config_set("k0", NULL);
  config_set(over, "v");
  CHECK_STR(config_get(over), "v");
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
  for (int i = 0; i < CONFIG_MAX_ENTRIES; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    config_set(k, v);
  }
  CHECK(config_save());
  CHECK(config_load());
  for (int i = 0; i < CONFIG_MAX_ENTRIES; i++) {
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
  // More entries in the file than the store holds: the first
  // CONFIG_MAX_ENTRIES load.
  char many[256] = "{", e[16], last[8], over[8];
  for (int i = 1; i <= CONFIG_MAX_ENTRIES + 1; i++) {
    snprintf(e, sizeof(e), "%s\"%d\":\"a\"", i > 1 ? "," : "", i);
    strcat(many, e);
  }
  strcat(many, "}");
  sdfake_put(PATH, many, strlen(many));
  CHECK(config_load());
  snprintf(last, sizeof(last), "%d", CONFIG_MAX_ENTRIES);
  snprintf(over, sizeof(over), "%d", CONFIG_MAX_ENTRIES + 1);
  CHECK_STR(config_get(last), "a");
  CHECK(config_get(over) == NULL);
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
  test_entry_limit();
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
