#include "sdcard.h"
#include "../hardware.h"

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "ff.h"
#include "diskio.h"
#include "fat_within.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── FatFS globals ─────────────────────────────────────────────────────────────

static FATFS s_fs;
static bool  s_mounted = false;

// Protects all FatFS operations from concurrent access across cores.
// Recursive, so a nested sdcard_* call from a holder cannot deadlock. Callbacks
// (list_dir entries, copy progress) run with it released.
recursive_mutex_t g_sdcard_mutex;

// ── Filesystem corruption logging ────────────────────────────────────────────

#define FS_LOG_PATH "/system/filesystem.log"

// Returns true for hardware/structural FatFS errors that indicate corruption
// or media failure, as opposed to benign errors like FR_NO_FILE.
static bool is_fs_corruption(FRESULT res) {
    return res == FR_DISK_ERR      // (1) Low-level I/O error
        || res == FR_INT_ERR       // (2) FatFS assertion failure
        || res == FR_NOT_READY     // (3) Physical drive not ready
        || res == FR_NO_FILESYSTEM;// (13) No valid FAT volume found
}

static const char *fresult_name(FRESULT res) {
    switch (res) {
        case FR_DISK_ERR:      return "DISK_ERR";
        case FR_INT_ERR:       return "INT_ERR";
        case FR_NOT_READY:     return "NOT_READY";
        case FR_NO_FILESYSTEM: return "NO_FILESYSTEM";
        default:               return "FS_ERROR";
    }
}

// Appends one line to /system/filesystem.log and mirrors it to UART.
// op:   short name of the sdcard_* function that failed (e.g. "sdcard_fopen")
// path: file/dir path involved, or NULL for mount-level errors
//
// Rate-limited: during sustained failures (e.g. SD card reset during download),
// disk_write fails on every chunk, producing hundreds of identical messages.
// After 3 errors, UART output is throttled to once per second and filesystem
// log writes are skipped entirely (the card is likely still broken).
static bool s_log_busy = false; // recursion guard
static uint32_t s_corruption_count = 0;
static uint32_t s_last_corruption_log_ms = 0;

static void sdcard_log_corruption(FRESULT res, const char *op, const char *path) {
    s_corruption_count++;
    uint32_t now_ms = (uint32_t)(time_us_64() / 1000);

    // Rate-limit UART output during sustained failures
    if (s_corruption_count > 3 && (now_ms - s_last_corruption_log_ms) < 1000)
        return;
    s_last_corruption_log_ms = now_ms;

    char buf[256];
    if (path)
        snprintf(buf, sizeof(buf), "[%8lums] %s(%d) on %s: %s\n",
                 (unsigned long)now_ms,
                 fresult_name(res), (int)res, op, path);
    else
        snprintf(buf, sizeof(buf), "[%8lums] %s(%d) on %s\n",
                 (unsigned long)now_ms,
                 fresult_name(res), (int)res, op);

    if (s_corruption_count > 3)
        printf("[SD] corruption (x%lu): %s", (unsigned long)s_corruption_count, buf);
    else
        printf("[SD] corruption: %s", buf);

    // Skip filesystem log write during sustained failures — card is likely broken
    if (s_corruption_count > 3) return;

    if (!s_mounted) return;

    // Open log in append mode; if the card is too corrupt to write, just skip.
    FIL *f = (FIL *)umm_malloc(sizeof(FIL));
    if (f) {
        recursive_mutex_enter_blocking(&g_sdcard_mutex);
        if (s_log_busy) {
            recursive_mutex_exit(&g_sdcard_mutex);
            umm_free(f);
            return;
        }
        s_log_busy = true;
        if (f_open(f, FS_LOG_PATH, FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK) {
            UINT bw = 0;
            f_write(f, buf, strlen(buf), &bw);
            f_close(f);
        }
        s_log_busy = false;
        recursive_mutex_exit(&g_sdcard_mutex);
        umm_free(f);
    }
}

// Reset corruption counter — call after successful SD operation to re-enable logging
void sdcard_reset_corruption_count(void) {
    s_corruption_count = 0;
}

// ── SPI SD card low-level (bit-bang layer for FatFS diskio) ──────────────────
// FatFS calls the diskio_ functions. Those in turn call these SPI helpers.
// The actual diskio.c implementation is in third_party/fatfs/port/diskio.c
// which you configure to use SPI0 with the pins below.
// These are exposed here so diskio.c can reference the hardware configuration.

const uint s_sd_spi_baud = SD_SPI_BAUD;
const uint s_sd_pin_cs   = SD_PIN_CS;
const uint s_sd_pin_miso = SD_PIN_MISO;
const uint s_sd_pin_mosi = SD_PIN_MOSI;
const uint s_sd_pin_sck  = SD_PIN_SCK;

bool sdcard_init(void) {
    recursive_mutex_init(&g_sdcard_mutex);

    // SPI0 hardware setup
    spi_init(SD_SPI_PORT, 400 * 1000);   // 400 kHz for card init
    spi_set_format(SD_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO);   // Hold MISO high when card tristates the line

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);   // deselect

    // Give the card ~80 clock cycles to power up
    sleep_ms(10);

    FRESULT res = f_mount(&s_fs, "", 1);
    if (res == FR_OK) {
        s_mounted = true;

        // Ensure standard directory structure exists
        f_mkdir("/apps");
        f_mkdir("/data");
        f_mkdir("/system");
    } else if (is_fs_corruption(res)) {
        sdcard_log_corruption(res, "sdcard_init", NULL);
    }

    return s_mounted;
}

