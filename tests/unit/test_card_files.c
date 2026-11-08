// Host unit tests for src/os/card_files.c: the firmware writes the files it owns
// (the App Store, System Update, /system/lib) to the card at boot, over the
// in-memory SD fake. Issue #92.
#include "card_files.h"
#include "check.h"
#include "fakes/sdcard_fake.h"
#include "sdcard.h"

#include <stdio.h>
#include <string.h>

// ── A small bundle, in two of the roots ──────────────────────────────────────

static const uint8_t k_app[] = "{\"id\":\"net.picodeck.store\"}";
static const uint8_t k_main[] = "return 'bundled'\n";
static const uint8_t k_font[] = {0x00, 0x01, 0x02, 0x03};

static const card_file_t k_files[] = {
    {"/apps/store/app.json", k_app, sizeof(k_app) - 1},
    {"/apps/store/main.lua", k_main, sizeof(k_main) - 1},
    {"/system/lib/fonts/f.pfn", k_font, sizeof(k_font)},
};
static const char *const k_roots[] = {"/apps/store/", "/apps/updater/", "/system/lib/"};

#define ID_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define ID_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static const card_bundle_t k_bundle_a = {ID_A, k_files, 3, k_roots, 3};
static const card_bundle_t k_bundle_b = {ID_B, k_files, 3, k_roots, 3};

static const char k_marker_a[] =
    "picodeck-card-files 1\n"
    "id " ID_A "\n"
    "/apps/store/app.json\n"
    "/apps/store/main.lua\n"
    "/system/lib/fonts/f.pfn\n";

static bool same_bytes(const char *path, const void *want, size_t len) {
  size_t got_len = 0;
  const char *got = sdfake_get(path, &got_len);
  return got && got_len == len && memcmp(got, want, len) == 0;
}

static bool has_bundle(void) {
  for (int i = 0; i < 3; i++)
    if (!same_bytes(k_files[i].path, k_files[i].data, k_files[i].len))
      return false;
  return true;
}

static void put_str(const char *path, const char *s) { sdfake_put(path, s, strlen(s)); }

