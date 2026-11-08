// The files the firmware owns on the SD card: see card_files.h. Host-tested in
// tests/unit/test_card_files.c over the in-memory SD fake.
#include "card_files.h"

#include "hardware/watchdog.h"
#include "sd_atomic.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>

#define MARKER_HEADER "picodeck-card-files 1\n"
#define ID_LEN 64

// A regular file's size, or -1 when there is none. FatFS's f_stat succeeds on
// a directory too (size 0): a directory is never the file we want, and never
// ours to delete.
static int file_size(const char *path) {
    sdcard_stat_t st;
    if (!sdcard_stat(path, &st) || st.is_dir)
        return -1;
    return (int)st.size;
}

static void note_failure(card_files_result_t *r, const char *what, const char *path) {
    if (r->failed++ == 0)
        snprintf(r->error, sizeof(r->error), "%s %s", what, path);
}

// A marker that parses: *id (NUL-terminated, in buf) and the paths after it.
static bool marker_parse(char *buf, const char **id, char **paths) {
    size_t hl = strlen(MARKER_HEADER);
    if (strncmp(buf, MARKER_HEADER, hl) != 0 || strncmp(buf + hl, "id ", 3) != 0)
        return false;
    char *p = buf + hl + 3;
    for (int i = 0; i < ID_LEN; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return false;
    if (p[ID_LEN] != '\n')
        return false;
    p[ID_LEN] = '\0';
    *id = p;
    *paths = p + ID_LEN + 1;
    return true;
}

// Inside what the firmware owns, by the marker's word: a root's prefix and no "..".
static bool owned(const card_bundle_t *b, const char *path) {
    if (strstr(path, "..") != NULL)
        return false;
    for (int i = 0; i < b->nroots; i++)
        if (strncmp(path, b->roots[i], strlen(b->roots[i])) == 0)
            return true;
    return false;
}

static bool in_bundle(const card_bundle_t *b, const char *path) {
    for (int i = 0; i < b->count; i++)
        if (strcmp(b->files[i].path, path) == 0)
            return true;
    return false;
}

// Make each directory above path, one level at a time (f_mkdir makes one).
// *made is the last parent made, so files that share it skip the walk.
static bool make_parents(const char *path, char *made, size_t made_size) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n == 0 || n >= made_size)
        return n == 0;
    if (strlen(made) == n && strncmp(made, path, n) == 0)
        return true;
    char dir[CARD_FILES_PATH_MAX + 1];
    if (n >= sizeof(dir))
        return false;
    memcpy(dir, path, n);
    dir[n] = '\0';
    for (size_t i = 1; i <= n; i++) {
        if (dir[i] != '/' && dir[i] != '\0')
            continue;
        char c = dir[i];
        dir[i] = '\0';
        bool ok = sdcard_mkdir(dir);
        dir[i] = c;
        if (!ok)
            return false;
    }
    memcpy(made, dir, n + 1);
    return true;
}

static bool write_marker(const card_bundle_t *b) {
    size_t len = strlen(MARKER_HEADER) + 3 + ID_LEN + 1;
    for (int i = 0; i < b->count; i++)
        len += strlen(b->files[i].path) + 1;
    char *buf = umm_malloc(len + 1);
    if (!buf)
        return false;
    char *p = buf;
    p += sprintf(p, "%sid %s\n", MARKER_HEADER, b->id);
    for (int i = 0; i < b->count; i++)
        p += sprintf(p, "%s\n", b->files[i].path);
    bool ok = sd_atomic_write(CARD_FILES_MARKER, buf, (int)(p - buf));
    umm_free(buf);
    return ok;
}

// The old marker's paths that the new bundle no longer ships.
static void delete_stale(const card_bundle_t *b, char *paths, card_files_result_t *r) {
    for (char *line = paths; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        if (*line && owned(b, line) && !in_bundle(b, line) && file_size(line) >= 0) {
            if (sdcard_delete(line))
                r->deleted++;
            else
                note_failure(r, "delete", line);
            watchdog_update();  // the boot's 10 s watchdog: a failing card is slow
        }
        line = end ? end + 1 : NULL;
    }
}

bool card_files_sync(const card_bundle_t *b, card_files_result_t *r) {
    memset(r, 0, sizeof(*r));

    sd_atomic_recover(CARD_FILES_MARKER);
    char *marker = sdcard_read_file(CARD_FILES_MARKER, NULL);
    const char *old_id = NULL;
    char *old_paths = NULL;
    bool parsed = marker && marker_parse(marker, &old_id, &old_paths);
    bool same = parsed && strcmp(old_id, b->id) == 0;

    char made[CARD_FILES_PATH_MAX + 1] = "";
    for (int i = 0; i < b->count; i++) {
        const card_file_t *f = &b->files[i];
        if (same && file_size(f->path) == (int)f->len)
            continue;
        if (!make_parents(f->path, made, sizeof(made)) ||
            !sd_atomic_write(f->path, (const char *)f->data, (int)f->len)) {
            note_failure(r, "write", f->path);
        } else {
            r->written++;
            r->bytes += f->len;
        }
        // The SD layer feeds only between 64 KB pieces, and no bundled file is
        // that big; a failing card can take seconds per file.
        watchdog_update();
    }

    if (parsed && !same)
        delete_stale(b, old_paths, r);
    if (marker)
        umm_free(marker);

    if (!same && r->failed == 0 && !write_marker(b))
        note_failure(r, "write", CARD_FILES_MARKER);

    if (r->written == 0 && r->deleted == 0 && r->failed == 0)
        printf("[CARD] up to date (bundle %.8s)\n", b->id);
    else
        printf("[CARD] wrote %d files (%lu KB), deleted %d (bundle %.8s)\n", r->written,
               (unsigned long)((r->bytes + 1023) / 1024), r->deleted, b->id);
    if (r->failed)
        printf("[CARD] failed %d: %s\n", r->failed, r->error);
    return r->failed == 0;
}
