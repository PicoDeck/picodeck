// Host unit tests for src/os/app_manifest.c (the launcher's app.json reader).
#include "check.h"
#include "app_manifest.h"

#include <stdlib.h>
#include <string.h>

static app_entry_t parse(const char *json) {
  app_entry_t a;
  memset(&a, 0x5a, sizeof(a));  // prove every manifest field is written
  app_manifest_parse(json, strlen(json), "mydir", &a);
  return a;
}

static void test_full_manifest(void) {
  app_entry_t a = parse(
      "{\n"
      "  \"id\": \"com.example.app\",\n"
      "  \"name\": \"Example\",\n"
      "  \"description\": \"Does things\",\n"
      "  \"version\": \"2.1\",\n"
      "  \"author\": \"me\",\n"
      "  \"category\": \"games\",\n"
      "  \"requirements\": [\"http\", \"audio\"],\n"
      "  \"system_clock_khz\": 250000,\n"
      "  \"min_psram_kb\": 4096\n"
      "}\n");
  CHECK_STR(a.id, "com.example.app");
  CHECK_STR(a.name, "Example");
  CHECK_STR(a.description, "Does things");
  CHECK_STR(a.version, "2.1");
  CHECK_STR(a.category, "games");
  CHECK(a.has_http);
  CHECK(a.has_audio);
  CHECK(!a.has_root_filesystem);
  CHECK_EQ_U32(a.system_clock_khz, 250000);
  CHECK_EQ_U32(a.min_psram_kb, 4096);
}

static void test_defaults(void) {
  app_entry_t a = parse("{}");
  CHECK_STR(a.id, "local.mydir");
  CHECK_STR(a.name, "mydir");
  CHECK_STR(a.description, "");
  CHECK_STR(a.version, "1.0");
  CHECK_STR(a.category, "");
  CHECK(!a.has_http && !a.has_audio && !a.has_root_filesystem);
  CHECK_EQ_U32(a.system_clock_khz, 0);
  CHECK_EQ_U32(a.min_psram_kb, 0);

  app_entry_t b;
  memset(&b, 0x5a, sizeof(b));
  app_manifest_defaults("other", &b);
  CHECK_STR(b.id, "local.other");
  CHECK_STR(b.name, "other");
  CHECK_STR(b.version, "?");
  CHECK_STR(b.description, "");
  CHECK_STR(b.category, "");
  CHECK(!b.has_http);
  CHECK_EQ_U32(b.min_psram_kb, 0);
}

// The review's Low row: "The launcher's JSON reader ignores escapes."
static void test_escapes(void) {
  app_entry_t a = parse(
      "{\"name\": \"Say \\\"hi\\\"\", \"description\": \"a\\\\b\\/c\\td\\ne\","
      " \"version\": \"\\u0041\\u00e9\"}");
  CHECK_STR(a.name, "Say \"hi\"");
  CHECK_STR(a.description, "a\\b/c\td\ne");
  CHECK_STR(a.version, "A?");
  // An escaped quote must not end the value early and leak into the next key.
  app_entry_t b = parse("{\"description\": \"x \\\"id\\\": \\\"evil\\\"\", \"id\": \"good\"}");
  CHECK_STR(b.id, "good");
  CHECK_STR(b.description, "x \"id\": \"evil\"");
}

static void test_key_inside_value(void) {
  // "name" appears inside the description's value first: skipped, because
  // it is not followed by a string value.
  app_entry_t a = parse("{\"description\": \"the \\\"name\\\" key\", \"name\": \"Real\"}");
  CHECK_STR(a.name, "Real");
  app_entry_t b = parse("{\"tags\": [\"name\"], \"name\": \"Real\"}");
  CHECK_STR(b.name, "Real");
}

