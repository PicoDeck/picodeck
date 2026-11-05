// HAL SD Card - Host Filesystem Implementation
// Maps SD card operations to host filesystem

#include "hal_sdcard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include "os/sim_hooks.h"

char g_base_path[512] = ".";
static int g_initialized = 0;

bool hal_sdcard_init(const char* base_path) {
    if (base_path) {
        strncpy(g_base_path, base_path, sizeof(g_base_path) - 1);
        g_base_path[sizeof(g_base_path) - 1] = '\0';
    }
    
    // Create standard directories if they don't exist
    char path[1024];
    
    snprintf(path, sizeof(path), "%s/apps", g_base_path);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/data", g_base_path);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/system", g_base_path);
    mkdir(path, 0755);
    
    g_initialized = 1;
    printf("[SDCard] Initialized with base path: %s\n", g_base_path);
    return true;
}

void hal_sdcard_shutdown(void) {
    g_initialized = 0;
    printf("[SDCard] Shutdown\n");
}

// Map an SD path to a host path under g_base_path. `.` and `..` are
// resolved lexically first; a path that would climb above the SD root is
// refused (false, logged as "[SIM] SD escape: <path>" on the err log
// source) so nothing an app does can touch the host outside the SD image.
// Absolute and relative SD paths are both rooted at the SD root.
bool hal_sdcard_resolve(const char* path, char* out, size_t out_size) {
    char norm[1024];
    size_t nlen = 0;
    norm[0] = '\0';
    const char* p = path ? path : "";
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 1 && p[0] == '.') {
            // current directory: nothing to add
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (nlen == 0) {
                fprintf(stderr, "[SIM] SD escape: %s\n", path);
                sim_log_err("[SIM] SD escape: %s", path);
                return false;
            }
            char* slash = strrchr(norm, '/');
            nlen = (size_t)(slash - norm);
            norm[nlen] = '\0';
        } else {
            if (nlen + 1 + len >= sizeof(norm)) return false;
            norm[nlen++] = '/';
            memcpy(norm + nlen, p, len);
            nlen += len;
            norm[nlen] = '\0';
        }
        p += len;
    }
    int n = snprintf(out, out_size, "%s%s", g_base_path, norm);
    return n >= 0 && (size_t)n < out_size;
}

static bool build_path(char* out, size_t out_size, const char* path) {
    return hal_sdcard_resolve(path, out, out_size);
}

// FatFS parity: the firmware builds FatFS with FF_FS_LOCK = 16
// (third_party/fatfs/ffconf.h), so at most 16 files can be open at once and
// f_open fails (FR_TOO_MANY_OPEN_FILES) until one is closed. The host has no
// such limit, which hid leaked handles in the simulator. Counts every open
// handle, the OS's own included, as the firmware does. Atomic: Core 1 (a
// thread here) opens files too.
static atomic_int s_open_count = 0;

int hal_sdcard_open_count(void) {
    return atomic_load(&s_open_count);
}

// Simulate FatFS file operations
void* hal_sdcard_open(const char* path, const char* mode) {
    if (!g_initialized) return NULL;

    char full_path[1024];
    if (!build_path(full_path, sizeof(full_path), path)) return NULL;

    if (atomic_fetch_add(&s_open_count, 1) >= HAL_SDCARD_MAX_OPEN) {
        atomic_fetch_sub(&s_open_count, 1);
        fprintf(stderr, "[SIM] too many open files (FF_FS_LOCK=%d): %s\n",
                HAL_SDCARD_MAX_OPEN, path);
        return NULL;
    }

    // FatFS parity: f_open() on a directory fails on hardware, but host
    // fopen(dir, "r") succeeds on Linux. Apps distinguish files from
    // directories by exactly this difference (C-Dogs' stat() shim treats
    // "openable" as a regular file), so without this check every
    // subdirectory classifies as a file and directory walks never recurse
    // — C-Dogs silently loaded ~90 of its 1683 sprites in the simulator
    // while loading all of them on hardware.
    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        atomic_fetch_sub(&s_open_count, 1);
        return NULL;
    }
    // FatFS parity (mode_to_fatfs in src/drivers/sdcard.c): see hal_sdcard.h.
    bool has_r = strchr(mode, 'r') != NULL;
    bool has_w = strchr(mode, 'w') != NULL;
    bool has_a = strchr(mode, 'a') != NULL;
    bool can_read = has_r && (!has_a || has_w);
    bool can_write = has_w || has_a;
    bool truncate = has_w && !has_r;
    bool create = has_w || has_a;
    bool exists = access(full_path, F_OK) == 0;
    FILE *fp = NULL;
    if (truncate || (create && !exists))
        fp = fopen(full_path, "w+b");
    else if (exists)
        fp = fopen(full_path, can_write ? "r+b" : "rb");
    if (!fp) {
        atomic_fetch_sub(&s_open_count, 1);
        return NULL;
    }
    if (has_a && !has_w)
        fseek(fp, 0, SEEK_END);
    hal_sdfile_t *h = malloc(sizeof(*h));
    if (!h) {
        fclose(fp);
        atomic_fetch_sub(&s_open_count, 1);
        return NULL;
    }
    h->fp = fp;
    h->can_read = can_read;
    h->can_write = can_write;
    h->last_was_read = false;
    h->last_was_write = false;
    return h;
}