bool sdcard_is_mounted(void) {
    return s_mounted;
}

bool sdcard_ensure_ready(void) {
    if (!s_mounted) return false;

    recursive_mutex_enter_blocking(&g_sdcard_mutex);

    /* Probe the card with a lightweight operation (stat the root).
     * If this fails, the card went unresponsive — remount to trigger
     * the full recovery sequence in disk_initialize(). */
    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    bool ok = false;
    if (fi) {
        ok = (f_stat("/system", fi) == FR_OK);
        umm_free(fi);
    }

    if (!ok) {
        printf("[SD] Card not responding, attempting recovery...\n");
        f_unmount("");
        s_mounted = false;
        sleep_ms(50);
        if (f_mount(&s_fs, "", 1) == FR_OK) {
            s_mounted = true;
            ok = true;
            printf("[SD] Card recovered via remount\n");
        } else {
            printf("[SD] Recovery failed — card may need physical reset\n");
        }
    }

    recursive_mutex_exit(&g_sdcard_mutex);
    return ok;
}

void sdcard_apply_clock(void) {
    spi_set_baudrate(SD_SPI_PORT, SD_SPI_BAUD);
}

bool sdcard_remount(void) {
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    f_unmount("");
    s_mounted = false;
    sleep_ms(10);  // Brief delay for card to stabilize after unmount
    FRESULT res = f_mount(&s_fs, "", 1);
    s_mounted = (res == FR_OK);
    printf("[SD] remount: %s (FRESULT=%d)\n", s_mounted ? "OK" : "FAILED", res);
    recursive_mutex_exit(&g_sdcard_mutex);
    if (!s_mounted && is_fs_corruption(res))
        sdcard_log_corruption(res, "sdcard_remount", NULL);
    return s_mounted;
}

// ── FatFS mode string → FATFS flags ──────────────────────────────────────────

static BYTE mode_to_fatfs(const char *mode) {
    BYTE m = 0;
    bool has_r = false, has_w = false, has_a = false;
    for (const char *p = mode; *p; p++) {
        if (*p == 'r') has_r = true;
        if (*p == 'w') has_w = true;
        if (*p == 'a') has_a = true;
    }
    if (has_r && !has_w) m = FA_READ | FA_OPEN_EXISTING;
    if (has_w)           m = FA_WRITE | FA_CREATE_ALWAYS;
    if (has_a)           m = FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS;
    if (has_r && has_w)  m = FA_READ | FA_WRITE | FA_OPEN_ALWAYS;
    return m;
}

