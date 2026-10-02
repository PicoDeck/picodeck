#pragma once

// Path hygiene for the dev `mv` command (mv_op.c). Pure: host-tested in
// tests/unit/test_mv_op.c.

#include <stdbool.h>
#include <stddef.h>

// FatFS reads far more than the spelling it is given: '\' is a separator,
// "//" collapses, "." and ".." are components (FF_FS_RPATH = 2), a trailing
// dot or space on a name is stripped, and a long name also matches its 8.3
// alias. So a string comparison of two paths proves nothing; mv_op.c decides
// "is this the same directory" by FatFS identity (sdcard_path_within), after
// this function has cut the spellings down to one plain form.
//
// Writes the canonical form of `in` to `out`: '\' mapped to '/', repeated
// and trailing '/' removed, a single leading '/'. Returns false (out
// unspecified) when `in` is not absolute, is longer than `n`, has a control
// character or one of "*:<>?|, or has a component that is "." or "..", or
// ends in '.' or ' ' (they alias another name), or when it is just "/".
bool mv_path_canon(const char *in, char *out, size_t n);

// True for "/", "/apps", "/data" and "/system" (case-insensitive) in
// canonical form: the top-level directories `mv` never moves, or moves onto.
bool mv_path_is_root_dir(const char *canon);