FILE *hal_sdcard_stream(void* handle) {
    return handle ? ((hal_sdfile_t *)handle)->fp : NULL;
}

void hal_sdcard_close(void* handle) {
    if (handle) {
        fclose(hal_sdcard_stream(handle));
        free(handle);
        atomic_fetch_sub(&s_open_count, 1);
    }
}

int hal_sdcard_read_checked(void* handle, void* buf, size_t len) {
    hal_sdfile_t *h = handle;
    if (!h || !h->can_read) return -1;
    if (h->last_was_write) fseek(h->fp, 0, SEEK_CUR);
    h->last_was_write = false;
    h->last_was_read = true;
    size_t n = fread(buf, 1, len, h->fp);
    return ferror(h->fp) ? -1 : (int)n;
}

int hal_sdcard_write_checked(void* handle, const void* buf, size_t len) {
    hal_sdfile_t *h = handle;
    if (!h || !h->can_write) return -1;
    if (h->last_was_read) fseek(h->fp, 0, SEEK_CUR);
    h->last_was_read = false;
    h->last_was_write = true;
    return (int)fwrite(buf, 1, len, h->fp);
}

size_t hal_sdcard_read(void* handle, void* buf, size_t len) {
    int n = hal_sdcard_read_checked(handle, buf, len);
    return n > 0 ? (size_t)n : 0;
}

size_t hal_sdcard_write(void* handle, const void* buf, size_t len) {
    int n = hal_sdcard_write_checked(handle, buf, len);
    return n > 0 ? (size_t)n : 0;
}

int hal_sdcard_seek(void* handle, long offset) {
    hal_sdfile_t *h = handle;
    if (!h) return -1;
    h->last_was_read = false;
    h->last_was_write = false;
    return fseek(h->fp, offset, SEEK_SET);
}

long hal_sdcard_tell(void* handle) {
    if (!handle) return -1;
    return ftell(hal_sdcard_stream(handle));
}

int hal_sdcard_exists(const char* path) {
    if (!g_initialized) return 0;
    
    char full_path[1024];
    if (!build_path(full_path, sizeof(full_path), path)) return 0;
    
    struct stat st;
    return stat(full_path, &st) == 0;
}

int hal_sdcard_size(const char* path) {
    if (!g_initialized) return -1;
    
    char full_path[1024];
    if (!build_path(full_path, sizeof(full_path), path)) return -1;
    
    struct stat st;
    if (stat(full_path, &st) != 0) return -1;
    return (int)st.st_size;
}

int hal_sdcard_isdir(const char* path) {
    if (!g_initialized) return 0;
    char full_path[1024];
    if (!build_path(full_path, sizeof(full_path), path)) return 0;
    struct stat st;
    return stat(full_path, &st) == 0 && S_ISDIR(st.st_mode);
}

int hal_sdcard_mkdir(const char* path) {
    if (!g_initialized) return -1;
    
    char full_path[1024];
    if (!build_path(full_path, sizeof(full_path), path)) {
        errno = EACCES;
        return -1;
    }
    
    return mkdir(full_path, 0755);
}
