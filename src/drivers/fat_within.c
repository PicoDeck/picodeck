#include "fat_within.h"

#include <string.h>

#include "ff.h"

typedef enum { DIR_OK, DIR_ABSENT, DIR_ERROR } dir_res_t;

// Identity of the directory `path` names: its first cluster (0 for a FAT12/16
// root). ABSENT only for "no such name / path"; any other failure is ERROR.
static dir_res_t dir_id(const char *path, DWORD *id) {
    DIR d;
    FRESULT r = f_opendir(&d, path);
    if (r == FR_NO_FILE || r == FR_NO_PATH)
        return DIR_ABSENT;
    if (r != FR_OK)
        return DIR_ERROR;
    *id = (DWORD)d.obj.sclust;
    f_closedir(&d);
    return DIR_OK;
}

fat_within_t fat_path_within(const char *root, const char *path) {
    DWORD root_id = 0;
    dir_res_t r = dir_id(root, &root_id);
    if (r == DIR_ERROR)
        return FAT_WITHIN_UNKNOWN;
    if (r == DIR_ABSENT)
        return FAT_WITHIN_NO;
    char prefix[256];
    size_t n = strlen(path);
    if (n >= sizeof(prefix))
        return FAT_WITHIN_UNKNOWN;
    memcpy(prefix, path, n + 1);
    // Every ancestor of `path`, itself last. f_opendir resolves "//", "\\",
    // "." and 8.3 aliases the way every other FatFS call does.
    for (size_t i = 1; i <= n; i++) {
        if (prefix[i] != '/' && prefix[i] != '\\' && prefix[i] != '\0')
            continue;
        char saved = prefix[i];
        prefix[i] = '\0';
        DWORD id = 0;
        r = dir_id(prefix, &id);
        prefix[i] = saved;
        if (r == DIR_ERROR)
            return FAT_WITHIN_UNKNOWN;
        if (r == DIR_ABSENT)
            return FAT_WITHIN_NO;  // it does not exist, so nothing below it does
        if (id == root_id && root_id != 0)
            return FAT_WITHIN_YES;
    }
    return FAT_WITHIN_NO;
}
