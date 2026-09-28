// In-memory SD card for host unit tests (see sdcard_fake.h).
#include "sdcard_fake.h"
#include "../../../src/drivers/sdcard.h"
#include "umm_malloc.h"

#include <stdlib.h>
#include <string.h>

#define FAKE_FILES 16
#define FAKE_DIRS  16

typedef struct {
    char   path[160];
    char  *data;
    size_t len;
    bool   used;
} fake_file_t;

typedef struct {
    fake_file_t *file;
    size_t       pos;
    bool         write;
} fake_handle_t;

static fake_file_t s_files[FAKE_FILES];
static char        s_dirs[FAKE_DIRS][160];
static int         s_write_limit = -1;
static bool        s_busy;
static int         s_try_reads;
static int         s_blocking_while_busy;
static int         s_rename_ok_left = -1;
static int         s_renames;

static fake_file_t *lookup(const char *path) {
    for (int i = 0; i < FAKE_FILES; i++)
        if (s_files[i].used && strcmp(s_files[i].path, path) == 0)
            return &s_files[i];
    return NULL;
}

static fake_file_t *create(const char *path) {
    fake_file_t *f = lookup(path);
    if (f)
        return f;
    for (int i = 0; i < FAKE_FILES; i++) {
        if (!s_files[i].used) {
            f = &s_files[i];
            memset(f, 0, sizeof(*f));
            strncpy(f->path, path, sizeof(f->path) - 1);
            f->used = true;
            return f;
        }
    }
    abort();  // table full: grow FAKE_FILES
}

void sdfake_reset(void) {
    for (int i = 0; i < FAKE_FILES; i++) {
        free(s_files[i].data);
        memset(&s_files[i], 0, sizeof(s_files[i]));
    }
    memset(s_dirs, 0, sizeof(s_dirs));
    s_write_limit = -1;
    s_busy = false;
    s_try_reads = 0;
    s_blocking_while_busy = 0;
    s_rename_ok_left = -1;
    s_renames = 0;
}

void sdfake_fail_rename_after(int n_ok) { s_rename_ok_left = n_ok; }
int sdfake_renames(void) { return s_renames; }

void sdfake_put(const char *path, const char *data, size_t len) {
    fake_file_t *f = create(path);
    free(f->data);
    f->data = malloc(len + 1);
    memcpy(f->data, data, len);
    f->data[len] = '\0';
    f->len = len;
}

const char *sdfake_get(const char *path, size_t *len) {
    fake_file_t *f = lookup(path);
    if (!f)
        return NULL;
    if (len)
        *len = f->len;
    return f->data ? f->data : "";
}

bool sdfake_dir_exists(const char *path) {
    for (int i = 0; i < FAKE_DIRS; i++)
        if (s_dirs[i][0] && strcmp(s_dirs[i], path) == 0)
            return true;
    return false;
}

void sdfake_limit_writes(int limit) { s_write_limit = limit; }

// ── sdcard.h subset ──────────────────────────────────────────────────────────

char *sdcard_read_file(const char *path, int *out_len) {
    fake_file_t *f = lookup(path);
    if (!f)
        return NULL;
    char *b = umm_malloc(f->len + 1);
    memcpy(b, f->data ? f->data : "", f->len);
    b[f->len] = '\0';
    if (out_len)
        *out_len = (int)f->len;
    return b;
}

sdfile_t sdcard_fopen(const char *path, const char *mode) {
    bool write = strchr(mode, 'w') != NULL;
    fake_file_t *f = write ? create(path) : lookup(path);
    if (!f)
        return NULL;
    if (write) {
        free(f->data);
        f->data = NULL;
        f->len = 0;
    }
    fake_handle_t *h = calloc(1, sizeof(*h));
    h->file = f;
    h->write = write;
    return h;
}

int sdcard_fread(sdfile_t fh, void *buf, int len) {
    if (s_busy) s_blocking_while_busy++;
    fake_handle_t *h = fh;
    if (!h || len <= 0)
        return 0;
    size_t avail = h->file->len - h->pos;
    size_t n = (size_t)len < avail ? (size_t)len : avail;
    memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (int)n;
}

void sdfake_set_busy(bool busy) { s_busy = busy; }
int sdfake_try_reads(void) { return s_try_reads; }
int sdfake_blocking_while_busy(void) { return s_blocking_while_busy; }

int sdcard_try_fread_at(sdfile_t fh, uint32_t offset, void *buf, int len) {
    s_try_reads++;
    if (!fh)
        return -1;
    if (s_busy)
        return SDCARD_BUSY;
    if (!sdcard_fseek(fh, offset))
        return -1;
    return len > 0 ? sdcard_fread(fh, buf, len) : 0;
}

int sdcard_fwrite(sdfile_t fh, const void *buf, int len) {
    fake_handle_t *h = fh;
    if (!h || !h->write || len < 0)
        return -1;
    if (s_write_limit >= 0 && len > s_write_limit)
        len = s_write_limit;
    fake_file_t *f = h->file;
    f->data = realloc(f->data, f->len + (size_t)len + 1);
    memcpy(f->data + f->len, buf, (size_t)len);
    f->len += (size_t)len;
    f->data[f->len] = '\0';
    return len;
}

void sdcard_fclose(sdfile_t fh) { free(fh); }

bool sdcard_fseek(sdfile_t fh, uint32_t offset) {
    if (s_busy) s_blocking_while_busy++;
    fake_handle_t *h = fh;
    if (!h || offset > h->file->len)
        return false;
    h->pos = offset;
    return true;
}

uint32_t sdcard_ftell(sdfile_t fh) { return fh ? (uint32_t)((fake_handle_t *)fh)->pos : 0; }

int sdcard_fsize_handle(sdfile_t fh) {
    if (s_busy) s_blocking_while_busy++;
    return fh ? (int)((fake_handle_t *)fh)->file->len : -1;
}

int sdcard_fsize(const char *path) {
    fake_file_t *f = lookup(path);
    return f ? (int)f->len : -1;
}

bool sdcard_fexists(const char *path) {
    return lookup(path) != NULL || sdfake_dir_exists(path);
}

bool sdcard_mkdir(const char *path) {
    if (sdfake_dir_exists(path))
        return true;
    for (int i = 0; i < FAKE_DIRS; i++) {
        if (!s_dirs[i][0]) {
            strncpy(s_dirs[i], path, sizeof(s_dirs[i]) - 1);
            return true;
        }
    }
    return false;
}

bool sdcard_delete(const char *path) {
    fake_file_t *f = lookup(path);
    if (!f)
        return false;
    free(f->data);
    memset(f, 0, sizeof(*f));
    return true;
}

// FatFS f_rename: FR_EXIST when dst exists, FR_NO_FILE when src is missing.
bool sdcard_rename(const char *src, const char *dst) {
    fake_file_t *f = lookup(src);
    if (!f || lookup(dst))
        return false;
    if (s_rename_ok_left == 0) {
        s_rename_ok_left = -1;  // one failure, then renames work again
        return false;
    }
    if (s_rename_ok_left > 0)
        s_rename_ok_left--;
    memset(f->path, 0, sizeof(f->path));
    strncpy(f->path, dst, sizeof(f->path) - 1);
    s_renames++;
    return true;
}
