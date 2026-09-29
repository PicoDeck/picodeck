#include "zip_archive.h"

#include <stdio.h>
#include <string.h>

#include "zip_util.h"

#ifndef PICODECK_SIMULATOR
#include "hardware/watchdog.h"
#endif

// Shared g_api.zip / simulator-trampoline entry points. Everything goes
// through the hardened seek-based engine in zip_util.c: the archive streams
// from SD (no whole-file PSRAM copy), entry names are validated against
// traversal, parent directories are created, and entry-count / total-size
// caps are enforced.

static bool zip_archive_progress(int done, int total, const char *name,
                                 void *user) {
    (void)done; (void)total; (void)name; (void)user;
#ifndef PICODECK_SIMULATOR
    // Extraction can outlast the 10s watchdog window and nothing else feeds
    // it while we're in here.
    watchdog_update();
#endif
    return true;
}

bool zip_archive_extract(const char *zip_path, const char *dest_dir) {
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        printf("[ZIP] %s: %s\n", zip_path, err);
        return false;
    }
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest_dir, NULL,
                                     zip_archive_progress, NULL, &result, err);
    zip_reader_close(&zr);
    if (!ok)
        printf("[ZIP] extract %s -> %s failed: %s\n", zip_path, dest_dir, err);
    return ok;
}

int zip_archive_list(const char *zip_path) {
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err))
        return -1;
    int n = zip_reader_num_entries(&zr);
    zip_reader_close(&zr);
    return n;
}

// ── Read-in-place handles ────────────────────────────────────────────────────
// Static pool: a zip_reader_t must not move after open (miniz keeps a back
// pointer), and a fixed pool sidesteps heap ownership questions between the
// firmware, native apps and the simulator. zip_archive_close_all() runs on
// app exit so leaked handles cannot brick the pool.

typedef struct {
    zip_reader_t zr;
    bool         in_use;
} zip_slot_t;

static zip_slot_t s_zip_slots[ZIP_ARCHIVE_MAX_OPEN];

static zip_slot_t *slot_of(pczip_t z) {
    zip_slot_t *s = (zip_slot_t *)z;
    if (s >= s_zip_slots && s < s_zip_slots + ZIP_ARCHIVE_MAX_OPEN && s->in_use)
        return s;
    return NULL;
}

pczip_t zip_archive_open(const char *zip_path) {
    if (!zip_path) return NULL;
    for (int i = 0; i < ZIP_ARCHIVE_MAX_OPEN; i++) {
        if (s_zip_slots[i].in_use) continue;
        char err[ZIP_ERR_MAX];
        if (!zip_reader_open(&s_zip_slots[i].zr, zip_path, err)) {
            printf("[ZIP] open %s: %s\n", zip_path, err);
            return NULL;
        }
        s_zip_slots[i].in_use = true;
        return (pczip_t)&s_zip_slots[i];
    }
    printf("[ZIP] open %s: too many open archives (max %d)\n",
           zip_path, ZIP_ARCHIVE_MAX_OPEN);
    return NULL;
}

void zip_archive_close(pczip_t z) {
    zip_slot_t *s = slot_of(z);
    if (!s) return;
    zip_reader_close(&s->zr);
    s->in_use = false;
}

int zip_archive_num_entries(pczip_t z) {
    zip_slot_t *s = slot_of(z);
    return s ? zip_reader_num_entries(&s->zr) : -1;
}

int zip_archive_locate(pczip_t z, const char *name) {
    zip_slot_t *s = slot_of(z);
    return s ? zip_reader_locate(&s->zr, name) : -1;
}

bool zip_archive_stat_index(pczip_t z, int idx, pczip_stat_t *out) {
    zip_slot_t *s = slot_of(z);
    if (!s || !out) return false;
    zip_entry_info_t info;
    if (!zip_reader_stat_index(&s->zr, idx, &info)) return false;
    // zip_entry_info_t.name is ZIP_MAX_NAME+1 == sizeof(out->name)
    memcpy(out->name, info.name, sizeof(out->name));
    out->size      = info.size;
    out->comp_size = info.comp_size;
    out->is_dir    = info.is_dir;
    return true;
}

int zip_archive_read(pczip_t z, int idx, void *buf, uint32_t buf_cap) {
    zip_slot_t *s = slot_of(z);
    if (!s) return -1;
    // A too-small buffer gets its own code so a caller can tell it from a
    // failed read (both were -1); still negative, so `n < 0` callers stay safe.
    zip_entry_info_t info;
    if (buf && zip_reader_stat_index(&s->zr, idx, &info) &&
        info.size > buf_cap) {
        printf("[ZIP] read idx %d: buffer too small (%u needed)\n", idx,
               (unsigned)info.size);
        return PCZIP_ERR_TOO_SMALL;
    }
    char err[ZIP_ERR_MAX];
    int n = zip_reader_read_to_buf(&s->zr, idx, buf, buf_cap, err);
    if (n < 0) printf("[ZIP] read idx %d: %s\n", idx, err);
    return n;
}

bool zip_archive_extract_entry(pczip_t z, int idx, const char *dest_path) {
    zip_slot_t *s = slot_of(z);
    if (!s || !dest_path) return false;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_extract_entry(&s->zr, idx, dest_path, err)) {
        printf("[ZIP] extractEntry idx %d: %s\n", idx, err);
        return false;
    }
    return true;
}

void zip_archive_close_all(void) {
    for (int i = 0; i < ZIP_ARCHIVE_MAX_OPEN; i++) {
        if (s_zip_slots[i].in_use) {
            zip_reader_close(&s_zip_slots[i].zr);
            s_zip_slots[i].in_use = false;
        }
    }
}
