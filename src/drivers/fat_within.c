#include "fat_within.h"

#include <string.h>

#include "ff.h"

// Identity of the directory `path` names: its first cluster, and the
// volume's root as 0. False when it is not an existing directory.
static bool dir_id(const char *path, DWORD *id) {
    DIR d;
    if (f_opendir(&d, path) != FR_OK)
        return false;
    *id = (DWORD)d.obj.sclust;
    f_closedir(&d);
    return true;
}

bool fat_path_within(const char *root, const char *path) {
    DWORD root_id;
    if (!dir_id(root, &root_id))
        return false;
    char prefix[256];
    size_t n = strlen(path);
    if (n >= sizeof(prefix))
        return false;
    memcpy(prefix, path, n + 1);
    // Every ancestor of `path`, itself last. f_opendir resolves "//", "\",
    // "." and 8.3 aliases the way every other FatFS call does.
    for (size_t i = 1; i <= n; i++) {
        if (prefix[i] != '/' && prefix[i] != '\\' && prefix[i] != '\0')
            continue;
        char saved = prefix[i];
        prefix[i] = '\0';
        DWORD id;
        bool ok = dir_id(prefix, &id);
        prefix[i] = saved;
        if (!ok)
            return false;  // it does not exist, so nothing below it does
        if (id == root_id && root_id != 0)
            return true;
    }
    return false;
}
