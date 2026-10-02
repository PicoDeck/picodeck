// Host unit tests for src/os/zip_archive.c: the g_api.zip read-in-place
// entry points, over the real zip_util/miniz engine and the in-memory SD
// fake.  Covers the zip->read return contract: bytes written, -1 for an
// error, PCZIP_ERR_TOO_SMALL (a distinct negative) for an undersized buffer.
#include "check.h"
#include "fakes/sdcard_fake.h"
#include "sdcard.h"
#include "zip_archive.h"
#include "zip_util.h"

#include <stdlib.h>
#include <string.h>

// zip_util.c also calls these; the tests never reach them.
bool sdcard_stat(const char *path, sdcard_stat_t *out) {
  (void)path; (void)out;
  return false;
}
bool sdcard_disk_info(uint32_t *out_free_kb, uint32_t *out_total_kb) {
  (void)out_free_kb; (void)out_total_kb;
  return false;
}

// hello.txt ("hello world", 11 bytes) and empty.txt, both stored.
static const unsigned char k_zip[] = {0x50,0x4b,0x3,0x4,0x14,0x0,0x0,0x0,0x0,0x0,0x4d,0x71,0x3d,0x5d,0x85,0x11,0x4a,0xd,0xb,0x0,0x0,0x0,0xb,0x0,0x0,0x0,0x9,0x0,0x0,0x0,0x68,0x65,0x6c,0x6c,0x6f,0x2e,0x74,0x78,0x74,0x68,0x65,0x6c,0x6c,0x6f,0x20,0x77,0x6f,0x72,0x6c,0x64,0x50,0x4b,0x3,0x4,0x14,0x0,0x0,0x0,0x0,0x0,0x4d,0x71,0x3d,0x5d,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x9,0x0,0x0,0x0,0x65,0x6d,0x70,0x74,0x79,0x2e,0x74,0x78,0x74,0x50,0x4b,0x1,0x2,0x14,0x3,0x14,0x0,0x0,0x0,0x0,0x0,0x4d,0x71,0x3d,0x5d,0x85,0x11,0x4a,0xd,0xb,0x0,0x0,0x0,0xb,0x0,0x0,0x0,0x9,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x80,0x1,0x0,0x0,0x0,0x0,0x68,0x65,0x6c,0x6c,0x6f,0x2e,0x74,0x78,0x74,0x50,0x4b,0x1,0x2,0x14,0x3,0x14,0x0,0x0,0x0,0x0,0x0,0x4d,0x71,0x3d,0x5d,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x9,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x80,0x1,0x32,0x0,0x0,0x0,0x65,0x6d,0x70,0x74,0x79,0x2e,0x74,0x78,0x74,0x50,0x4b,0x5,0x6,0x0,0x0,0x0,0x0,0x2,0x0,0x2,0x0,0x6e,0x0,0x0,0x0,0x59,0x0,0x0,0x0,0x0,0x0};

static void test_read(void) {
  sdfake_reset();
  sdfake_put("/t.zip", (const char *)k_zip, sizeof(k_zip));
  pczip_t z = zip_archive_open("/t.zip");
  CHECK(z != NULL);
  if (!z) return;

  int idx = zip_archive_locate(z, "hello.txt");
  CHECK(idx >= 0);
  pczip_stat_t st;
  CHECK(zip_archive_stat_index(z, idx, &st));
  CHECK_EQ_U32(st.size, 11);

  char buf[32];
  // Exact fit and roomy buffers: bytes written.
  memset(buf, 0, sizeof(buf));
  CHECK_EQ_INT(zip_archive_read(z, idx, buf, 11), 11);
  CHECK(memcmp(buf, "hello world", 11) == 0);
  CHECK_EQ_INT(zip_archive_read(z, idx, buf, sizeof(buf)), 11);

  // Undersized buffer: the distinct code, negative (so an "n < 0" caller
  // stays safe), not -1, and nothing written past buf_cap.
  memset(buf, 0x55, sizeof(buf));
  int n = zip_archive_read(z, idx, buf, 10);
  CHECK_EQ_INT(n, PCZIP_ERR_TOO_SMALL);
  CHECK(n < 0 && n != -1);
  CHECK((unsigned char)buf[0] == 0x55);

  // Zero capacity: too small for a non-empty entry, buffer untouched.
  memset(buf, 0x55, sizeof(buf));
  CHECK_EQ_INT(zip_archive_read(z, idx, buf, 0), PCZIP_ERR_TOO_SMALL);
  CHECK((unsigned char)buf[0] == 0x55);
  // An empty entry reads as 0 bytes, with any capacity including 0.
  int eidx = zip_archive_locate(z, "empty.txt");
  CHECK(eidx >= 0);
  CHECK_EQ_INT(zip_archive_read(z, eidx, buf, sizeof(buf)), 0);
  CHECK_EQ_INT(zip_archive_read(z, eidx, buf, 0), 0);

  // Genuine errors stay -1: bad index, NULL buffer, bad handle.
  CHECK_EQ_INT(zip_archive_read(z, 99, buf, sizeof(buf)), -1);
  CHECK_EQ_INT(zip_archive_read(z, -1, buf, sizeof(buf)), -1);
  CHECK_EQ_INT(zip_archive_read(z, idx, NULL, sizeof(buf)), -1);
  CHECK_EQ_INT(zip_archive_read(NULL, idx, buf, sizeof(buf)), -1);

  zip_archive_close(z);
  CHECK_EQ_INT(zip_archive_read(z, idx, buf, sizeof(buf)), -1);  // closed
}

// A bad entry index must be refused BEFORE the destination is opened: opening
// it "w" first destroyed an existing file at dest.
static void test_extract_entry_bad_index_keeps_dest(void) {
  sdfake_reset();
  sdfake_put("/t.zip", (const char *)k_zip, sizeof(k_zip));
  sdfake_put("/keep.txt", "precious", 8);
  zip_reader_t zr;
  char err[ZIP_ERR_MAX];
  CHECK(zip_reader_open(&zr, "/t.zip", err));
  int n = zip_reader_num_entries(&zr);
  CHECK_EQ_INT(n, 2);
  int bad[] = {n, n + 1, 999, -1};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    err[0] = 0;
    CHECK(!zip_reader_extract_entry(&zr, bad[i], "/keep.txt", err));
    CHECK(strstr(err, "no such entry") != NULL);
    size_t len = 0;
    const char *d = sdfake_get("/keep.txt", &len);
    CHECK(d && len == 8 && memcmp(d, "precious", 8) == 0);
    zip_entry_info_t info;
    CHECK(!zip_reader_stat_index(&zr, bad[i], &info));
  }
  CHECK_EQ_INT(zip_reader_locate(&zr, "absent.txt"), -1);
  // A valid index still extracts (over the existing file).
  CHECK(zip_reader_extract_entry(&zr, zip_reader_locate(&zr, "hello.txt"),
                                 "/keep.txt", err));
  size_t len = 0;
  const char *d = sdfake_get("/keep.txt", &len);
  CHECK(d && len == 11 && memcmp(d, "hello world", 11) == 0);
  zip_reader_close(&zr);
}

int main(void) {
  test_read();
  test_extract_entry_bad_index_keeps_dest();
  return check_report("test_zip_archive");
}
