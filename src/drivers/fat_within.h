#pragma once

// FatFS directory identity, for the dev `mv` command. No SD lock, no SDK:
// sdcard_path_within() (sdcard.c) takes the lock and calls this, and
// tests/unit/test_mv_op.c runs it on a RAM disk.

// The answer is three-way, and only the first two are answers. FatFS opens a
// directory through its open-file table (FF_FS_LOCK, 16 entries, shared by
// every open file, an app's and the audio stream's), so with the table full
// f_opendir fails for a directory that exists, while f_rename still works:
// "could not look" must never read as "not inside".
typedef enum {
    FAT_WITHIN_NO = 0,       // `path` is not `root` nor below it
    FAT_WITHIN_YES = 1,      // it is `root`, or below it
    FAT_WITHIN_UNKNOWN = 2,  // FatFS could not say (open-file table full, I/O)
} fat_within_t;

// Is `root` a directory and `path` it or below it, by FatFS identity (the
// directory's first cluster) at `root` and at each ancestor of `path`?
// Whatever the spelling (case, "//", "\", ".", an 8.3 alias) it is the same
// directory or it is not. NO also covers `root` not being a directory (a
// file has no subtree) and a `path` that never reaches it; only FR_NO_FILE /
// FR_NO_PATH on an ancestor means "does not exist".
fat_within_t fat_path_within(const char *root, const char *path);
