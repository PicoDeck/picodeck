#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pico/mutex.h"

// Global SD card mutex — protects all FatFS operations from concurrent access
// across cores. Recursive so that sdcard_list_dir callbacks can call other
// sdcard_* functions without deadlocking. Initialised in sdcard_init().
extern recursive_mutex_t g_sdcard_mutex;

// =============================================================================
// SD Card / Filesystem Driver
// Uses Chan FatFS (bundled in pico-extras or vendored in third_party/fatfs).
// SPI0 at 10 MHz: MISO=GP16, CS=GP17, SCK=GP18, MOSI=GP19
//
// All paths are relative to the SD card root ("/").
// App directories live at:  /apps/<app_name>/main.lua
//                            /apps/<app_name>/app.json
// Shared config lives at:   /system/config.json
// App save data lives at:   /data/<app_name>/<file>
// =============================================================================

// Returns true if the SD card was found and mounted successfully
bool sdcard_init(void);

// Re-set SPI baud rate after a system clock change (keeps SD at SD_SPI_BAUD)
void sdcard_apply_clock(void);

// Returns true if an SD card is currently mounted
bool sdcard_is_mounted(void);

// Verify the card is responsive; if not, attempt full recovery (bus reset +
// remount).  Call before critical multi-write operations (e.g. OTA update)
// when the card may have gone unresponsive during long WiFi activity.
bool sdcard_ensure_ready(void);

// Remount (e.g. after card swap). Returns true on success.
bool sdcard_remount(void);

// ── File I/O ─────────────────────────────────────────────────────────────────

typedef void *sdfile_t;  // opaque FIL* handle

sdfile_t sdcard_fopen(const char *path, const char *mode);
int      sdcard_fread(sdfile_t f, void *buf, int len);
// Non-blocking positioned read for Core 1's audio pollers: seeks to offset
// and reads, all under one try-lock. Returns SDCARD_BUSY without touching
// the file when the other core holds the SD card, else bytes read (0 at
// EOF) or -1 on error. Never blocks, never logs. Callers track their own
// offset, so Core 1 never needs a (blocking) sdcard_fseek.
#define SDCARD_BUSY (-2)
int      sdcard_try_fread_at(sdfile_t f, uint32_t offset, void *buf, int len);
int      sdcard_fwrite(sdfile_t f, const void *buf, int len);
void     sdcard_fclose(sdfile_t f);
bool     sdcard_fexists(const char *path);
int      sdcard_fsize(const char *path);   // Returns -1 on error
int      sdcard_fsize_handle(sdfile_t f);  // Returns -1 on error
bool     sdcard_fseek(sdfile_t f, uint32_t offset);
uint32_t sdcard_ftell(sdfile_t f);

// Create a directory (and parent directories if needed)
bool     sdcard_mkdir(const char *path);

// ── Directory listing ─────────────────────────────────────────────────────────

typedef struct {
    char     name[256];
    bool     is_dir;
    uint32_t size;
    uint16_t fdate;  // FatFS packed date: bits[15:9]=year-1980, [8:5]=month, [4:0]=day
    uint16_t ftime;  // FatFS packed time: bits[15:11]=hour, [10:5]=min, [4:0]=sec/2
} sdcard_entry_t;

// Calls callback for each entry in path. Returns count of entries, -1 on error.
int sdcard_list_dir(const char *path,
                    void (*callback)(const sdcard_entry_t *entry, void *user),
                    void *user);

// ── File/directory operations ─────────────────────────────────────────────────

// Delete a file or empty directory.
bool sdcard_delete(const char *path);

// Recursively delete a directory and all its contents.
// Returns true if the directory and all contents were deleted.
bool sdcard_delete_recursive(const char *path);

// Is `path` the directory `root` or below it, by FatFS identity (any
// spelling: case, "//", "\", ".", 8.3 aliases)? SDCARD_WITHIN_UNKNOWN when
// FatFS could not look (open-file table full, card absent): treat as "maybe".
typedef enum {
    SDCARD_WITHIN_NO = 0,
    SDCARD_WITHIN_YES = 1,
    SDCARD_WITHIN_UNKNOWN = 2,
} sdcard_within_t;
sdcard_within_t sdcard_path_within(const char *root, const char *path);

// Rename or move a file/directory. Both paths must be on the same volume.
bool sdcard_rename(const char *src, const char *dst);

// Copy src to dst. Optional progress callback: cb(bytes_done, total, user).
// Returns false and removes dst on error.
bool sdcard_copy(const char *src, const char *dst,
                 void (*progress_cb)(uint32_t done, uint32_t total, void *user),
                 void *user);

// ── Metadata ─────────────────────────────────────────────────────────────────

// Detailed file/directory metadata (single path lookup, no directory scan).
typedef struct {
    uint32_t size;     // File size in bytes (0 for directories)
    bool     is_dir;
    uint16_t fdate;    // Same encoding as sdcard_entry_t
    uint16_t ftime;
} sdcard_stat_t;

// Fills *out with metadata for path. Returns false if path not found.
bool sdcard_stat(const char *path, sdcard_stat_t *out);

// Returns free and total space in KB for the mounted volume.
bool sdcard_disk_info(uint32_t *out_free_kb, uint32_t *out_total_kb);

// ── SPI speed control ────────────────────────────────────────────────────────

// Drop SPI0 to 1 MHz (slow=true) for writes during CYW43 radio activity,
// or restore full speed (slow=false). CYW43 EMI corrupts SPI0 at 25 MHz;
// 1 MHz has enough noise margin to be immune.
void sd_set_slow_mode(bool slow);

// ── Convenience: read entire file into a heap-allocated buffer ─────────────-
// Caller must free() the returned pointer. Returns NULL on error.
// *out_len is set to the file size.
char *sdcard_read_file(const char *path, int *out_len);