static void test_requirements(void) {
  app_entry_t a = parse("{\"requirements\": [\"root-filesystem\"]}");
  CHECK(a.has_root_filesystem);
  // A requirement name outside the array does not count.
  app_entry_t b = parse("{\"requirements\": [], \"note\": \"http\"}");
  CHECK(!b.has_http);
  app_entry_t c = parse("{\"requirements\": [\"https\"]}");
  CHECK(!c.has_http);  // exact token only
  app_entry_t d = parse("{\"requirements\": [\"http\"");  // unterminated array
  CHECK(!d.has_http);
  app_entry_t e = parse("{\"requirements\": \"http\"}");  // not an array
  CHECK(!e.has_http);
}

// The set of requirement names, for requirements without a bool of their own
// (Task 7 gates sysconfig / system-update on it).
static void test_requirement_list(void) {
  app_entry_t a = parse(
      "{\"requirements\": [\"http\", \"audio\", \"sysconfig\", \"system-update\"]}");
  CHECK_STR(a.requirements, "http audio sysconfig system-update");
  CHECK(app_requirements_has(a.requirements, "http"));
  CHECK(app_requirements_has(a.requirements, "sysconfig"));
  CHECK(app_requirements_has(a.requirements, "system-update"));
  CHECK(!app_requirements_has(a.requirements, "system"));  // whole names only
  CHECK(!app_requirements_has(a.requirements, "update"));
  CHECK(!app_requirements_has(a.requirements, "root-filesystem"));
  CHECK(!app_requirements_has(a.requirements, ""));
  CHECK(!app_requirements_has(NULL, "http"));

  { app_entry_t t1 = parse("{}"); CHECK_STR(t1.requirements, ""); }
  { app_entry_t t2 = parse("{\"requirements\": []}"); CHECK_STR(t2.requirements, ""); }
  // Names outside [a-z0-9._-] (after folding) and non-strings are dropped;
  // a name that does not fit is dropped whole, never cut.
  app_entry_t b = parse(
      "{\"requirements\": [\"HTTP\", 7, \"a b\", \"\", \"audio\"]}");
  CHECK_STR(b.requirements, "http audio");
  CHECK(b.has_http);  // the bools read the same folded set
  char big[512];
  int n = snprintf(big, sizeof(big), "{\"requirements\": [\"%0*d\", \"http\"]}",
                   (int)sizeof(a.requirements) + 4, 0);
  CHECK(n > 0 && (size_t)n < sizeof(big));
  { app_entry_t t3 = parse(big); CHECK_STR(t3.requirements, "http"); }
  // The unterminated / non-array forms give an empty set, as the bools do.
  { app_entry_t t4 = parse("{\"requirements\": [\"http\""); CHECK_STR(t4.requirements, ""); }
  { app_entry_t t5 = parse("{\"requirements\": \"http\"}"); CHECK_STR(t5.requirements, ""); }
}

// FatFS is case-insensitive: ids are folded to lower case so "/data/<id>"
// has one spelling per app.  Invalid ids are kept (folded) for the launcher
// to refuse.
static void test_id_folding(void) {
  { app_entry_t t6 = parse("{\"id\": \"Com.Example.App\"}"); CHECK_STR(t6.id, "com.example.app"); }
  { app_entry_t t7 = parse("{\"id\": \"../Evil\"}"); CHECK_STR(t7.id, "../evil"); }
  CHECK(!app_manifest_id_valid(parse("{\"id\": \"../Evil\"}").id));
  // Default ids come from the directory name and are always valid.
  app_entry_t a;
  app_manifest_parse("{}", 2, "My Game", &a);
  CHECK_STR(a.id, "local.my_game");
  CHECK(app_manifest_id_valid(a.id));
  app_manifest_defaults("a..b", &a);
  CHECK_STR(a.id, "local.a._b");
  CHECK(app_manifest_id_valid(a.id));
  app_manifest_defaults("Caf\xc3\xa9", &a);
  CHECK_STR(a.id, "local.caf__");
  CHECK(app_manifest_id_valid(a.id));
  // A trailing dot (host filesystems keep one) becomes '_'.
  app_manifest_defaults("game.", &a);
  CHECK_STR(a.id, "local.game_");
  CHECK(app_manifest_id_valid(a.id));
  // ...also when truncation to 79 chars leaves a dot last.
  char dir[100];
  memset(dir, 'b', sizeof(dir));
  dir[72] = '.';  // "local." (6) + 73 chars = 79: the dot is the last kept
  dir[99] = '\0';
  app_manifest_defaults(dir, &a);
  CHECK(strlen(a.id) == 79);
  CHECK(app_manifest_id_valid(a.id));
}

