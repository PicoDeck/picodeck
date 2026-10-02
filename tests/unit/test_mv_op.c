// Host tests for the dev `mv`: path canonicalisation (mv_path.c, pure) and
// the whole command (mv_op.c) over the vendored FatFS (ff.c) on a RAM disk,
// where the dangerous spellings are real: a rename of a directory below
// itself is not checked by f_rename and orphans the subtree, and /system
// must stay put whatever it is called.
#include "check.h"
#include "mv_op.h"
#include "mv_path.h"
#include "app_identity.h"
#include "drivers/fat_within.h"
#include "drivers/sdcard.h"
#include "ff.h"
#include "diskio.h"

#include <stdlib.h>
#include <string.h>

// ── RAM disk ────────────────────────────────────────────────────────────────

#define NSECT 16384  // 8 MB
static uint8_t *s_disk;

DSTATUS disk_initialize(BYTE p) { (void)p; return 0; }
DSTATUS disk_status(BYTE p) { (void)p; return 0; }
DRESULT disk_read(BYTE p, BYTE *b, LBA_t s, UINT c) {
  (void)p; memcpy(b, s_disk + (size_t)s * 512, (size_t)c * 512); return RES_OK;
}
DRESULT disk_write(BYTE p, const BYTE *b, LBA_t s, UINT c) {
  (void)p; memcpy(s_disk + (size_t)s * 512, b, (size_t)c * 512); return RES_OK;
}
DRESULT disk_ioctl(BYTE p, BYTE cmd, void *buff) {
  (void)p;
  if (cmd == GET_SECTOR_COUNT) { *(LBA_t *)buff = NSECT; return RES_OK; }
  if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buff = 1; return RES_OK; }
  if (cmd == CTRL_SYNC) return RES_OK;
  return RES_PARERR;
}
void *ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }

// ── The sdcard_* calls mv_op.c makes, over FatFS as sdcard.c does ───────────

bool sdcard_stat(const char *path, sdcard_stat_t *out) {
  FILINFO fi;
  if (f_stat(path, &fi) != FR_OK) return false;
  out->is_dir = (fi.fattrib & AM_DIR) != 0;
  return true;
}
bool sdcard_mkdir(const char *path) {
  FRESULT r = f_mkdir(path);
  return r == FR_OK || r == FR_EXIST;
}
bool sdcard_rename(const char *a, const char *b) { return f_rename(a, b) == FR_OK; }
bool sdcard_delete(const char *path) { return f_unlink(path) == FR_OK; }
sdcard_within_t sdcard_path_within(const char *root, const char *path) {
  return (sdcard_within_t)fat_path_within(root, path);
}
static app_identity_t s_me;
static bool s_app_running;
const app_identity_t *app_identity_current(void) { return s_app_running ? &s_me : NULL; }

static FATFS s_fs;

static void reset(BYTE fmt) {
  BYTE work[4096];
  MKFS_PARM opt = {fmt, 0, 0, 0, 0};
  memset(s_disk, 0, (size_t)NSECT * 512);
  CHECK_EQ_INT(f_mkfs("", &opt, work, sizeof work), FR_OK);
  CHECK_EQ_INT(f_mount(&s_fs, "", 1), FR_OK);
  f_mkdir("/system"); f_mkdir("/system/lib"); f_mkdir("/apps");
  f_mkdir("/apps/foo"); f_mkdir("/apps/longappname"); f_mkdir("/data");
  FIL f;
  f_open(&f, "/system/config.json", FA_WRITE | FA_CREATE_ALWAYS); f_close(&f);
  f_open(&f, "/apps/foo/main.lua", FA_WRITE | FA_CREATE_ALWAYS); f_close(&f);
  s_app_running = false;
}

static bool exists(const char *p) { FILINFO fi; return f_stat(p, &fi) == FR_OK; }

static bool mv(const char *cmd, char *reply, size_t n) {
  char args[256];
  snprintf(args, sizeof args, "%s", cmd);
  return mv_op(args, reply, n);
}

