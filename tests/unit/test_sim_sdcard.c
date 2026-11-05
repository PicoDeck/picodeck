// Host unit tests for the simulator's SD-root containment
// (simulator/hal/hal_sdcard.c hal_sdcard_resolve).
//
// The E2E harness used to reach this through game.save("../../x"); since
// game.save validates names at the bridge no unprivileged Lua path produces a
// "..", so the resolver is pinned here instead.
#include "check.h"
#include "hal_sdcard.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char g_base_path[512];

static char s_err[256];

void sim_log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_err, sizeof(s_err), fmt, ap);
  va_end(ap);
}

static bool resolve(const char *path, const char *want) {
  char out[1024];
  s_err[0] = '\0';
  bool ok = hal_sdcard_resolve(path, out, sizeof(out));
  if (want == NULL)
    return !ok;
  if (!ok || strcmp(out, want) != 0) {
    printf("  '%s' -> %s '%s', want '%s'\n", path, ok ? "ok" : "refused",
           ok ? out : "", want);
    return false;
  }
  return true;
}

static void test_resolve(void) {
  strcpy(g_base_path, "/sd");
  CHECK(resolve("/saves/x.json", "/sd/saves/x.json"));
  CHECK(resolve("saves/x.json", "/sd/saves/x.json"));
  CHECK(resolve("/./a//b/", "/sd/a/b"));
  CHECK(resolve("/saves/../saves/inside.json", "/sd/saves/inside.json"));
  CHECK(resolve("/a/b/../../c", "/sd/c"));
  CHECK(resolve("/", "/sd"));

  CHECK(resolve("/saves/../../escape.json", NULL));
  CHECK(strstr(s_err, "[SIM] SD escape: /saves/../../escape.json") != NULL);
  CHECK(resolve("/..", NULL));
  CHECK(resolve("../x", NULL));
  CHECK(resolve("/a/../../x", NULL));
}

// FF_FS_LOCK parity: the 17th concurrently open file is refused, and closing
// one frees its slot. Failed opens (missing file, directory) take no slot.
static void test_open_limit(void) {
  char dir[] = "/tmp/picodeck_sdcard_XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  CHECK(hal_sdcard_init(dir));
  void *h[HAL_SDCARD_MAX_OPEN];
  char name[256];  // holds "<dir>/data/extra.txt": 32 truncated it and leaked dir
  CHECK(hal_sdcard_open("/missing/nope.txt", "r") == NULL);
  CHECK(hal_sdcard_open("/apps", "r") == NULL);
  CHECK(hal_sdcard_open_count() == 0);
  for (int i = 0; i < HAL_SDCARD_MAX_OPEN; i++) {
    snprintf(name, sizeof(name), "/data/f%d.txt", i);
    h[i] = hal_sdcard_open(name, "w");
    CHECK(h[i] != NULL);
  }
  CHECK(hal_sdcard_open_count() == HAL_SDCARD_MAX_OPEN);
  CHECK(hal_sdcard_open("/data/extra.txt", "w") == NULL);
  hal_sdcard_close(h[0]);
  h[0] = hal_sdcard_open("/data/extra.txt", "w");
  CHECK(h[0] != NULL);
  for (int i = 0; i < HAL_SDCARD_MAX_OPEN; i++) {
    hal_sdcard_close(h[i]);
    snprintf(name, sizeof(name), "%s/data/f%d.txt", dir, i);
    remove(name);
  }
  CHECK(hal_sdcard_open_count() == 0);
  snprintf(name, sizeof(name), "%s/data/extra.txt", dir);
  remove(name);
  const char *sub[] = {"apps", "data", "system"};
  for (int i = 0; i < 3; i++) {
    snprintf(name, sizeof(name), "%s/%s", dir, sub[i]);
    rmdir(name);
  }
  rmdir(dir);
}

// Modes mean what the firmware's mode_to_fatfs makes them.
static void test_modes(void) {
  char dir[] = "/tmp/picodeck_sdcard_XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  CHECK(hal_sdcard_init(dir));
  char buf[16];
  const char *p = "/data/modes.txt";

  void *h = hal_sdcard_open(p, "r");
  CHECK(h == NULL);  // "r" needs the file
  h = hal_sdcard_open(p, "r+");
  CHECK(h == NULL);  // so does "r+"

  h = hal_sdcard_open(p, "w+");  // creates, write-only
  CHECK(h != NULL);
  CHECK(hal_sdcard_write_checked(h, "abcdef", 6) == 6);
  CHECK(hal_sdcard_seek(h, 0) == 0);
  CHECK(hal_sdcard_read_checked(h, buf, 2) == -1);
  hal_sdcard_close(h);

  h = hal_sdcard_open(p, "r+");  // read-only, as "r"
  CHECK(h != NULL);
  CHECK(hal_sdcard_write_checked(h, "Q", 1) == -1);
  CHECK(hal_sdcard_read_checked(h, buf, 2) == 2);
  CHECK(memcmp(buf, "ab", 2) == 0);
  hal_sdcard_close(h);

  h = hal_sdcard_open(p, "rw");  // both, no truncation; stdio needs no help
  CHECK(h != NULL);
  CHECK(hal_sdcard_read_checked(h, buf, 2) == 2);
  CHECK(hal_sdcard_write_checked(h, "XY", 2) == 2);
  CHECK(hal_sdcard_read_checked(h, buf, 2) == 2);
  CHECK(memcmp(buf, "ef", 2) == 0);
  hal_sdcard_close(h);

  h = hal_sdcard_open(p, "a");  // starts at the end; a seek moves it, as FatFS
  CHECK(h != NULL);
  CHECK(hal_sdcard_tell(h) == 6);
  CHECK(hal_sdcard_read_checked(h, buf, 1) == -1);
  CHECK(hal_sdcard_write_checked(h, "g", 1) == 1);
  CHECK(hal_sdcard_seek(h, 0) == 0);
  CHECK(hal_sdcard_write_checked(h, "A", 1) == 1);
  hal_sdcard_close(h);

  h = hal_sdcard_open(p, "r");
  CHECK(hal_sdcard_read_checked(h, buf, 7) == 7);
  CHECK(memcmp(buf, "AbXYefg", 7) == 0);
  hal_sdcard_close(h);

  h = hal_sdcard_open(p, "w");  // truncates
  CHECK(h != NULL);
  hal_sdcard_close(h);
  CHECK(hal_sdcard_size(p) == 0);

  h = hal_sdcard_open("/data/new_a.txt", "ab");  // creates
  CHECK(h != NULL);
  hal_sdcard_close(h);
  h = hal_sdcard_open("/data/new_rw.txt", "rw");  // creates
  CHECK(h != NULL);
  hal_sdcard_close(h);
  CHECK(hal_sdcard_isdir("/data"));
  CHECK(!hal_sdcard_isdir(p));

  char name[256];
  const char *files[] = {"modes.txt", "new_a.txt", "new_rw.txt"};
  for (int i = 0; i < 3; i++) {
    snprintf(name, sizeof(name), "%s/data/%s", dir, files[i]);
    remove(name);
  }
  const char *sub[] = {"apps", "data", "system"};
  for (int i = 0; i < 3; i++) {
    snprintf(name, sizeof(name), "%s/%s", dir, sub[i]);
    rmdir(name);
  }
  rmdir(dir);
}

int main(void) {
  test_resolve();
  test_open_limit();
  test_modes();
  return check_report("test_sim_sdcard");
}