static bool sync(const card_bundle_t *b, card_files_result_t *r) {
  memset(r, 0x5a, sizeof(*r));  // the function must set every field
  return card_files_sync(b, r);
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void test_blank_card_gets_every_file_and_the_marker(void) {
  sdfake_reset();
  card_files_result_t r;
  CHECK(sync(&k_bundle_a, &r));
  CHECK(has_bundle());
  CHECK(sdfake_dir_exists("/apps/store"));
  CHECK(sdfake_dir_exists("/system/lib"));
  CHECK(sdfake_dir_exists("/system/lib/fonts"));
  CHECK_STR(sdfake_get(CARD_FILES_MARKER, NULL), k_marker_a);
  CHECK_EQ_INT(r.written, 3);
  CHECK_EQ_INT(r.deleted, 0);
  CHECK_EQ_INT(r.failed, 0);
  CHECK_EQ_INT(r.bytes, sizeof(k_app) - 1 + sizeof(k_main) - 1 + sizeof(k_font));
  CHECK_STR(r.error, "");
}

static void test_second_run_writes_nothing(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  int renames = sdfake_renames();
  CHECK(sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.written, 0);
  CHECK_EQ_INT(r.deleted, 0);
  CHECK_EQ_INT(r.failed, 0);
  CHECK_EQ_INT(r.bytes, 0);
  CHECK_EQ_INT(sdfake_renames(), renames);
}

static void test_deleted_file_comes_back_alone(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  sdcard_delete("/system/lib/fonts/f.pfn");
  CHECK(sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.written, 1);
  CHECK(has_bundle());
}

static void test_size_change_is_rewritten(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  put_str("/apps/store/main.lua", "x");
  CHECK(sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.written, 1);
  CHECK(has_bundle());
}

// Review Focus 5: normal boots compare sizes, not contents (the spec's limit).
static void test_same_size_edit_survives_until_bundle_changes(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  char edited[sizeof(k_main)];
  memcpy(edited, k_main, sizeof(k_main));
  edited[0] = 'R';
  sdfake_put("/apps/store/main.lua", edited, sizeof(k_main) - 1);
  CHECK(sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.written, 0);
  CHECK(same_bytes("/apps/store/main.lua", edited, sizeof(k_main) - 1));
  CHECK(sync(&k_bundle_b, &r));  // a firmware change
  CHECK_EQ_INT(r.written, 3);
  CHECK(has_bundle());
}

static void test_new_bundle_rewrites_all_and_drops_stale(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  char marker[512];
  snprintf(marker, sizeof(marker), "%s/apps/store/old.lua\n", k_marker_a);
  put_str(CARD_FILES_MARKER, marker);
  put_str("/apps/store/old.lua", "old");
  CHECK(sync(&k_bundle_b, &r));
  CHECK_EQ_INT(r.written, 3);
  CHECK_EQ_INT(r.deleted, 1);
  CHECK_EQ_INT(r.failed, 0);
  CHECK(sdfake_get("/apps/store/old.lua", NULL) == NULL);
  const char *m = sdfake_get(CARD_FILES_MARKER, NULL);
  CHECK(m && strstr(m, "id " ID_B "\n") != NULL);
  CHECK(m && strstr(m, "old.lua") == NULL);
}

static void test_stale_file_already_gone_is_not_a_failure(void) {
  sdfake_reset();
  char marker[512];
  snprintf(marker, sizeof(marker), "%s/apps/store/gone.lua\n", k_marker_a);
  put_str(CARD_FILES_MARKER, marker);
  card_files_result_t r;
  CHECK(sync(&k_bundle_b, &r));
  CHECK_EQ_INT(r.deleted, 0);
  CHECK_EQ_INT(r.failed, 0);
}

static void test_files_outside_the_bundle_are_untouched(void) {
  sdfake_reset();
  put_str("/apps/other/main.lua", "return 'mine'\n");
  put_str("/data/net.picodeck.store/catalog.json", "{\"apps\":[]}");
  put_str("/system/config.json", "{\"wifi_ssid\":\"home\"}");
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  CHECK(sync(&k_bundle_b, &r));
  CHECK_STR(sdfake_get("/apps/other/main.lua", NULL), "return 'mine'\n");
  CHECK_STR(sdfake_get("/data/net.picodeck.store/catalog.json", NULL), "{\"apps\":[]}");
  CHECK_STR(sdfake_get("/system/config.json", NULL), "{\"wifi_ssid\":\"home\"}");
}

// Review Focus 1: a marker's word is trusted only inside the roots.
static void test_marker_paths_outside_roots_are_never_deleted(void) {
  sdfake_reset();
  put_str("/system/config.json", "{}");
  put_str("/data/x/y", "keep");
  put_str("/apps/storex/main.lua", "keep");  // shares a prefix, not a root
  put_str(CARD_FILES_MARKER,
          "picodeck-card-files 1\n"
          "id " ID_A "\n"
          "/system/config.json\n"
          "/data/x/y\n"
          "/apps/storex/main.lua\n"
          "/apps/store/../../system/config.json\n");
  card_files_result_t r;
  CHECK(sync(&k_bundle_b, &r));
  CHECK_EQ_INT(r.deleted, 0);
  CHECK_STR(sdfake_get("/system/config.json", NULL), "{}");
  CHECK_STR(sdfake_get("/data/x/y", NULL), "keep");
  CHECK_STR(sdfake_get("/apps/storex/main.lua", NULL), "keep");
}

static void test_corrupt_marker_counts_as_none(void) {
  static const char *const bad[] = {
      "garbage",
      "",
      "picodeck-card-files 2\nid " ID_A "\n",
      "picodeck-card-files 1\nid short\n",
      "picodeck-card-files 1\n",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    sdfake_reset();
    put_str("/apps/store/old.lua", "old");
    put_str(CARD_FILES_MARKER, bad[i]);
    card_files_result_t r;
    CHECK(sync(&k_bundle_a, &r));
    CHECK_EQ_INT(r.written, 3);
    CHECK_EQ_INT(r.deleted, 0);
    CHECK(sdfake_get("/apps/store/old.lua", NULL) != NULL);
    CHECK_STR(sdfake_get(CARD_FILES_MARKER, NULL), k_marker_a);
  }
}

static void test_unreadable_marker_counts_as_none(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  sdfake_fail_reads(true);
  CHECK(sync(&k_bundle_a, &r));
  sdfake_fail_reads(false);
  CHECK_EQ_INT(r.written, 3);
  CHECK(has_bundle());
}

// Review Focus 2. sd_atomic_write opens <path>.tmp, so that is what won't open.
static void test_one_unwritable_file_does_not_stop_the_rest(void) {
  sdfake_reset();
  sdfake_lock_file("/apps/store/main.lua.tmp");
  card_files_result_t r;
  CHECK(!sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.failed, 1);
  CHECK_EQ_INT(r.written, 2);
  CHECK(strstr(r.error, "/apps/store/main.lua") != NULL);
  CHECK(same_bytes("/apps/store/app.json", k_app, sizeof(k_app) - 1));
  CHECK(same_bytes("/system/lib/fonts/f.pfn", k_font, sizeof(k_font)));
  CHECK(sdfake_get(CARD_FILES_MARKER, NULL) == NULL);
  sdfake_lock_file(NULL);
  CHECK(sync(&k_bundle_a, &r));
  CHECK(has_bundle());
  CHECK_STR(sdfake_get(CARD_FILES_MARKER, NULL), k_marker_a);
}

// Review Focus 3: a full card.
static void test_short_write_keeps_old_file_and_retries(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  char edited[] = "an older store that is longer than the bundled one\n";
  put_str("/apps/store/main.lua", edited);
  sdfake_limit_writes(10);
  CHECK(!sync(&k_bundle_b, &r));
  CHECK(r.failed >= 1);
  CHECK_STR(sdfake_get("/apps/store/main.lua", NULL), edited);  // never truncated
  const char *m = sdfake_get(CARD_FILES_MARKER, NULL);
  CHECK(m && strstr(m, ID_B) == NULL);
  sdfake_limit_writes(-1);
  CHECK(sync(&k_bundle_b, &r));
  CHECK(has_bundle());
  m = sdfake_get(CARD_FILES_MARKER, NULL);
  CHECK(m && strstr(m, "id " ID_B "\n") != NULL);
}

// Review Focus 4: power lost mid-write on the device.
static void test_interrupted_rename_is_repaired_next_run(void) {
  sdfake_reset();
  card_files_result_t r;
  sync(&k_bundle_a, &r);
  put_str("/apps/store/main.lua", "older");
  sdfake_fail_rename_after(0);
  CHECK(!sync(&k_bundle_b, &r));
  CHECK(r.failed >= 1);
  CHECK(sync(&k_bundle_b, &r));
  CHECK(has_bundle());
  const char *m = sdfake_get(CARD_FILES_MARKER, NULL);
  CHECK(m && strstr(m, "id " ID_B "\n") != NULL);
}

static void test_marker_write_failure_is_reported(void) {
  sdfake_reset();
  sdfake_lock_file(CARD_FILES_MARKER ".tmp");  // sd_atomic_write's temporary
  card_files_result_t r;
  CHECK(!sync(&k_bundle_a, &r));
  CHECK_EQ_INT(r.failed, 1);
  CHECK(strstr(r.error, CARD_FILES_MARKER) != NULL);
  CHECK(has_bundle());
}

int main(void) {
  test_blank_card_gets_every_file_and_the_marker();
  test_second_run_writes_nothing();
  test_deleted_file_comes_back_alone();
  test_size_change_is_rewritten();
  test_same_size_edit_survives_until_bundle_changes();
  test_new_bundle_rewrites_all_and_drops_stale();
  test_stale_file_already_gone_is_not_a_failure();
  test_files_outside_the_bundle_are_untouched();
  test_marker_paths_outside_roots_are_never_deleted();
  test_corrupt_marker_counts_as_none();
  test_unreadable_marker_counts_as_none();
  test_one_unwritable_file_does_not_stop_the_rest();
  test_short_write_keeps_old_file_and_retries();
  test_interrupted_rename_is_repaired_next_run();
  test_marker_write_failure_is_reported();
  return check_report("test_card_files");
}