static void test_ints(void) {
  CHECK_EQ_U32(parse("{\"min_psram_kb\":12}").min_psram_kb, 12);
  CHECK_EQ_U32(parse("{\"min_psram_kb\": \"12\"}").min_psram_kb, 0);  // atoi("\"12\"")
  CHECK_EQ_U32(parse("{\"min_psram_kb\": abc}").min_psram_kb, 0);
  CHECK_EQ_U32(parse("{\"min_psram_kb\": -1}").min_psram_kb, 0xFFFFFFFFu);
  CHECK_EQ_U32(parse("{\"system_clock_khz\":\n 300000}").system_clock_khz, 300000);
}

// system_clock_khz: only the validated clocks; anything else is the default.
static void test_system_clock_table(void) {
  static const uint32_t ok[] = {125000, 150000, 200000, 250000, 300000};
  char j[64];
  for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
    snprintf(j, sizeof(j), "{\"system_clock_khz\": %lu}", (unsigned long)ok[i]);
    CHECK_EQ_U32(parse(j).system_clock_khz, ok[i]);
  }
  CHECK_EQ_U32(parse("{\"system_clock_khz\": 0}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": 999999}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": -200000}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": 133000}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": 400000}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": 1}").system_clock_khz, 0);
  CHECK_EQ_U32(parse("{\"system_clock_khz\": \"250000\"}").system_clock_khz, 0);
}

static void test_truncation_and_bounds(void) {
  // Values longer than the field are truncated, NUL-terminated.
  char big[400];
  memset(big, 'x', sizeof(big));
  memcpy(big, "{\"version\": \"", 13);
  big[sizeof(big) - 3] = '"';
  big[sizeof(big) - 2] = '}';
  big[sizeof(big) - 1] = '\0';
  app_entry_t a = parse(big);
  CHECK_EQ_INT(strlen(a.version), sizeof(a.version) - 1);

  // len bounds the scan: the id after len is not seen.
  const char *j = "{\"name\": \"A\"}{\"id\": \"hidden\"}";
  app_entry_t b;
  app_manifest_parse(j, 13, "d", &b);
  CHECK_STR(b.name, "A");
  CHECK_STR(b.id, "local.d");

  // Unterminated string at the end of the buffer: exact-size heap copy so
  // ASan catches any overread.
  const char *u = "{\"name\": \"abc";
  size_t n = strlen(u);
  char *h = malloc(n);
  memcpy(h, u, n);  // no NUL
  app_entry_t c;
  app_manifest_parse(h, n, "d", &c);
  CHECK_STR(c.name, "abc");
  free(h);

  // Truncated escape at the end.
  const char *t = "{\"name\": \"ab\\";
  n = strlen(t);
  h = malloc(n);
  memcpy(h, t, n);
  app_manifest_parse(h, n, "d", &c);
  CHECK_STR(c.name, "ab\\");
  free(h);
  const char *t2 = "{\"name\": \"ab\\u00";
  n = strlen(t2);
  h = malloc(n);
  memcpy(h, t2, n);
  app_manifest_parse(h, n, "d", &c);
  CHECK_STR(c.name, "ab?");
  free(h);

  // NULL json behaves as an empty manifest.
  app_manifest_parse(NULL, 0, "d", &c);
  CHECK_STR(c.id, "local.d");
}

