// In-memory SD card for host unit tests: the subset of src/drivers/sdcard.h
// that config.c / appconfig.c / sound code use, backed by a small file table.
#pragma once

#include <stdbool.h>
#include <stddef.h>

void        sdfake_reset(void);
void        sdfake_put(const char *path, const char *data, size_t len);
// Contents of path (NUL-terminated) or NULL; *len gets its size.
const char *sdfake_get(const char *path, size_t *len);
bool        sdfake_dir_exists(const char *path);
// Make the next sdcard_fwrite calls write at most `limit` bytes (-1 = off).
void        sdfake_limit_writes(int limit);
// While set, sdcard_try_fread_at reports SDCARD_BUSY (the other core holds
// the SD card) and counts the attempt.
void        sdfake_set_busy(bool busy);
// Called at the start of every sdcard_try_fread_at (NULL = none): the audio
// refill interrupt runs during a real SD read.
void        sdfake_set_read_hook(void (*fn)(void));
int         sdfake_try_reads(void);   // sdcard_try_fread_at calls since reset
// Blocking calls (fread, fseek, fsize_handle) made while busy: on the device
// each would wait for the other core's SD mutex. Core 1 must make none.
int         sdfake_blocking_while_busy(void);
// The sdcard_rename call after `n_ok` more successful ones fails, once
// (-1 = never). Renames follow FatFS: refused when dst already exists.
void        sdfake_fail_rename_after(int n_ok);
int         sdfake_renames(void);     // successful sdcard_rename calls since reset
// While set, sdcard_read_file fails (a read error, or no memory for the
// buffer) for files that exist.
void        sdfake_fail_reads(bool fail);