static void intact(void) {
  CHECK(exists("/system"));
  CHECK(exists("/system/config.json"));
  CHECK(exists("/system/lib"));
  CHECK(exists("/apps/foo/main.lua"));
  CHECK(exists("/apps/longappname"));
}

// ── mv_path ─────────────────────────────────────────────────────────────────

static void test_canon(void) {
  char o[64];
  CHECK(mv_path_canon("/a/b", o, sizeof o)); CHECK_STR(o, "/a/b");
  CHECK(mv_path_canon("//a///b/", o, sizeof o)); CHECK_STR(o, "/a/b");
  CHECK(mv_path_canon("/\\a\\b", o, sizeof o)); CHECK_STR(o, "/a/b");
  CHECK(mv_path_canon("/system", o, sizeof o)); CHECK_STR(o, "/system");
  const char *bad[] = {"", "a/b", "/", "//", "/./a", "/a/./b", "/../a", "/a/..",
                       "/a/../b", "/a.", "/a./b", "/a /b", "/a/b ", "/system.",
                       "/a:b", "/a*", "/a\tb", "/a|b", "/a?"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    CHECK(!mv_path_canon(bad[i], o, sizeof o));
  CHECK(!mv_path_canon(NULL, o, sizeof o));
  CHECK(!mv_path_canon("/abcdefgh", o, 5));        // too long
  CHECK(mv_path_canon("/abc", o, 5));              // just fits
  CHECK(mv_path_canon("/.hidden/x", o, sizeof o)); // a leading dot is a name
  CHECK(mv_path_canon("/a..b", o, sizeof o));

  CHECK(mv_path_is_root_dir("/")); CHECK(mv_path_is_root_dir("/APPS"));
  CHECK(mv_path_is_root_dir("/data")); CHECK(mv_path_is_root_dir("/System"));
  CHECK(!mv_path_is_root_dir("/apps/foo")); CHECK(!mv_path_is_root_dir("/dat"));
}

// ── mv_op on FatFS ──────────────────────────────────────────────────────────

static void test_system_is_off_limits(BYTE fmt) {
  const char *cases[] = {
      "/system /data/sys", "//system /data/sys", "/./system /data/sys",
      "/system. /data/sys", "/\\system /data/sys", "/SYSTEM /data/sys",
      "/system./config.json /data/c.json", "/system/config.json /data/c.json",
      "/data /system./data", "/data //system/data", "/apps/foo /system/foo",
      "/apps/foo /system/lib/foo", "/apps/foo /SYSTEM/x/y/z",
      "/system/lib /apps/lib"};
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    reset(fmt);
    char reply[200];
    bool ok = mv(cases[i], reply, sizeof reply);
    if (ok) printf("  moved: %s\n", cases[i]);
    CHECK(!ok);
    intact();
    CHECK(!exists("/data/sys") && !exists("/data/c.json") && !exists("/system/foo"));
    CHECK(!exists("/system/data"));
  }
  // The identity test, not the spelling, decides: an 8.3 alias of a long name.
  reset(fmt);
  f_mkdir("/system/longsubdirname");
  CHECK_EQ_INT(fat_path_within("/system", "/system/LONGSU~1/x"), FAT_WITHIN_YES);
  CHECK_EQ_INT(fat_path_within("/apps", "/system/lib"), FAT_WITHIN_NO);
  CHECK_EQ_INT(fat_path_within("/nope", "/system/lib"), FAT_WITHIN_NO);
  CHECK_EQ_INT(fat_path_within("/system", "/nope/x"), FAT_WITHIN_NO);
}

static void test_top_level_roots_stay(BYTE fmt) {
  const char *cases[] = {"/apps /x", "/data /y", "/apps/foo /apps", "/apps/foo /data",
                         "/APPS /x", "//apps /z"};
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    reset(fmt);
    char reply[200];
    CHECK(!mv(cases[i], reply, sizeof reply));
    CHECK(exists("/apps/foo/main.lua") && exists("/data"));
  }
}

