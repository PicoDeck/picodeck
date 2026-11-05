// Host tests for the firmware's SD driver (src/drivers/sdcard.c) over the
// vendored FatFS on a RAM disk: what the simulator's own SD layer cannot show.
// A long transfer feeds the device's 10 s watchdog as it goes (a Lua app has no
// other feeder during a C call), a full card is a short count, not an error, and
// a progress callback can stop a copy.
#include "check.h"
#include "drivers/sdcard.h"
#include "ff.h"
#include "diskio.h"
#include "hardware.h"
#include "umm_malloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_watchdog_feeds;
static int s_heartbeats;
void core0_heartbeat(void) { s_heartbeats++; }

// What the pico/time.h stub calls.
void hal_sleep_ms(uint32_t ms) { (void)ms; }
uint64_t hal_get_time_us(void) { return 0; }

// ── RAM disk ────────────────────────────────────────────────────────────────

#define NSECT 16384  // 8 MB
static uint8_t *s_disk;
static bool s_read_error;
static int s_disk_reads;  // read commands, the cost of a path walk on the card

DSTATUS disk_initialize(BYTE p) { (void)p; return 0; }
DSTATUS disk_status(BYTE p) { (void)p; return 0; }
DRESULT disk_read(BYTE p, BYTE *b, LBA_t s, UINT c) {
  (void)p;
  if (s_read_error) return RES_ERROR;
  s_disk_reads++;
  memcpy(b, s_disk + (size_t)s * 512, (size_t)c * 512);
  return RES_OK;
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
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (1 << 21) | (1 << 16); }

#define BIG (1024 * 1024)
static uint8_t *s_big;

static void reset(void) {
  BYTE work[4096];
  MKFS_PARM opt = {FM_FAT, 0, 0, 0, 0};
  f_unmount("");
  memset(s_disk, 0, (size_t)NSECT * 512);
  CHECK_EQ_INT(f_mkfs("", &opt, work, sizeof work), FR_OK);
  CHECK(sdcard_init());
  g_watchdog_feeds = 0;
  s_read_error = false;
}

// ── Long transfers ──────────────────────────────────────────────────────────

static void test_big_write_and_read_feed(void) {
  reset();
  sdfile_t f = sdcard_fopen("/data/big.bin", "w");
  CHECK(f != NULL);
  CHECK_EQ_INT(sdcard_fwrite(f, s_big, BIG), BIG);
  CHECK(g_watchdog_feeds >= 8);  // not once per MB: it would outlast 10 s at 1 MHz
  sdcard_fclose(f);

  g_watchdog_feeds = 0;
  uint8_t *back = malloc(BIG);
  f = sdcard_fopen("/data/big.bin", "r");
  CHECK(f != NULL);
  CHECK_EQ_INT(sdcard_fread(f, back, BIG), BIG);
  CHECK(g_watchdog_feeds >= 8);
  CHECK(memcmp(back, s_big, BIG) == 0);

  // Reads stop at the end of the file, with the count.
  CHECK_EQ_INT(sdcard_fread(f, back, 16), 0);
  CHECK(sdcard_fseek(f, BIG - 10));
  CHECK_EQ_INT(sdcard_fread(f, back, BIG), 10);
  CHECK(sdcard_fseek(f, 3));
  CHECK_EQ_INT(sdcard_fread(f, back, 5), 5);
  CHECK(memcmp(back, s_big + 3, 5) == 0);
  CHECK_EQ_INT(sdcard_fread(f, back, 0), 0);
  CHECK_EQ_INT(sdcard_fread(f, back, -1), -1);
  sdcard_fclose(f);
  free(back);
}

static void test_small_transfers_do_not_feed(void) {
  reset();
  sdfile_t f = sdcard_fopen("/data/small.bin", "w");
  CHECK_EQ_INT(sdcard_fwrite(f, s_big, 4096), 4096);
  sdcard_fclose(f);
  f = sdcard_fopen("/data/small.bin", "r");
  uint8_t buf[4096];
  CHECK_EQ_INT(sdcard_fread(f, buf, 4096), 4096);
  sdcard_fclose(f);
  CHECK_EQ_INT(g_watchdog_feeds, 0);  // an app's own loop decides there
}