// ── File I/O ─────────────────────────────────────────────────────────────────

sdfile_t sdcard_fopen(const char *path, const char *mode) {
    if (!s_mounted) return NULL;
    FIL *f = (FIL *)umm_malloc(sizeof(FIL));
    if (!f) return NULL;

    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    BYTE flags = mode_to_fatfs(mode);
    FRESULT res = f_open(f, path, flags);

    /* If the card went unresponsive (e.g., brownout during WiFi activity),
     * f_open fails with DISK_ERR.  Remount triggers disk_initialize() which
     * runs the full card recovery sequence, then retry the open once. */
    if (res == FR_DISK_ERR || res == FR_NOT_READY) {
        printf("[SD] fopen failed (%d), attempting remount+retry...\n", (int)res);
        f_unmount("");
        s_mounted = false;
        sleep_ms(50);
        if (f_mount(&s_fs, "", 1) == FR_OK) {
            s_mounted = true;
            res = f_open(f, path, flags);
            if (res == FR_OK)
                printf("[SD] fopen succeeded after remount\n");
        } else {
            printf("[SD] remount failed\n");
        }
    }

    recursive_mutex_exit(&g_sdcard_mutex);
    if (res != FR_OK) {
        if (is_fs_corruption(res)) sdcard_log_corruption(res, "sdcard_fopen", path);
        umm_free(f);
        return NULL;
    }
    return (sdfile_t)f;
}

int sdcard_fread(sdfile_t f, void *buf, int len) {
    if (!f) return -1;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    UINT br = 0;
    FRESULT res = f_read((FIL *)f, buf, (UINT)len, &br);
    recursive_mutex_exit(&g_sdcard_mutex);
    if (res != FR_OK && is_fs_corruption(res))
        sdcard_log_corruption(res, "sdcard_fread", NULL);
    return (res == FR_OK) ? (int)br : -1;
}

int sdcard_try_fread_at(sdfile_t f, uint32_t offset, void *buf, int len) {
    if (!f) return -1;
    if (!recursive_mutex_try_enter(&g_sdcard_mutex, NULL))
        return SDCARD_BUSY;
    UINT br = 0;
    FRESULT res = FR_OK;
    if ((uint32_t)f_tell((FIL *)f) != offset)
        res = f_lseek((FIL *)f, (FSIZE_t)offset);
    if (res == FR_OK && len > 0)
        res = f_read((FIL *)f, buf, (UINT)len, &br);
    recursive_mutex_exit(&g_sdcard_mutex);
    return (res == FR_OK) ? (int)br : -1;
}

int sdcard_fwrite(sdfile_t f, const void *buf, int len) {
    if (!f) return -1;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    UINT bw = 0;
    FRESULT res = f_write((FIL *)f, buf, (UINT)len, &bw);
    recursive_mutex_exit(&g_sdcard_mutex);
    if (res == FR_OK) {
        if (s_corruption_count > 0) s_corruption_count = 0;
    } else if (is_fs_corruption(res)) {
        sdcard_log_corruption(res, "sdcard_fwrite", NULL);
    }
    return (res == FR_OK) ? (int)bw : -1;
}

void sdcard_fclose(sdfile_t f) {
    if (!f) return;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    f_sync((FIL *)f);   // Flush dirty sectors to SD before close
    f_close((FIL *)f);
    recursive_mutex_exit(&g_sdcard_mutex);
    umm_free(f);  // Must match sdcard_fopen() which allocates via umm_malloc
}

bool sdcard_fseek(sdfile_t f, uint32_t offset) {
    if (!f) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    bool ok = f_lseek((FIL *)f, (FSIZE_t)offset) == FR_OK;
    recursive_mutex_exit(&g_sdcard_mutex);
    return ok;
}