static void test_id_valid(void) {
  CHECK(app_manifest_id_valid("com.example.app"));
  CHECK(app_manifest_id_valid("local.my_dir-2"));
  CHECK(app_manifest_id_valid("A"));
  CHECK(!app_manifest_id_valid(NULL));
  CHECK(!app_manifest_id_valid(""));
  CHECK(!app_manifest_id_valid("."));
  CHECK(!app_manifest_id_valid(".."));
  CHECK(!app_manifest_id_valid("../evil"));
  CHECK(!app_manifest_id_valid("../../system"));
  CHECK(!app_manifest_id_valid("com..x"));
  CHECK(!app_manifest_id_valid("a/b"));
  CHECK(!app_manifest_id_valid("a\\b"));
  CHECK(!app_manifest_id_valid("has space"));
  CHECK(!app_manifest_id_valid("tab\t"));
  CHECK(!app_manifest_id_valid("caf\xc3\xa9"));
  CHECK(!app_manifest_id_valid("a:b"));
  // FatFS strips trailing dots: "com.victim." would alias /data/com.victim.
  CHECK(!app_manifest_id_valid("com.victim."));
  CHECK(!app_manifest_id_valid("a."));
  CHECK(app_manifest_id_valid(".hidden"));
  char longid[100];
  memset(longid, 'a', sizeof(longid));
  longid[79] = '\0';  // 79 chars: fits app_entry_t.id[80]
  CHECK(app_manifest_id_valid(longid));
  longid[79] = 'a';
  longid[80] = '\0';  // 80 chars: does not
  CHECK(!app_manifest_id_valid(longid));
}

// Every committed app.json parses to a valid id.
static void test_hidden_name_valid(void) {
  CHECK(app_hidden_name_valid("probe"));
  CHECK(app_hidden_name_valid("hwp_bench-2.v1"));
  CHECK(app_hidden_name_valid("a.b"));
  CHECK(!app_hidden_name_valid(NULL));
  CHECK(!app_hidden_name_valid(""));
  CHECK(!app_hidden_name_valid("../x"));
  CHECK(!app_hidden_name_valid(".."));
  CHECK(!app_hidden_name_valid("a..b"));
  CHECK(!app_hidden_name_valid("a/b"));
  CHECK(!app_hidden_name_valid("a\\b"));
  CHECK(!app_hidden_name_valid(".x"));
  CHECK(!app_hidden_name_valid(".test"));
  CHECK(!app_hidden_name_valid("a\nb"));
  CHECK(!app_hidden_name_valid("x."));  // FatFS strips a trailing dot

  char n63[64], n64[65];
  memset(n63, 'a', 63);
  n63[63] = '\0';
  memset(n64, 'a', 64);
  n64[64] = '\0';
  CHECK(app_hidden_name_valid(n63));
  CHECK(!app_hidden_name_valid(n64));
}

static void test_real_manifests(void) {
  static const char *k_apps[] = {"hello", "hello_c", "editor", "snake", "store",
                                 "guinea_pig", "panels_demo", "c64", "dos86"};
  for (size_t i = 0; i < sizeof(k_apps) / sizeof(k_apps[0]); i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/apps/%s/app.json", PICODECK_ROOT, k_apps[i]);
    FILE *f = fopen(path, "rb");
    if (!f)
      continue;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    app_entry_t a;
    app_manifest_parse(buf, n, k_apps[i], &a);
    if (!app_manifest_id_valid(a.id))
      printf("  %s: id '%s' invalid\n", path, a.id);
    CHECK(app_manifest_id_valid(a.id));
    CHECK(a.name[0] != '\0');
  }
}

int main(void) {
  test_full_manifest();
  test_defaults();
  test_escapes();
  test_key_inside_value();
  test_requirements();
  test_requirement_list();
  test_id_folding();
  test_ints();
  test_system_clock_table();
  test_truncation_and_bounds();
  test_id_valid();
  test_hidden_name_valid();
  test_real_manifests();
  return check_report("test_app_manifest");
}