// A path walk is one read per directory sector up to the name, ~10 on a card of
// 40 apps, and an app loads each of its modules through sdcard_read_file: it may
// walk the path only once, as a plain open and read does.
static void test_read_file_walks_the_path_once(void) {
  reset();
  CHECK(sdcard_mkdir("/apps/game"));
  for (int i = 0; i < 40; i++) {  // puts the file many directory sectors in
    char name[64];
    snprintf(name, sizeof name, "/apps/game/a_module_of_many_%02d.lua", i);
    sdfile_t m = sdcard_fopen(name, "w");
    CHECK_EQ_INT(sdcard_fwrite(m, "x", 1), 1);
    sdcard_fclose(m);
  }
  sdfile_t f = sdcard_fopen("/apps/game/z.lua", "w");
  CHECK_EQ_INT(sdcard_fwrite(f, s_big, 3000), 3000);
  sdcard_fclose(f);

  // The same start for both: FatFS's sector window holds something else.
  CHECK(sdcard_fexists("/system"));
  s_disk_reads = 0;
  FIL fil;
  UINT got = 0;
  uint8_t buf[3000];
  CHECK_EQ_INT(f_open(&fil, "/apps/game/z.lua", FA_READ), FR_OK);
  CHECK_EQ_INT(f_read(&fil, buf, sizeof buf, &got), FR_OK);
  f_close(&fil);
  const int plain = s_disk_reads;
  CHECK(plain >= 5);  // a real walk, or this shows nothing

  CHECK(sdcard_fexists("/system"));
  s_disk_reads = 0;
  int n = 0;
  char *src = sdcard_read_file("/apps/game/z.lua", &n);
  CHECK_EQ_INT(s_disk_reads, plain);
  CHECK(src != NULL);
  CHECK_EQ_INT(n, 3000);
  CHECK(memcmp(src, s_big, 3000) == 0);
  CHECK_EQ_INT(src[3000], 0);
  umm_free(src);

  // What callers rely on: NULL if it cannot be opened, a terminated empty
  // buffer for an empty file, and out_len optional.
  CHECK(sdcard_read_file("/apps/game/none.lua", &n) == NULL);
  CHECK(sdcard_read_file("/apps/game", &n) == NULL);
  sdcard_fclose(sdcard_fopen("/apps/game/empty.lua", "w"));
  n = -1;
  src = sdcard_read_file("/apps/game/empty.lua", &n);
  CHECK(src != NULL);
  CHECK_EQ_INT(n, 0);
  CHECK_EQ_INT(src[0], 0);
  umm_free(src);
  src = sdcard_read_file("/apps/game/z.lua", NULL);
  CHECK(src != NULL);
  umm_free(src);
}

static void test_read_error_is_minus_one_after_a_partial_read(void) {
  reset();
  sdfile_t f = sdcard_fopen("/data/big.bin", "w");
  CHECK_EQ_INT(sdcard_fwrite(f, s_big, 200000), 200000);
  sdcard_fclose(f);
  f = sdcard_fopen("/data/big.bin", "r");
  uint8_t *back = malloc(200000);
  CHECK_EQ_INT(sdcard_fread(f, back, 70000), 70000);
  s_read_error = true;
  CHECK_EQ_INT(sdcard_fread(f, back, 70000), -1);
  s_read_error = false;
  sdcard_fclose(f);
  free(back);
}

static void test_full_card_is_a_short_count(void) {
  reset();
  sdfile_t f = sdcard_fopen("/data/full.bin", "w");
  CHECK(f != NULL);
  int total = 0, n = 0;
  // 8 MB card: write until it stops taking bytes.
  for (int i = 0; i < 12 && (n = sdcard_fwrite(f, s_big, BIG)) == BIG; i++)
    total += n;
  CHECK(n >= 0);   // FatFS says OK...
  CHECK(n < BIG);  // ...with fewer bytes: callers must compare the count
  sdcard_fclose(f);
  CHECK_EQ_INT(sdcard_fsize("/data/full.bin"), total + n);
}