uint32_t sdcard_ftell(sdfile_t f) {
    if (!f) return 0;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    uint32_t pos = (uint32_t)f_tell((FIL *)f);
    recursive_mutex_exit(&g_sdcard_mutex);
    return pos;
}

bool sdcard_fexists(const char *path) {
    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    if (!fi) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    bool exists = (f_stat(path, fi) == FR_OK);
    recursive_mutex_exit(&g_sdcard_mutex);
    umm_free(fi);
    return exists;
}

int sdcard_fsize(const char *path) {
    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    if (!fi) return -1;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    FRESULT res = f_stat(path, fi);
    recursive_mutex_exit(&g_sdcard_mutex);
    if (res != FR_OK) {
        umm_free(fi);
        return -1;
    }
    int size = (int)fi->fsize;
    umm_free(fi);
    return size;
}

int sdcard_fsize_handle(sdfile_t f) {
    if (!f) return -1;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    int size = (int)f_size((FIL *)f);
    recursive_mutex_exit(&g_sdcard_mutex);
    return size;
}

bool sdcard_mkdir(const char *path) {
    if (!s_mounted) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    // f_mkdir returns FR_OK if successful or FR_EXIST if already exists
    FRESULT res = f_mkdir(path);
    if (res == FR_OK)
        disk_ioctl(0, CTRL_SYNC, NULL);  // Flush directory entry to SD
    recursive_mutex_exit(&g_sdcard_mutex);
    return (res == FR_OK || res == FR_EXIST);
}

// Directory entries are read into a packed PSRAM snapshot under the SD lock,
// then the lock is released (and the directory closed) before any callback
// runs. Callbacks used to run with the recursive mutex held: a slow one (a
// Lua listDir building tables, a recursive delete) kept Core 1's audio
// try-locks failing for the whole walk, and one that raised a Lua error
// longjmp'd out with the mutex held and the DIR open for good.
typedef struct {
    uint32_t size;
    uint16_t fdate, ftime;
    uint16_t name_len;
    uint8_t  is_dir;
    uint8_t  pad;
    // name bytes + '\0' follow, then padding to a 4-byte boundary
} list_rec_t;

#define LIST_REC_ALIGN(n) (((n) + 3u) & ~3u)

int sdcard_list_dir(const char *path,
                    void (*callback)(const sdcard_entry_t *entry, void *user),
                    void *user) {
    if (!s_mounted) return -1;

    DIR *dir = (DIR *)umm_malloc(sizeof(DIR));
    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    size_t cap = 2048, used = 0;
    uint8_t *snap = (uint8_t *)umm_malloc(cap);
    if (!dir || !fi || !snap) {
        if (dir) umm_free(dir);
        if (fi) umm_free(fi);
        if (snap) umm_free(snap);
        return -1;
    }

    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    FRESULT open_res = f_opendir(dir, path);
    if (open_res != FR_OK) {
        recursive_mutex_exit(&g_sdcard_mutex);
        umm_free(dir);
        umm_free(fi);
        umm_free(snap);
        if (is_fs_corruption(open_res))
            sdcard_log_corruption(open_res, "sdcard_list_dir(opendir)", path);
        return -1;
    }

    int count = 0;
    bool oom = false;
    FRESULT rd_res;
    while ((rd_res = f_readdir(dir, fi)) == FR_OK && fi->fname[0]) {
        size_t name_len = strnlen(fi->fname, sizeof(((sdcard_entry_t *)0)->name) - 1);
        size_t rec = LIST_REC_ALIGN(sizeof(list_rec_t) + name_len + 1);
        if (used + rec > cap) {
            size_t ncap = cap * 2;
            while (used + rec > ncap) ncap *= 2;
            uint8_t *n = (uint8_t *)umm_realloc(snap, ncap);
            if (!n) { oom = true; break; }
            snap = n;
            cap = ncap;
        }
        list_rec_t *r = (list_rec_t *)(snap + used);
        r->size = fi->fsize;
        r->fdate = fi->fdate;
        r->ftime = fi->ftime;
        r->name_len = (uint16_t)name_len;
        r->is_dir = (fi->fattrib & AM_DIR) != 0;
        r->pad = 0;
        memcpy((char *)(r + 1), fi->fname, name_len);
        ((char *)(r + 1))[name_len] = '\0';
        used += rec;
        count++;
    }
    f_closedir(dir);
    recursive_mutex_exit(&g_sdcard_mutex);
    umm_free(dir);
    umm_free(fi);

    if (oom) {
        umm_free(snap);
        printf("[SD] list_dir %s: out of memory for the listing\n", path);
        return -1;
    }
    if (rd_res != FR_OK && is_fs_corruption(rd_res))
        sdcard_log_corruption(rd_res, "sdcard_list_dir(readdir)", path);

    // Callbacks run unlocked; they may call other sdcard_* functions (which
    // take the lock themselves), including on this directory.
    if (callback) {
        sdcard_entry_t e;
        for (size_t off = 0; off < used;) {
            const list_rec_t *r = (const list_rec_t *)(snap + off);
            memcpy(e.name, (const char *)(r + 1), (size_t)r->name_len + 1);
            e.is_dir = r->is_dir;
            e.size   = r->size;
            e.fdate  = r->fdate;
            e.ftime  = r->ftime;
            callback(&e, user);
            off += LIST_REC_ALIGN(sizeof(list_rec_t) + r->name_len + 1);
        }
    }
    umm_free(snap);
    return count; // partial count after a readdir error, as before
}