static void test_into_itself(BYTE fmt) {
  const char *cases[] = {
      "/apps/foo /apps/foo/bar", "/apps/foo /apps//foo/bar",
      "/apps/foo /apps/./foo/bar", "/apps/foo /apps/foo./bar",
      "/apps/foo /apps/FOO/bar", "/apps/foo /apps\\foo\\bar",
      "/apps/foo /apps/foo/a/b/c", "/apps /apps/foo/x",
      "/apps/longappname /apps/LONGAP~1/x"};
  // exFAT has no 8.3 aliases: the last spelling is then a plain new name.
  size_t ncases = sizeof cases / sizeof *cases - (fmt == FM_EXFAT ? 1 : 0);
  for (size_t i = 0; i < ncases; i++) {
    reset(fmt);
    char reply[200];
    bool ok = mv(cases[i], reply, sizeof reply);
    if (ok) printf("  moved: %s\n", cases[i]);
    CHECK(!ok);
    intact();
    CHECK(exists("/apps/foo"));
    CHECK(!exists("/apps/foo/bar") && !exists("/apps/foo/a"));
  }
}

static void test_normal_moves(BYTE fmt) {
  char reply[200];
  reset(fmt);
  CHECK(mv("/apps/foo /apps/.dev/foo", reply, sizeof reply));
  CHECK_STR(reply, "Moved: /apps/foo -> /apps/.dev/foo");
  CHECK(!exists("/apps/foo") && exists("/apps/.dev/foo/main.lua"));
  // Back, with sloppy spelling on both sides, and a file.
  CHECK(mv("//apps//.dev/foo/ /apps/bar", reply, sizeof reply));
  CHECK(exists("/apps/bar/main.lua") && !exists("/apps/.dev/foo"));
  CHECK(mv("/apps/bar/main.lua /data/deep/er/m.lua", reply, sizeof reply));
  CHECK(exists("/data/deep/er/m.lua"));
  // Siblings that merely share a prefix are not "inside".
  CHECK(mv("/apps/longappname /apps/longappname2", reply, sizeof reply));
  CHECK(exists("/apps/longappname2"));
  // Not a refusal: a directory beside another one it shares a name start with.
  CHECK(mv("/apps/bar /apps/bar2", reply, sizeof reply));
}

static void test_refusals_and_edges(BYTE fmt) {
  char reply[200];
  reset(fmt);
  CHECK(!mv("/apps/foo /apps/longappname", reply, sizeof reply));  // exists
  CHECK(strstr(reply, "destination exists"));
  CHECK(!mv("/apps/foo /apps/LONGAPPNAME", reply, sizeof reply));  // case-only-ish
  CHECK(!mv("/apps/foo /apps/FOO", reply, sizeof reply));          // case only
  CHECK(strstr(reply, "case-only"));
  CHECK(exists("/apps/foo/main.lua"));
  CHECK(!mv("/apps/foo /apps/foo", reply, sizeof reply));
  CHECK(!mv("/apps/missing /apps/x", reply, sizeof reply));
  CHECK(strstr(reply, "no such"));
  CHECK(!mv("/apps/foo", reply, sizeof reply));
  CHECK(strstr(reply, "Usage"));
  CHECK(!mv("apps/foo /apps/x", reply, sizeof reply));
  CHECK(!mv("/apps/foo /apps/a b", reply, sizeof reply));
  CHECK(!mv("/apps/foo /apps/../x", reply, sizeof reply));
  // A target whose parent is a file: the rename fails, and the directories
  // this call created on the way are gone again.
  FIL f;
  f_open(&f, "/data/afile", FA_WRITE | FA_CREATE_ALWAYS); f_close(&f);
  CHECK(!mv("/apps/foo /data/afile/x", reply, sizeof reply));
  CHECK(exists("/apps/foo/main.lua"));
  // The running app's own directory (or one above) stays put.
  s_app_running = true;
  snprintf(s_me.dir, sizeof s_me.dir, "/apps/foo");
  CHECK(!mv("/apps/foo /apps/.dev/foo", reply, sizeof reply));
  CHECK(strstr(reply, "running app"));
  CHECK(!mv("/apps /x", reply, sizeof reply));
  CHECK(mv("/apps/longappname /apps/other", reply, sizeof reply));
  s_app_running = false;
  CHECK(mv("/apps/foo /apps/.dev/foo", reply, sizeof reply));
}