// ── Copy ────────────────────────────────────────────────────────────────────

static int s_progress_calls, s_stop_after;
static uint32_t s_done, s_total;
static bool on_progress(uint32_t done, uint32_t total, void *user) {
  (void)user;
  s_progress_calls++;
  s_done = done;
  s_total = total;
  return s_stop_after == 0 || s_progress_calls < s_stop_after;
}

static void test_copy_feeds_and_can_stop(void) {
  reset();
  sdfile_t f = sdcard_fopen("/data/src.bin", "w");
  CHECK_EQ_INT(sdcard_fwrite(f, s_big, 100000), 100000);
  sdcard_fclose(f);

  // No progress callback: the driver feeds by itself.
  g_watchdog_feeds = 0;
  CHECK(sdcard_copy("/data/src.bin", "/data/dst.bin", NULL, NULL));
  CHECK(g_watchdog_feeds >= 20);  // once per 4 KB chunk
  CHECK_EQ_INT(sdcard_fsize("/data/dst.bin"), 100000);

  s_progress_calls = 0;
  s_stop_after = 0;
  CHECK(sdcard_copy("/data/src.bin", "/data/dst2.bin", on_progress, NULL));
  CHECK(s_progress_calls >= 20);
  CHECK_EQ_U32(s_done, 100000);
  CHECK_EQ_U32(s_total, 100000);

  // A false stops the copy and removes the partial destination.
  s_progress_calls = 0;
  s_stop_after = 3;
  CHECK(!sdcard_copy("/data/src.bin", "/data/dst3.bin", on_progress, NULL));
  CHECK_EQ_INT(s_progress_calls, 3);
  CHECK(!sdcard_fexists("/data/dst3.bin"));
  s_stop_after = 0;
}

// ── Long directory work ─────────────────────────────────────────────────────

static void count_entry(const sdcard_entry_t *e, void *user) {
  (void)e;
  (*(int *)user)++;
}

static void test_directory_walks_feed(void) {
  reset();
  CHECK(sdcard_mkdir("/data/tree"));
  char name[64];
  for (int i = 0; i < 40; i++) {
    snprintf(name, sizeof name, "/data/tree/f%d", i);
    sdfile_t f = sdcard_fopen(name, "w");
    CHECK(f != NULL);
    sdcard_fclose(f);
  }
  g_watchdog_feeds = 0;
  int n = 0;
  CHECK_EQ_INT(sdcard_list_dir("/data/tree", count_entry, &n), 40);
  CHECK_EQ_INT(n, 40);
  CHECK(g_watchdog_feeds >= 40);

  g_watchdog_feeds = 0;
  CHECK(sdcard_delete_recursive("/data/tree"));
  CHECK(g_watchdog_feeds >= 40);
  CHECK(!sdcard_fexists("/data/tree"));
  // The volume root stays refused by the driver too.
  CHECK(!sdcard_delete_recursive("/"));
  CHECK(sdcard_fexists("/data"));
}

// A whole-FAT scan cannot be interrupted: Core 1's relay has to cover it.
static void test_disk_info_stamps_the_heartbeat(void) {
  reset();
  s_heartbeats = 0;
  uint32_t free_kb = 0, total_kb = 0;
  CHECK(sdcard_disk_info(&free_kb, &total_kb));
  CHECK(s_heartbeats > 0);
  CHECK(total_kb > 0 && free_kb <= total_kb);
}

// ── The SD clock ────────────────────────────────────────────────────────────

// spi_set_baudrate clears SSE for a moment, which can cost a read in flight the
// byte in the shifter; spi_write_read_blocking then waits for it for good. Core
// 1 reads with the SD mutex held, right through to the end of an image_load
// nobody wants any more (image_preload_update), so the write has to wait.

unsigned g_fake_spi_baud;
static int s_baud_writes;
static int s_baud_writes_during_read;
static atomic_bool s_core1_reading;