bool sdcard_delete(const char *path) {
    if (!s_mounted) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    bool ok = f_unlink(path) == FR_OK;
    if (ok)
        disk_ioctl(0, CTRL_SYNC, NULL);  // Flush FAT/directory update to SD
    recursive_mutex_exit(&g_sdcard_mutex);
    return ok;
}

// Recursively delete a directory and all its contents.
static void delete_recursive_cb(const sdcard_entry_t *entry, void *user) {
    char *parent = (char *)user;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", parent, entry->name);
    if (entry->is_dir) {
        sdcard_delete_recursive(path);
    } else {
        sdcard_delete(path);
    }
}

bool sdcard_delete_recursive(const char *path) {
    if (!s_mounted) return false;

    sdcard_stat_t st;
    if (!sdcard_stat(path, &st)) return false;

    if (!st.is_dir) {
        return sdcard_delete(path);
    }

    // Delete contents first, then the empty directory
    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    sdcard_list_dir(path, delete_recursive_cb, path_copy);
    return sdcard_delete(path);
}

sdcard_within_t sdcard_path_within(const char *root, const char *path) {
    if (!s_mounted) return SDCARD_WITHIN_UNKNOWN;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    fat_within_t within = fat_path_within(root, path);
    recursive_mutex_exit(&g_sdcard_mutex);
    return (sdcard_within_t)within;
}

bool sdcard_rename(const char *src, const char *dst) {
    if (!s_mounted) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    bool ok = f_rename(src, dst) == FR_OK;
    if (ok)
        disk_ioctl(0, CTRL_SYNC, NULL);  // Flush FAT/directory update to SD
    recursive_mutex_exit(&g_sdcard_mutex);
    return ok;
}

#define COPY_CHUNK 4096

