#include "check.h"
#include "module_path.h"
#include <string.h>

static bool mp(const char *name, char *out, size_t n) {
  return module_path(name, strlen(name), "/apps/game", out, n);
}

static void test_valid_names(void) {
  char out[128];
  CHECK(mp("track", out, sizeof out));
  CHECK_STR(out, "/apps/game/track.lua");
  CHECK(mp("src.track_gen", out, sizeof out));
  CHECK_STR(out, "/apps/game/src/track_gen.lua");
  CHECK(mp("a.b.c-d", out, sizeof out));
  CHECK_STR(out, "/apps/game/a/b/c-d.lua");
}

static void test_rejected_names(void) {
  char out[128];
  const char *bad[] = {"", ".", "..", "a..b", ".a", "a.", "a/b", "../x", "a\\b",
                       "a b", "a.b.", "x.~", NULL};
  for (int i = 0; bad[i]; i++) CHECK(!mp(bad[i], out, sizeof out));
  char longname[200];
  memset(longname, 'a', sizeof longname - 1);
  longname[sizeof longname - 1] = 0;
  CHECK(!mp(longname, out, sizeof out));             // > 128 bytes
  CHECK(!module_path("a\0b", 3, "/apps/game", out, sizeof out));  // embedded NUL
}

static void test_does_not_overflow(void) {
  char out17[17], out18[18];
  // "/apps/game/ab.lua" is 17 characters + NUL = 18 bytes.
  CHECK(!mp("ab", out17, sizeof out17));
  CHECK(mp("ab", out18, sizeof out18));
  CHECK_STR(out18, "/apps/game/ab.lua");
}

int main(void) {
  test_valid_names();
  test_rejected_names();
  test_does_not_overflow();
  return check_report("test_module_path");
}
