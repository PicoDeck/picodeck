#include "mv_op.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_identity.h"
#include "drivers/sdcard.h"
#include "mv_path.h"

#define MV_PATH_MAX 192
#define MV_MADE_MAX 8

// Missing parents of dst, created one level at a time (FatFS has no mkdir
// -p). The ones this call made are remembered, so a failed rename can take
// them away again.
typedef struct {
    char path[MV_PATH_MAX];
    size_t made[MV_MADE_MAX];   // offsets of the '/' ending each directory created
    int n_made;
} mv_parents_t;

static bool make_parents(mv_parents_t *mp, const char *dst) {
    snprintf(mp->path, sizeof(mp->path), "%s", dst);
    mp->n_made = 0;
    for (char *c = mp->path + 1; (c = strchr(c, '/')) != NULL; c++) {
        *c = '\0';
        sdcard_stat_t st;
        bool existed = sdcard_stat(mp->path, &st);
        // Deeper than the record can hold: refuse rather than leave
        // directories a failed rename could not take away again.
        bool ok = existed ? st.is_dir
                          : (mp->n_made < MV_MADE_MAX && sdcard_mkdir(mp->path));
        if (ok && !existed)
            mp->made[mp->n_made++] = (size_t)(c - mp->path);
        *c = '/';
        if (!ok)
            return false;
    }
    return true;
}

static void unmake_parents(mv_parents_t *mp) {
    for (int i = mp->n_made - 1; i >= 0; i--) {
        mp->path[mp->made[i]] = '\0';
        sdcard_delete(mp->path);   // empty by construction: fails if not
    }
}

// `p` is `root` or below it, as canonical strings (case-insensitive, whole
// components). The second guard: it needs no FatFS call, so it holds when
// the identity test cannot run.
static bool str_within(const char *root, const char *p) {
    size_t n = strlen(root);
    return strncasecmp(p, root, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

#define VERIFY_MSG "Error: mv: cannot verify the move (too many open files?): " \
                   "close files and retry"

bool mv_op(char *args, char *reply, size_t n) {
    char *a = args;
    char *b = strchr(a, ' ');
    if (!b || a[0] != '/' || b[1] != '/') {
        snprintf(reply, n, "Usage: mv /src /dst");
        return false;
    }
    *b++ = '\0';
    char src[MV_PATH_MAX], dst[MV_PATH_MAX];
    if (strchr(b, ' ') || !mv_path_canon(a, src, sizeof(src)) ||
        !mv_path_canon(b, dst, sizeof(dst))) {
        snprintf(reply, n, "Error: mv: bad path (absolute, no spaces, no . or "
                           ".. component, no name ending in . or space)");
        return false;
    }
    if (mv_path_is_root_dir(src) || mv_path_is_root_dir(dst)) {
        snprintf(reply, n, "Error: mv: not the top-level directories (/, "
                           "/apps, /data, /system)");
        return false;
    }
    // /system, the directory and everything below it, either side: by
    // spelling (a host filesystem that tells case apart) and by FatFS
    // identity (any spelling: "//system", an 8.3 alias; a nonexistent target
    // below it is caught by its ancestors). An identity test that could not
    // look (the open-file table is full, FatFS then fails to open a directory
    // that exists while f_rename still works) is a refusal, not a "no".
    if (str_within("/system", src) || str_within("/system", dst)) {
        snprintf(reply, n, "Error: mv: /system is off limits");
        return false;
    }
    sdcard_within_t w1 = sdcard_path_within("/system", src);
    sdcard_within_t w2 = sdcard_path_within("/system", dst);
    if (w1 == SDCARD_WITHIN_YES || w2 == SDCARD_WITHIN_YES) {
        snprintf(reply, n, "Error: mv: /system is off limits");
        return false;
    }
    if (w1 == SDCARD_WITHIN_UNKNOWN || w2 == SDCARD_WITHIN_UNKNOWN) {
        snprintf(reply, n, VERIFY_MSG);
        return false;
    }
    sdcard_stat_t st, dst_st;
    if (!sdcard_stat(src, &st)) {
        snprintf(reply, n, "Error: mv: no such file or directory: %s", src);
        return false;
    }
    if (sdcard_stat(dst, &dst_st)) {
        // FatFS refuses a rename onto an existing name, case-only included,
        // so a case change needs a detour through a temporary name.
        snprintf(reply, n, "Error: mv: destination exists: %s (a case-only "
                           "rename must go via another name)", dst);
        return false;
    }
    // f_rename does not check for a subtree: a directory moved below
    // itself is cut out of the tree and its clusters are lost.
    if (st.is_dir) {
        sdcard_within_t w = sdcard_path_within(src, dst);
        if (str_within(src, dst) || w == SDCARD_WITHIN_YES) {
            snprintf(reply, n, "Error: mv: cannot move %s into itself", src);
            return false;
        }
        if (w == SDCARD_WITHIN_UNKNOWN) {
            snprintf(reply, n, VERIFY_MSG);
            return false;
        }
    }
    // Moving a directory the running app lives in (or one above it) pulls
    // the app's files out from under it.
    const app_identity_t *me = app_identity_current();
    if (me && me->dir[0]) {
        sdcard_within_t w = sdcard_path_within(src, me->dir);
        if (str_within(src, me->dir) || w == SDCARD_WITHIN_YES) {
            snprintf(reply, n, "Error: mv: %s holds the running app (exit it "
                               "first)", src);
            return false;
        }
        if (w == SDCARD_WITHIN_UNKNOWN) {
            snprintf(reply, n, VERIFY_MSG);
            return false;
        }
    }
    mv_parents_t mp;
    if (!make_parents(&mp, dst)) {
        unmake_parents(&mp);
        snprintf(reply, n, "Error: mv: cannot create the parent of %s", dst);
        return false;
    }
    if (!sdcard_rename(src, dst)) {
        unmake_parents(&mp);
        snprintf(reply, n, "Error: mv failed: %s -> %s", src, dst);
        return false;
    }
    snprintf(reply, n, "Moved: %s -> %s", src, dst);
    return true;
}