// The SD lock is held per FatFS step (open, each chunk's read+write, close),
// never across the progress callback: a Lua progress callback used to run
// with the recursive mutex held, so Core 1's audio try-locks failed for the
// whole copy, and a callback that slept (or showed a dialog) stalled it
// further. Between chunks other code may use the card; FatFS's file lock
// (FF_FS_LOCK) keeps anyone from opening dst for writing meanwhile.
bool sdcard_copy(const char *src, const char *dst,
                 void (*progress_cb)(uint32_t done, uint32_t total, void *user),
                 void *user) {
    if (!s_mounted) return false;

    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    FIL *fsrc = (FIL *)umm_malloc(sizeof(FIL));
    FIL *fdst = (FIL *)umm_malloc(sizeof(FIL));
    uint8_t *buf = (uint8_t *)umm_malloc(COPY_CHUNK);
    bool ok = fi && fsrc && fdst && buf;
    bool src_open = false, dst_open = false;
    uint32_t total = 0;

    if (ok) {
        recursive_mutex_enter_blocking(&g_sdcard_mutex);
        total = (f_stat(src, fi) == FR_OK) ? fi->fsize : 0;
        src_open = f_open(fsrc, src, FA_READ | FA_OPEN_EXISTING) == FR_OK;
        if (src_open)
            dst_open = f_open(fdst, dst, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
        recursive_mutex_exit(&g_sdcard_mutex);
        ok = src_open && dst_open;
    }

    uint32_t done = 0;
    while (ok) {
        UINT br = 0, bw = 0;
        recursive_mutex_enter_blocking(&g_sdcard_mutex);
        if (f_read(fsrc, buf, COPY_CHUNK, &br) != FR_OK)
            ok = false;
        else if (br > 0 && (f_write(fdst, buf, br, &bw) != FR_OK || bw != br))
            ok = false;
        recursive_mutex_exit(&g_sdcard_mutex);
        if (!ok || br == 0)
            break; // error or EOF
        done += br;
        if (progress_cb)
            progress_cb(done, total, user);
    }

    if (src_open || dst_open) {
        recursive_mutex_enter_blocking(&g_sdcard_mutex);
        if (src_open)
            f_close(fsrc);
        if (dst_open) {
            if (ok)
                f_sync(fdst);  // Flush destination data + directory entry before close
            f_close(fdst);
            if (!ok)
                f_unlink(dst); // remove partial destination on error
        }
        recursive_mutex_exit(&g_sdcard_mutex);
    }
    if (fi) umm_free(fi);
    if (fsrc) umm_free(fsrc);
    if (fdst) umm_free(fdst);
    if (buf) umm_free(buf);
    return ok;
}

bool sdcard_stat(const char *path, sdcard_stat_t *out) {
    if (!out) return false;
    FILINFO *fi = (FILINFO *)umm_malloc(sizeof(FILINFO));
    if (!fi) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    FRESULT res = f_stat(path, fi);
    recursive_mutex_exit(&g_sdcard_mutex);
    if (res != FR_OK) {
        umm_free(fi);
        return false;
    }
    out->size = fi->fsize;
    out->is_dir = (fi->fattrib & AM_DIR) != 0;
    out->fdate = fi->fdate;
    out->ftime = fi->ftime;
    umm_free(fi);
    return true;
}

bool sdcard_disk_info(uint32_t *out_free_kb, uint32_t *out_total_kb) {
    if (!s_mounted) return false;
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    FATFS *fs;
    DWORD free_clust;
    if (f_getfree("", &free_clust, &fs) != FR_OK) {
        recursive_mutex_exit(&g_sdcard_mutex);
        return false;
    }
    // sectors/cluster * 512 bytes/sector / 1024 bytes/KB = sectors/cluster / 2
    uint32_t spc = fs->csize;
    if (out_total_kb) *out_total_kb = (uint32_t)((uint64_t)(fs->n_fatent - 2) * spc / 2);
    if (out_free_kb)  *out_free_kb  = (uint32_t)((uint64_t)free_clust * spc / 2);
    recursive_mutex_exit(&g_sdcard_mutex);
    return true;
}

char *sdcard_read_file(const char *path, int *out_len) {
    int size = sdcard_fsize(path);
    if (size < 0) return NULL;

    char *buf = (char *)umm_malloc(size + 1);
    if (!buf) return NULL;

    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) { umm_free(buf); return NULL; }

    int n = sdcard_fread(f, buf, size);
    sdcard_fclose(f);

    if (n < 0) { umm_free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}