static void test_parent_creation(BYTE fmt) {
  char reply[200];
  reset(fmt);
  // Parent creation that fails on the way (a file in the way) leaves nothing
  // new behind, and a good move creates the whole chain.
  FIL f;
  f_open(&f, "/data/blocker", FA_WRITE | FA_CREATE_ALWAYS); f_close(&f);
  CHECK(!mv("/apps/foo /data/blocker/y/z", reply, sizeof reply));
  CHECK(!exists("/data/blocker/y"));
  CHECK(exists("/apps/foo/main.lua"));
  CHECK(mv("/apps/foo /data/new/dir/foo", reply, sizeof reply));
  CHECK(exists("/data/new/dir/foo/main.lua"));
}

// FatFS opens a directory through the same 16-entry table as files, so with
// it full f_opendir fails for a directory that exists, while f_rename works.
// The identity test must say "unknown", and mv must refuse, not move.
#define OPEN_FILES 16
static FIL s_open[OPEN_FILES];

static void fill_open_table(void) {
  for (int i = 0; i < OPEN_FILES; i++) {
    char p[32];
    snprintf(p, sizeof p, "/data/open%d", i);
    CHECK_EQ_INT(f_open(&s_open[i], p, FA_WRITE | FA_CREATE_ALWAYS), FR_OK);
  }
}
static void close_open_table(void) {
  for (int i = 0; i < OPEN_FILES; i++) f_close(&s_open[i]);
}

static void test_full_open_table_fails_closed(BYTE fmt) {
  const char *cases[] = {
      "/apps/foo /apps/foo/bar", "/apps/foo /apps//foo/bar",
      "/apps/foo /apps/FOO/bar", "/apps/foo /apps/foo/a/b",
      "/apps/longappname /apps/LONGAP~1/x", "/apps/foo /apps/.dev/foo",
      "/system/lib /apps/lib", "/apps/foo /system/foo", "/apps/foo /SYSTEM/x"};
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    reset(fmt);
    fill_open_table();
    CHECK_EQ_INT(fat_path_within("/apps/foo", "/apps/foo/bar"), FAT_WITHIN_UNKNOWN);
    char reply[200];
    bool ok = mv(cases[i], reply, sizeof reply);
    if (ok) printf("  moved with a full table: %s\n", cases[i]);
    CHECK(!ok);
    close_open_table();
    intact();
    CHECK(!exists("/apps/.dev/foo") && !exists("/system/foo"));
  }
  // The refusal says why, and the move works once files are closed.
  reset(fmt);
  fill_open_table();
  char reply[200];
  CHECK(!mv("/apps/foo /apps/foo/bar", reply, sizeof reply));
  CHECK(strstr(reply, "cannot verify"));
  close_open_table();
  CHECK(mv("/apps/foo /apps/.dev/foo", reply, sizeof reply));
}

int main(void) {
  s_disk = malloc((size_t)NSECT * 512);
  test_canon();
  BYTE fmts[] = {FM_ANY, FM_EXFAT};
  for (size_t i = 0; i < sizeof fmts; i++) {
    test_system_is_off_limits(fmts[i]);
    test_top_level_roots_stay(fmts[i]);
    test_into_itself(fmts[i]);
    test_normal_moves(fmts[i]);
    test_refusals_and_edges(fmts[i]);
    test_parent_creation(fmts[i]);
    test_full_open_table_fails_closed(fmts[i]);
  }
  free(s_disk);
  return check_report("test_mv_op");
}
