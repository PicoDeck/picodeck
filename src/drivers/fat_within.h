#pragma once

// FatFS directory identity, for the dev `mv` command. No SD lock, no SDK:
// sdcard_path_within() (sdcard.c) takes the lock and calls this, and
// tests/unit/test_mv_op.c runs it on a RAM disk.

#include <stdbool.h>

// True when `root` is a directory and `path` is `root` or lies below it,
// decided by FatFS identity (the directory's first cluster) at `root` and at
// each ancestor of `path` that exists: whatever the spelling (case, "//",
// "\", ".", an 8.3 alias), it is the same directory or it is not. False when
// `root` is not a directory (a file has no subtree) or `path` never reaches
// it.
bool fat_path_within(const char *root, const char *path);
