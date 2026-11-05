// HAL SD Card - Host Filesystem Implementation

#ifndef HAL_SDCARD_H
#define HAL_SDCARD_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

// Initialize SD card subsystem with base path
bool hal_sdcard_init(const char* base_path);

// Map an SD path to its host path under the SD root. Resolves "." and ".."
// lexically and returns false (logging "[SIM] SD escape") for any path that
// leaves the root. Every host-path join in the simulator must go through it.
bool hal_sdcard_resolve(const char* path, char* out, size_t out_size);

// Shutdown SD card subsystem
void hal_sdcard_shutdown(void);

// File operations (similar to FatFS API)
// A handle is a heap wrapper around the host FILE*, not the FILE* itself, so
// a use after close touches freed memory in instrumented simulator code and an
// ASan build reports it (glibc's own FILE accesses are invisible to ASan).
typedef struct {
    FILE *fp;
    bool can_read;    // the mode's FatFS flags: FA_READ, FA_WRITE
    bool can_write;
    bool last_was_write;  // stdio needs a seek between a write and a read
    bool last_was_read;
} hal_sdfile_t;

// At most HAL_SDCARD_MAX_OPEN handles are open at once (the firmware's
// FF_FS_LOCK); hal_sdcard_open returns NULL beyond that until one is closed.
#define HAL_SDCARD_MAX_OPEN 16
int hal_sdcard_open_count(void);

// Modes mean what src/drivers/sdcard.c's mode_to_fatfs makes them, not what
// fopen would: only the letters r, w and a count. "r" (also "r+") reads, the
// file must exist. "w" (also "w+") writes, truncating or creating. "a" (also
// "a+") writes from the end, creating if missing. "r" with "w" ("rw") does both,
// creating if missing and keeping the contents.
void* hal_sdcard_open(const char* path, const char* mode);
FILE *hal_sdcard_stream(void* handle);
void hal_sdcard_close(void* handle);
// Returns -1 on a handle not open for that direction, as sdcard_fread/fwrite
// return on the device (FatFS FR_DENIED); otherwise the bytes moved.
int hal_sdcard_read_checked(void* handle, void* buf, size_t len);
int hal_sdcard_write_checked(void* handle, const void* buf, size_t len);
size_t hal_sdcard_read(void* handle, void* buf, size_t len);
size_t hal_sdcard_write(void* handle, const void* buf, size_t len);
int hal_sdcard_seek(void* handle, long offset);
long hal_sdcard_tell(void* handle);

// File/directory info
int hal_sdcard_exists(const char* path);
int hal_sdcard_size(const char* path);
int hal_sdcard_isdir(const char* path);
int hal_sdcard_mkdir(const char* path);

#endif // HAL_SDCARD_H