void fake_spi_baud_written(unsigned baud) {
  g_fake_spi_baud = baud;
  s_baud_writes++;
  if (atomic_load(&s_core1_reading))
    s_baud_writes_during_read++;
}

static atomic_bool s_core1_started;

// Core 1, in the middle of a read of the card.
static void *core1_read(void *arg) {
  (void)arg;
  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  atomic_store(&s_core1_reading, true);
  atomic_store(&s_core1_started, true);
  usleep(50 * 1000);
  atomic_store(&s_core1_reading, false);
  recursive_mutex_exit(&g_sdcard_mutex);
  return NULL;
}

static void clock_reset(unsigned baud) {
  reset();
  g_fake_spi_baud = baud;
  s_baud_writes = 0;
  s_baud_writes_during_read = 0;
  atomic_store(&s_core1_started, false);
}

static void set_slow_during_a_read(bool slow) {
  pthread_t t;
  CHECK_EQ_INT(pthread_create(&t, NULL, core1_read, NULL), 0);
  while (!atomic_load(&s_core1_started))
    usleep(100);
  sd_set_slow_mode(slow);
  pthread_join(t, NULL);
}

static void test_slow_mode_waits_for_a_read_in_flight(void) {
  clock_reset(SD_SPI_BAUD);
  set_slow_during_a_read(true);
  CHECK_EQ_INT(s_baud_writes, 1);
  CHECK_EQ_INT(s_baud_writes_during_read, 0);
  CHECK(g_fake_spi_baud == 1000 * 1000);

  s_baud_writes = 0;
  s_baud_writes_during_read = 0;
  atomic_store(&s_core1_started, false);
  set_slow_during_a_read(false);
  CHECK_EQ_INT(s_baud_writes, 1);
  CHECK_EQ_INT(s_baud_writes_during_read, 0);
  CHECK(g_fake_spi_baud == SD_SPI_BAUD);
}

// The launcher asks at every app's exit, and nearly always it is off already.
static void test_slow_mode_already_as_asked_leaves_the_block_alone(void) {
  clock_reset(SD_SPI_BAUD);
  sd_set_slow_mode(false);
  CHECK_EQ_INT(s_baud_writes, 0);

  // Not even while a read is in flight, and without waiting for it.
  set_slow_during_a_read(false);
  CHECK_EQ_INT(s_baud_writes, 0);

  clock_reset(1000 * 1000);
  sd_set_slow_mode(true);
  CHECK_EQ_INT(s_baud_writes, 0);

  // A failed card recovery can leave the 400 kHz init clock: off restores it.
  clock_reset(400 * 1000);
  sd_set_slow_mode(false);
  CHECK_EQ_INT(s_baud_writes, 1);
  CHECK(g_fake_spi_baud == SD_SPI_BAUD);
}

// The mutex is recursive: a caller that holds it does not wait for itself.
static void test_slow_mode_with_the_lock_held(void) {
  clock_reset(SD_SPI_BAUD);
  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  sd_set_slow_mode(true);
  recursive_mutex_exit(&g_sdcard_mutex);
  CHECK_EQ_INT(s_baud_writes, 1);
}

int main(void) {
  s_disk = calloc(NSECT, 512);
  s_big = malloc(BIG);
  for (int i = 0; i < BIG; i++)
    s_big[i] = (uint8_t)(i * 7 + (i >> 8));

  test_big_write_and_read_feed();
  test_small_transfers_do_not_feed();
  test_read_file_walks_the_path_once();
  test_read_error_is_minus_one_after_a_partial_read();
  test_full_card_is_a_short_count();
  test_copy_feeds_and_can_stop();
  test_directory_walks_feed();
  test_disk_info_stamps_the_heartbeat();
  test_slow_mode_waits_for_a_read_in_flight();
  test_slow_mode_already_as_asked_leaves_the_block_alone();
  test_slow_mode_with_the_lock_held();

  f_unmount("");
  free(s_disk);
  free(s_big);
  return check_report("test_sdcard_driver");
}
